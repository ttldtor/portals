// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/primitives.hpp>

#include <algorithm>
#include <cmath>

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

std::array<GraphemeText, 6> borderGlyphs(WidthPolicy policy, PrimitiveGlyphs mode) {
  return {glyph("\xe2\x94\x8c", "+", policy, mode), glyph("\xe2\x94\x90", "+", policy, mode),
          glyph("\xe2\x94\x94", "+", policy, mode), glyph("\xe2\x94\x98", "+", policy, mode),
          glyph("\xe2\x94\x80", "-", policy, mode), glyph("\xe2\x94\x82", "|", policy, mode)};
}

LinearOptions frameLayout() {
  LinearOptions options;

  options.padding = {1, 1, 1, 1};

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
} // namespace

Border::Border(WidthPolicy width, BorderOptions options, ControlStyle style, SizePolicy policy)
    : SizedWidget(policy), layout_(frameLayout()), options_(options), style_(style),
      glyphs_(borderGlyphs(width, options.glyphs)) {
}

std::expected<void, TreeError> Border::validate(BorderOptions options) {
  if (!validGlyphs(options.glyphs) || options.maxPaintCells == 0) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  return {};
}

std::expected<CellSize, TreeError> Border::measure(CellSize) {
  const auto valid = validate(options_);

  return valid ? std::expected<CellSize, TreeError>(CellSize{2, 2}) : std::unexpected(valid.error());
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
  const auto clip = CellRect::from({std::min(1, size.width), std::min(1, size.height)},
                                   {std::max(0, size.width - 2), std::max(0, size.height - 2)});

  return clip ? std::expected<CellRect, TreeError>(*clip) : std::unexpected(TreeError::INVALID_GEOMETRY);
}

std::expected<void, TreeError> Border::paint(PaintContext &context) {
  const auto valid = validate(options_);

  if (!valid) {
    return valid;
  }

  if (bounds_.empty()) {
    return {};
  }

  const auto [width, height] = bounds_.size();
  const auto cells =
    width == 1 || height == 1 ? static_cast<std::uint64_t>(width) * height : 2ULL * width + 2ULL * height - 4;

  if (cells > options_.maxPaintCells) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  const auto resolved = computedStyle() ? *computedStyle() : styleDefaults();
  const auto style = enabled_ ? resolved.normal : resolved.disabled;
  auto draw = [&](std::int32_t x, std::int32_t y, std::size_t index) {
    return submitGlyph(context, {x, y}, glyphs_[index], bounds_, style);
  };

  // Emit each perimeter cell once, including collapsed one-cell rows/columns.
  for (std::int32_t x = 0; x < width; ++x) {
    auto result = draw(x, 0, x == 0 ? 0 : x == width - 1 ? 1 : 4);

    if (!result) {
      return result;
    }

    if (height > 1) {
      result = draw(x, height - 1, x == 0 ? 2 : x == width - 1 ? 3 : 4);

      if (!result) {
        return result;
      }
    }
  }

  for (std::int32_t y = 1; y < height - 1; ++y) {
    auto result = draw(0, y, 5);

    if (!result) {
      return result;
    }

    if (width > 1) {
      result = draw(width - 1, y, 5);

      if (!result) {
        return result;
      }
    }
  }

  return {};
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
  const auto eligible = tree.inputEligible(node);

  if (!eligible) {
    return std::unexpected(eligible.error());
  }

  std::expected<void, TreeError> result;
  const auto edited = tree.edit(node, [&](Border &border) {
    if (border.enabled_ == *eligible) {
      return;
    }

    result = tree.invalidate(node, Invalidation::PAINT);

    if (result) {
      border.enabled_ = *eligible;
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
