// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "frame_text.hpp"
#include <portals/primitives.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace portals {
namespace {
bool validGlyphs(PrimitiveGlyphs glyphs) {
  return glyphs == PrimitiveGlyphs::UNICODE || glyphs == PrimitiveGlyphs::ASCII;
}

GraphemeText glyph(std::string_view unicode, std::string_view ascii, WidthPolicy policy, PrimitiveGlyphs mode) {
  auto text = GraphemeText::from(mode == PrimitiveGlyphs::ASCII ? ascii : unicode, policy).value();

  if (text.measure() != 1) {
    return GraphemeText::from(ascii, policy).value();
  }

  return text;
}

using GlyphSources = std::array<std::string_view, 6>;
constexpr GlyphSources ASCII_FRAME{"+", "+", "+", "+", "-", "|"};
constexpr std::size_t MAX_CUSTOM_GLYPH_BYTES = 64;

GlyphSources customSources(const FrameGlyphs &custom) noexcept {
  return {custom.topLeft, custom.topRight, custom.bottomLeft, custom.bottomRight, custom.horizontal, custom.vertical};
}

GlyphSources patternSources(FramePattern pattern) noexcept {
  switch (pattern) {
  case FramePattern::SINGLE:
    return {"\xe2\x94\x8c", "\xe2\x94\x90", "\xe2\x94\x94", "\xe2\x94\x98", "\xe2\x94\x80", "\xe2\x94\x82"};
  case FramePattern::ROUNDED:
    return {"\xe2\x95\xad", "\xe2\x95\xae", "\xe2\x95\xb0", "\xe2\x95\xaf", "\xe2\x94\x80", "\xe2\x94\x82"};
  case FramePattern::DOUBLE:
    return {"\xe2\x95\x94", "\xe2\x95\x97", "\xe2\x95\x9a", "\xe2\x95\x9d", "\xe2\x95\x90", "\xe2\x95\x91"};
  case FramePattern::HEAVY:
    return {"\xe2\x94\x8f", "\xe2\x94\x93", "\xe2\x94\x97", "\xe2\x94\x9b", "\xe2\x94\x81", "\xe2\x94\x83"};
  case FramePattern::ASCII:
    return ASCII_FRAME;
  }

  return {};
}

LinearOptions frameLayout(FrameSides sides) {
  LinearOptions options;

  options.padding = sides.insets();

  return options;
}

ComputedStyle defaults(ControlStyle style) {
  ComputedStyle result;

  result.normal = style.normal;
  result.focused = style.focused;
  result.disabled = style.disabled;

  return result;
}

std::expected<void, TreeError> submitGlyph(PaintContext &context, CellPoint origin, const GraphemeText &text,
                                           CellRect clip, CellStyle style) {
  const RenderCommand command = TextCommand{origin, text, clip, style};

  return context.submit(std::span(&command, 1));
}

struct TitlePlacement {
  std::int32_t start{};
  std::int32_t width{};
};

struct Interval {
  std::int32_t begin{};
  std::int32_t end{};
};

Interval horizontalIntersection(CellRect rect, std::int32_t y, Interval extent) noexcept {
  if (y < rect.origin().y || y >= static_cast<std::int64_t>(rect.origin().y) + rect.size().height) {
    return {};
  }

  const auto begin = std::max(extent.begin, rect.origin().x);
  const auto end = static_cast<std::int32_t>(
    std::min<std::int64_t>(extent.end, static_cast<std::int64_t>(rect.origin().x) + rect.size().width));

  return {begin, std::max(begin, end)};
}

std::array<Interval, 2> horizontalExclusions(std::span<const CellRect> exclusions, std::int32_t y,
                                             Interval extent) noexcept {
  std::array<Interval, 2> result;

  for (std::size_t index = 0; index < exclusions.size(); ++index) {
    result[index] = horizontalIntersection(exclusions[index], y, extent);
  }

  if (result[1].begin < result[0].begin) {
    std::swap(result[0], result[1]);
  }

  return result;
}

TitlePlacement titlePlacement(const GraphemeText &title, const FrameOptions &options, CellSize size,
                              std::span<const CellRect> exclusions) noexcept {
  if (!options.sides.top || size.width <= 0 || size.height <= 0 || title.bytes().empty()) {
    return {};
  }

  const auto start = std::min(options.sides.left ? 1 : 0, size.width);
  const auto end = std::max(start, size.width - (options.sides.right ? 1 : 0));
  const auto excluded = horizontalExclusions(exclusions, 0, {start, end});
  Interval best{start, start};
  auto next = start;

  // At most two track intervals create at most three free runs. Prefer the first largest run.
  for (const auto interval : excluded) {
    const auto stop = std::clamp(interval.begin, next, end);

    if (stop - next > best.end - best.begin) {
      best = {next, stop};
    }

    next = std::clamp(std::max(next, interval.end), start, end);
  }

  if (end - next > best.end - best.begin) {
    best = {next, end};
  }

  const auto available = best.end - best.begin;
  const auto visible = title.clipPrefix(available).cells;
  const auto spare = available - visible;
  const auto offset = options.titleAlignment == FrameTitleAlignment::END      ? spare
                      : options.titleAlignment == FrameTitleAlignment::CENTER ? spare / 2
                                                                              : 0;

  return {best.begin + offset, visible};
}

std::uint64_t excludedHorizontal(std::int32_t y, Interval extent, std::span<const CellRect> exclusions) noexcept {
  const auto intervals = horizontalExclusions(exclusions, y, extent);
  const auto overlap =
    std::max(0, std::min(intervals[0].end, intervals[1].end) - std::max(intervals[0].begin, intervals[1].begin));

  return static_cast<std::uint64_t>(intervals[0].end - intervals[0].begin) +
         static_cast<std::uint64_t>(intervals[1].end - intervals[1].begin) - overlap;
}

std::uint64_t perimeterCells(FrameSides sides, CellSize size, std::span<const CellRect> exclusions) noexcept {
  if (size.width <= 0 || size.height <= 0) {
    return 0;
  }

  const auto horizontalRows = (sides.top ? 1 : 0) + (sides.bottom && (!sides.top || size.height > 1) ? 1 : 0);
  const auto verticalColumns = (sides.left ? 1 : 0) + (sides.right && (!sides.left || size.width > 1) ? 1 : 0);

  auto cells = static_cast<std::uint64_t>(horizontalRows) * size.width +
               static_cast<std::uint64_t>(verticalColumns) * (size.height - horizontalRows);

  if (sides.top) {
    cells -= excludedHorizontal(0, {0, size.width}, exclusions);
  }

  if (sides.bottom && (!sides.top || size.height > 1)) {
    cells -= excludedHorizontal(size.height - 1, {0, size.width}, exclusions);
  }

  std::array<CellRect, 2> transposed;

  for (std::size_t index = 0; index < exclusions.size(); ++index) {
    // Swapping axes preserves validity and makes the same bounded union calculation reusable.
    transposed[index] = *CellRect::from({exclusions[index].origin().y, exclusions[index].origin().x},
                                        {exclusions[index].size().height, exclusions[index].size().width});
  }

  const auto verticalExclusions = std::span(transposed).first(exclusions.size());
  const Interval vertical{sides.top ? 1 : 0, size.height - (sides.bottom ? 1 : 0)};

  if (vertical.end > vertical.begin) {
    if (sides.left) {
      cells -= excludedHorizontal(0, vertical, verticalExclusions);
    }

    if (sides.right && (!sides.left || size.width > 1)) {
      cells -= excludedHorizontal(size.width - 1, vertical, verticalExclusions);
    }
  }

  return cells;
}
} // namespace

namespace detail {
std::expected<PreparedFrameText, TreeError> prepareFrameText(WidthPolicy policy, const FrameOptions &options) {
  const auto valid = Border::validateFrame(options);

  if (!valid) {
    return std::unexpected(valid.error());
  }

  const auto sources = options.customGlyphs                              ? customSources(*options.customGlyphs)
                       : options.border.glyphs == PrimitiveGlyphs::ASCII ? ASCII_FRAME
                                                                         : patternSources(options.pattern);
  auto compile = [&](std::size_t index) -> std::expected<GraphemeText, TreeError> {
    auto text = GraphemeText::from(sources[index], policy);

    if (!text) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    const bool singleCell = text->graphemes().size() == 1 && text->measure() == 1 &&
                            text->graphemes().front().display == TextDisplay::PRINTABLE;

    if (singleCell) {
      return std::move(*text);
    }

    if (options.customGlyphs) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    text = GraphemeText::from(ASCII_FRAME[index], policy);

    return text ? std::expected<GraphemeText, TreeError>(std::move(*text))
                : std::unexpected(TreeError::INVALID_ARGUMENT);
  };
  std::array compiled{compile(0), compile(1), compile(2), compile(3), compile(4), compile(5)};

  for (const auto &text : compiled) {
    if (!text) {
      return std::unexpected(text.error());
    }
  }

  TextLimits limits;

  limits.max_input_bytes = options.maxTitleBytes;

  auto title = GraphemeText::from(options.title, policy, limits);

  if (!title) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  return PreparedFrameText{{std::move(*compiled[0]), std::move(*compiled[1]), std::move(*compiled[2]),
                            std::move(*compiled[3]), std::move(*compiled[4]), std::move(*compiled[5])},
                           std::move(*title)};
}

std::uint64_t framePaintWork(const PreparedFrameText &text, const FrameOptions &options, CellSize size,
                             std::span<const CellRect> exclusions) noexcept {
  if (exclusions.size() > 2) {
    return std::numeric_limits<std::uint64_t>::max();
  }

  const auto placement = titlePlacement(text.title, options, size, exclusions);
  auto work = perimeterCells(options.sides, size, exclusions);

  if (placement.width > 0) {
    // TextCommand borrows the complete source, so even its clipped tail consumes renderer work.
    work -= placement.width;
    work += text.title.bytes().size();
    work += text.title.graphemes().size();
  }

  return work;
}

std::expected<void, TreeError> paintFrame(PaintContext &context, const PreparedFrameText &text,
                                          const FrameOptions &options, CellRect bounds, CellStyle style,
                                          std::span<const CellRect> exclusions) {
  const auto [width, height] = bounds.size();

  if (bounds.empty()) {
    return {};
  }

  if (exclusions.size() > 2) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  if (framePaintWork(text, options, bounds.size(), exclusions) > options.border.maxPaintCells) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  const auto placement = titlePlacement(text.title, options, bounds.size(), exclusions);
  const auto sides = options.sides;
  auto draw = [&](std::int32_t x, std::int32_t y, std::size_t index) {
    for (const auto excluded : exclusions) {
      if (excluded.contains({x, y})) {
        return std::expected<void, TreeError>{};
      }
    }

    return submitGlyph(context, {x, y}, text.glyphs[index], bounds, style);
  };
  auto row = [&](std::int32_t y, bool top) -> std::expected<void, TreeError> {
    for (std::int32_t x = 0; x < width;) {
      if (top && x >= placement.start && x - placement.start < placement.width) {
        x = placement.start + placement.width;

        continue;
      }

      auto next = x;

      for (const auto excluded : exclusions) {
        if (excluded.contains({x, y})) {
          next = std::max(next, excluded.right());
        }
      }

      if (next > x) {
        x = next;

        continue;
      }

      const auto index = x == 0 && sides.left ? (top ? 0u : 2u) : x == width - 1 && sides.right ? (top ? 1u : 3u) : 4u;
      const auto result = draw(x, y, index);

      if (!result) {
        return result;
      }

      ++x;
    }

    return {};
  };

  if (sides.top) {
    const auto result = row(0, true);

    if (!result) {
      return result;
    }
  }

  if (sides.bottom && (!sides.top || height > 1)) {
    const auto result = row(height - 1, false);

    if (!result) {
      return result;
    }
  }

  const auto firstRow = sides.top ? 1 : 0;
  const auto lastRow = std::max(firstRow, height - (sides.bottom ? 1 : 0));

  auto column = [&](std::int32_t x) -> std::expected<void, TreeError> {
    for (auto y = firstRow; y < lastRow;) {
      auto next = y;

      for (const auto excluded : exclusions) {
        if (excluded.contains({x, y})) {
          next = std::max(next, excluded.bottom());
        }
      }

      if (next > y) {
        y = next;

        continue;
      }

      const auto result = draw(x, y, 5);

      if (!result) {
        return result;
      }

      ++y;
    }

    return {};
  };

  if (sides.left) {
    const auto result = column(0);

    if (!result) {
      return result;
    }
  }

  if (sides.right && (!sides.left || width > 1)) {
    const auto result = column(width - 1);

    if (!result) {
      return result;
    }
  }

  if (placement.width > 0) {
    const auto clip = CellRect::from({placement.start, 0}, {placement.width, 1});

    if (!clip) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }

    return submitGlyph(context, {placement.start, 0}, text.title, *clip, style);
  }

  return {};
}
} // namespace detail

Border::Border(WidthPolicy width, BorderOptions options, ControlStyle style, SizePolicy policy)
    : Border(width, FrameOptions{options}, style, policy, std::in_place) {
}

Border::Border(WidthPolicy width, FrameOptions options, ControlStyle style, SizePolicy policy, std::in_place_t)
    : SizedWidget(policy), layout_(frameLayout(options.sides)), options_(std::move(options)), style_(style) {
  auto prepared = detail::prepareFrameText(width, options_);

  if (prepared) {
    text_.emplace(std::move(*prepared));
  } else {
    preparationError_ = prepared.error();
  }
}

std::expected<void, TreeError> Border::validate(BorderOptions options) {
  if (!validGlyphs(options.glyphs) || options.maxPaintCells == 0) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  return {};
}

std::expected<void, TreeError> Border::validateFrame(const FrameOptions &options) noexcept {
  if (!validGlyphs(options.border.glyphs) || options.border.maxPaintCells == 0 || options.maxTitleBytes == 0 ||
      options.title.size() > options.maxTitleBytes ||
      (options.titleAlignment != FrameTitleAlignment::START && options.titleAlignment != FrameTitleAlignment::CENTER &&
       options.titleAlignment != FrameTitleAlignment::END) ||
      patternSources(options.pattern)[0].empty()) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  if (options.customGlyphs) {
    for (const auto source : customSources(*options.customGlyphs)) {
      if (source.empty() || source.size() > MAX_CUSTOM_GLYPH_BYTES) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
    }
  }

  return {};
}

std::expected<void, TreeError> Border::preparation() const noexcept {
  return preparationError_ ? std::unexpected(*preparationError_) : std::expected<void, TreeError>{};
}

std::expected<CellSize, TreeError> Border::measure(CellSize) {
  const auto valid = preparation();
  const auto padding = options_.sides.insets();

  return valid
           ? std::expected<CellSize, TreeError>(CellSize{padding.left + padding.right, padding.top + padding.bottom})
           : std::unexpected(valid.error());
}

std::expected<CellSize, TreeError> Border::childAvailable(CellSize available) const {
  return layout_.childAvailable(available);
}

std::expected<std::vector<CellRect>, TreeError> Border::arrangeChildren(std::span<const MeasuredChild> children,
                                                                        CellSize available) const {
  return layout_.arrangeChildren(children, available);
}

std::expected<CellSize, TreeError> Border::measureChildren(std::span<const MeasuredChild> children) const {
  auto result = layout_.measureChildren(children);

  if (result && computedStyle()) {
    result->width = std::max(result->width, computedStyle()->minWidth);
  }

  return result;
}

std::expected<void, TreeError> Border::arrange(CellRect local) {
  const auto bounds = CellRect::from({}, local.size());

  if (!bounds) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  bounds_ = *bounds;

  return {};
}

std::expected<CellRect, TreeError> Border::childClip(CellSize available) const {
  const auto size = available.normalized();
  const auto padding = options_.sides.insets();
  const auto clip = CellRect::from(
    {std::min(padding.left, size.width), std::min(padding.top, size.height)},
    {std::max(0, size.width - padding.left - padding.right), std::max(0, size.height - padding.top - padding.bottom)});

  return clip ? std::expected<CellRect, TreeError>(*clip) : std::unexpected(TreeError::INVALID_GEOMETRY);
}

std::expected<void, TreeError> Border::paint(PaintContext &context) {
  const auto valid = preparation();

  if (!valid) {
    return valid;
  }

  const auto resolved = computedStyle() ? *computedStyle() : styleDefaults();
  const auto style = !enabled_ ? resolved.disabled : focused_ ? resolved.focused : resolved.normal;

  return detail::paintFrame(context, *text_, options_, bounds_, style);
}

ComputedStyle Border::styleDefaults() const {
  return defaults(style_);
}

Gauge::Gauge(double value, WidthPolicy width, GaugeOptions options, ControlStyle style, SizePolicy policy)
    : SizedWidget(policy), value_(value), options_(options), style_(style),
      glyph_(glyph("\xe2\x96\x88", "#", width, options.glyphs)) {
}

std::expected<void, TreeError> Gauge::validate(double value, GaugeOptions options) {
  if (!std::isfinite(value) || !std::isfinite(options.minimum) || !std::isfinite(options.maximum) ||
      options.minimum >= options.maximum || !std::isfinite(options.maximum - options.minimum) ||
      value < options.minimum || value > options.maximum || options.preferredWidth < 0 || options.maxPaintCells == 0 ||
      !validGlyphs(options.glyphs)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  return {};
}

std::expected<CellSize, TreeError> Gauge::measure(CellSize) {
  const auto valid = validate(value_, options_);

  return valid ? std::expected<CellSize, TreeError>(CellSize{options_.preferredWidth, 1})
               : std::unexpected(valid.error());
}

std::expected<void, TreeError> Gauge::arrange(CellRect local) {
  const auto bounds = CellRect::from({}, local.size());

  if (!bounds) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  bounds_ = *bounds;

  return {};
}

std::expected<void, TreeError> Gauge::paint(PaintContext &context) {
  const auto valid = validate(value_, options_);

  if (!valid) {
    return valid;
  }

  const auto [width, height] = bounds_.size();

  if (static_cast<std::uint64_t>(width) * height > options_.maxPaintCells) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  if (bounds_.empty()) {
    return {};
  }

  const auto resolved = computedStyle() ? *computedStyle() : styleDefaults();
  const auto style = enabled_ ? resolved.normal : resolved.disabled;
  const RenderCommand background = FillCommand{bounds_, bounds_, style};
  auto result = context.submit(std::span(&background, 1));

  if (!result) {
    return result;
  }

  const auto fraction = (value_ - options_.minimum) / (options_.maximum - options_.minimum);
  const auto filled =
    static_cast<std::int32_t>(std::clamp(std::floor(fraction * width), 0.0, static_cast<double>(width)));

  for (std::int32_t y = 0; y < height; ++y) {
    for (std::int32_t x = 0; x < filled; ++x) {
      result = submitGlyph(context, {x, y}, glyph_, bounds_, style);

      if (!result) {
        return result;
      }
    }
  }

  return {};
}

ComputedStyle Gauge::styleDefaults() const {
  return defaults(style_);
}

double Gauge::value() const noexcept {
  return value_;
}

std::expected<void, TreeError> Gauge::display(double value) {
  const auto valid = validate(value, options_);

  if (!valid) {
    return valid;
  }

  value_ = value;

  return {};
}

std::expected<void, TreeError> setGaugeValue(WidgetTree &tree, Node<Gauge> node, double value) {
  std::expected<void, TreeError> result;
  const auto edited = tree.edit(node, [&](Gauge &gauge) {
    result = Gauge::validate(value, gauge.options_);

    if (!result || gauge.value_ == value) {
      return;
    }

    result = tree.invalidate(node, Invalidation::PAINT);

    if (result) {
      gauge.value_ = value;
    }
  });

  return edited ? result : edited;
}

std::expected<void, TreeError> syncBorder(WidgetTree &tree, Node<Border> node) {
  return syncBorder(tree, node, {});
}

std::expected<void, TreeError> syncBorder(WidgetTree &tree, Node<Border> node, Node<Widget> focused) {
  const auto eligible = tree.inputEligible(node);

  if (!eligible) {
    return std::unexpected(eligible.error());
  }

  bool projected{};

  if (focused != Node<Widget>{}) {
    const auto contains = tree.contains(node, focused);

    if (!contains) {
      return std::unexpected(contains.error());
    }

    projected = *eligible && *contains;
  }

  std::expected<void, TreeError> result;
  const auto edited = tree.edit(node, [&](Border &border) {
    if (border.enabled_ == *eligible && border.focused_ == projected) {
      return;
    }

    result = tree.invalidate(node, Invalidation::PAINT);

    if (result) {
      border.enabled_ = *eligible;
      border.focused_ = projected;
    }
  });

  return edited ? result : edited;
}

std::expected<void, TreeError> syncGauge(WidgetTree &tree, Node<Gauge> node) {
  const auto eligible = tree.inputEligible(node);

  if (!eligible) {
    return std::unexpected(eligible.error());
  }

  std::expected<void, TreeError> result;
  const auto edited = tree.edit(node, [&](Gauge &gauge) {
    if (gauge.enabled_ == *eligible) {
      return;
    }

    result = tree.invalidate(node, Invalidation::PAINT);

    if (result) {
      gauge.enabled_ = *eligible;
    }
  });

  return edited ? result : edited;
}
} // namespace portals
