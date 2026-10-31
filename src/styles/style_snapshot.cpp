// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/style_types.hpp>

#include <algorithm>
#include <utility>

namespace portals {
namespace {
constexpr std::array<FrameAppearance, 6> EMPTY_FRAME{};
constexpr std::array<ScrollbarAppearance, 6> EMPTY_SCROLLBAR{};
constexpr std::array<GlyphAppearance, 6> EMPTY_GLYPHS{};

// Hash semantic values, including optional presence; object padding is not presentation.
struct StyleHash {
  std::size_t value{};

  template <class T> void add(T input) noexcept {
    value ^= static_cast<std::size_t>(input) + static_cast<std::size_t>(0x9e3779b9U) + (value << 6) + (value >> 2);
  }

  template <class T> void add(const std::optional<T> &input) noexcept {
    add(input.has_value());
    if (input) {
      add(*input);
    }
  }

  template <class T, std::size_t N> void add(const std::array<T, N> &input) noexcept {
    for (const auto &part : input) {
      add(part);
    }
  }

  void add(const RgbColor &input) noexcept {
    add(input.red);
    add(input.green);
    add(input.blue);
  }

  void add(const CellStyle &input) noexcept {
    add(input.foreground);
    add(input.background);
    add(input.bold);
    add(input.italic);
    add(input.underline);
  }

  void add(const FrameAppearance &input) noexcept {
    add(input.line);
    add(input.title);
    add(input.ends);
  }

  void add(const ScrollbarAppearance &input) noexcept {
    add(input.track);
    add(input.thumb);
    add(input.ends);
    add(input.corner);
    add(input.arrows);
    add(input.trackPattern);
    add(input.thumbPattern);
    add(input.arrowPattern);
  }

  void add(const ScrollbarThumbAppearance &input) noexcept {
    add(input.start);
    add(input.body);
    add(input.end);
    add(input.single);
  }

  void add(const GlyphAppearance &input) noexcept {
    add(input.profile);
    add(input.fallback);
  }

  void add(const StyleAxisSize &input) noexcept {
    add(input.mode);
    add(input.extent);
    add(input.weight);
  }

  void add(const StyleSizing &input) noexcept {
    add(input.width);
    add(input.height);
    add(input.maxWidth);
    add(input.minHeight);
    add(input.maxHeight);
  }

  void add(const std::shared_ptr<const detail::ScrollbarGlyphLiteral> &input) noexcept {
    add(static_cast<bool>(input));
    if (input) {
      for (const unsigned char byte : input->text) {
        add(byte);
      }
    }
  }
};

std::size_t hashStyle(detail::ComputedStyleView input) noexcept {
  StyleHash hash;
  hash.add(input.normal);
  hash.add(input.focused);
  hash.add(input.disabled);
  hash.add(input.selected);
  hash.add(input.header);
  hash.add(input.caret);
  hash.add(input.minWidth);
  hash.add(input.sizing != nullptr);
  if (input.sizing) {
    hash.add(*input.sizing);
  }
  hash.add(input.frame);
  hash.add(input.scrollbar);
  hash.add(input.scrollbarThumbSegments != nullptr);
  if (input.scrollbarThumbSegments) {
    hash.add(*input.scrollbarThumbSegments);
  }
  hash.add(input.glyphs);
  hash.add(input.scrollbarGlyphs != nullptr);
  if (input.scrollbarGlyphs) {
    hash.add(*input.scrollbarGlyphs);
  }
  return hash.value;
}
} // namespace

namespace detail {
bool equalScrollbarGlyphs(const ScrollbarGlyphRefs &left, const ScrollbarGlyphRefs &right) noexcept {
  return std::ranges::equal(left, right, [](const auto &a, const auto &b) {
    return a == b || (a && b && *a == *b);
  });
}

bool equalScrollbarGlyphs(const ScrollbarGlyphStates &left, const ScrollbarGlyphStates &right) noexcept {
  return std::ranges::equal(left, right, [](const auto &a, const auto &b) {
    return equalScrollbarGlyphs(a, b);
  });
}

bool StyleGlyphBlock::operator==(const StyleGlyphBlock &other) const noexcept {
  return values == other.values &&
         (literals == other.literals ||
          (literals && other.literals && equalScrollbarGlyphs(*literals, *other.literals))) &&
         (sizing == other.sizing || (sizing && other.sizing && *sizing == *other.sizing));
}

std::size_t styleBlockHash(const StyleGlyphBlock &value) noexcept {
  StyleHash hash;
  hash.add(value.values);
  hash.add(static_cast<bool>(value.literals));
  if (value.literals) {
    hash.add(*value.literals);
  }
  hash.add(static_cast<bool>(value.sizing));
  if (value.sizing) {
    hash.add(*value.sizing);
  }
  return hash.value;
}

std::size_t styleBlockHash(const std::array<FrameAppearance, 6> &value) noexcept {
  StyleHash hash;
  hash.add(value);
  return hash.value;
}

std::size_t styleBlockHash(const std::array<ScrollbarAppearance, 6> &value) noexcept {
  StyleHash hash;
  hash.add(value);
  return hash.value;
}

std::size_t styleBlockHash(const std::array<ScrollbarThumbAppearance, 6> &value) noexcept {
  StyleHash hash;
  hash.add(value);
  return hash.value;
}

std::size_t styleBlockHash(const std::array<GlyphAppearance, 6> &value) noexcept {
  StyleHash hash;
  hash.add(value);
  return hash.value;
}
} // namespace detail

StyleSnapshot::StyleSnapshot(const ComputedStyle &value) : StyleSnapshot(PreparedTag{}, value, {}) {
  if (value.frame != EMPTY_FRAME) {
    blocks_.frame = std::make_shared<const std::array<FrameAppearance, 6>>(value.frame);
  }

  if (value.scrollbar != EMPTY_SCROLLBAR) {
    blocks_.scrollbar = std::make_shared<const std::array<ScrollbarAppearance, 6>>(value.scrollbar);
  }

  if (value.scrollbarThumbSegments) {
    blocks_.thumb = std::make_shared<const std::array<ScrollbarThumbAppearance, 6>>(*value.scrollbarThumbSegments);
  }

  if (value.glyphs != EMPTY_GLYPHS || value.scrollbarGlyphs.has_value() || value.sizing) {
    detail::StyleGlyphBlock glyphs{value.glyphs};
    if (value.scrollbarGlyphs.has_value()) {
      glyphs.literals = std::make_shared<const detail::ScrollbarGlyphStates>(*value.scrollbarGlyphs);
    }
    if (value.sizing) {
      glyphs.sizing = std::make_shared<const StyleSizing>(*value.sizing);
    }
    blocks_.glyphs = std::make_shared<const detail::StyleGlyphBlock>(std::move(glyphs));
  }
}

StyleSnapshot::StyleSnapshot(PreparedTag, const ComputedStyle &value, detail::StyleSnapshotBlocks blocks) noexcept
    : normal(value.normal), focused(value.focused), disabled(value.disabled), selected(value.selected),
      header(value.header), caret(value.caret), minWidth(value.minWidth), blocks_(std::move(blocks)) {
}

const std::array<FrameAppearance, 6> &StyleSnapshot::frame() const noexcept {
  return blocks_.frame ? *blocks_.frame : EMPTY_FRAME;
}

const std::array<ScrollbarAppearance, 6> &StyleSnapshot::scrollbar() const noexcept {
  return blocks_.scrollbar ? *blocks_.scrollbar : EMPTY_SCROLLBAR;
}

const std::array<ScrollbarThumbAppearance, 6> *StyleSnapshot::scrollbarThumbSegments() const noexcept {
  return blocks_.thumb.get();
}

const std::array<GlyphAppearance, 6> &StyleSnapshot::glyphs() const noexcept {
  return blocks_.glyphs ? blocks_.glyphs->values : EMPTY_GLYPHS;
}

const detail::ScrollbarGlyphStates *StyleSnapshot::scrollbarGlyphs() const noexcept {
  return blocks_.glyphs ? blocks_.glyphs->literals.get() : nullptr;
}

const StyleSizing *StyleSnapshot::sizing() const noexcept {
  return blocks_.glyphs ? blocks_.glyphs->sizing.get() : nullptr;
}

bool StyleSnapshot::hasFrame() const noexcept {
  return static_cast<bool>(blocks_.frame);
}

bool StyleSnapshot::hasScrollbar() const noexcept {
  return static_cast<bool>(blocks_.scrollbar);
}

bool StyleSnapshot::hasThumbSegments() const noexcept {
  return static_cast<bool>(blocks_.thumb);
}

bool StyleSnapshot::hasGlyphs() const noexcept {
  return blocks_.glyphs && (blocks_.glyphs->values != EMPTY_GLYPHS || blocks_.glyphs->literals);
}

ComputedStyle StyleSnapshot::flatten() const noexcept {
  ComputedStyle result;
  result.normal = normal;
  result.focused = focused;
  result.disabled = disabled;
  result.selected = selected;
  result.header = header;
  result.caret = caret;
  result.minWidth = minWidth;
  result.frame = frame();
  result.scrollbar = scrollbar();
  if (blocks_.thumb) {
    result.scrollbarThumbSegments = *blocks_.thumb;
  }
  result.glyphs = glyphs();
  if (const auto *literals = scrollbarGlyphs()) {
    result.scrollbarGlyphs = *literals;
  }
  if (const auto *constraints = sizing()) {
    result.sizing = *constraints;
  }
  return result;
}

bool StyleSnapshot::operator==(const ComputedStyle &other) const noexcept {
  return normal == other.normal && focused == other.focused && disabled == other.disabled &&
         selected == other.selected && header == other.header && caret == other.caret && minWidth == other.minWidth &&
         frame() == other.frame && scrollbar() == other.scrollbar &&
         static_cast<bool>(blocks_.thumb) == other.scrollbarThumbSegments.has_value() &&
         (!blocks_.thumb || *blocks_.thumb == *other.scrollbarThumbSegments) && glyphs() == other.glyphs &&
         static_cast<bool>(scrollbarGlyphs()) == other.scrollbarGlyphs.has_value() &&
         (!other.scrollbarGlyphs || detail::equalScrollbarGlyphs(*scrollbarGlyphs(), *other.scrollbarGlyphs)) &&
         static_cast<bool>(sizing()) == other.sizing.has_value() && (!sizing() || *sizing() == *other.sizing);
}

bool StyleSnapshot::operator==(const StyleSnapshot &other) const noexcept {
  return normal == other.normal && focused == other.focused && disabled == other.disabled &&
         selected == other.selected && header == other.header && caret == other.caret && minWidth == other.minWidth &&
         frame() == other.frame() && scrollbar() == other.scrollbar() &&
         static_cast<bool>(blocks_.thumb) == static_cast<bool>(other.blocks_.thumb) &&
         (!blocks_.thumb || *blocks_.thumb == *other.blocks_.thumb) && glyphs() == other.glyphs() &&
         static_cast<bool>(scrollbarGlyphs()) == static_cast<bool>(other.scrollbarGlyphs()) &&
         (!scrollbarGlyphs() || detail::equalScrollbarGlyphs(*scrollbarGlyphs(), *other.scrollbarGlyphs())) &&
         static_cast<bool>(sizing()) == static_cast<bool>(other.sizing()) &&
         (!sizing() || *sizing() == *other.sizing());
}

bool ComputedStyle::operator==(const ComputedStyle &other) const noexcept {
  return normal == other.normal && focused == other.focused && disabled == other.disabled &&
         selected == other.selected && header == other.header && caret == other.caret && minWidth == other.minWidth &&
         frame == other.frame && scrollbar == other.scrollbar &&
         scrollbarThumbSegments == other.scrollbarThumbSegments && glyphs == other.glyphs &&
         scrollbarGlyphs.has_value() == other.scrollbarGlyphs.has_value() &&
         (!scrollbarGlyphs || detail::equalScrollbarGlyphs(*scrollbarGlyphs, *other.scrollbarGlyphs)) &&
         sizing == other.sizing;
}

std::size_t StyleSnapshot::contentHash() const noexcept {
  return hashStyle(*this);
}

std::size_t StyleSnapshot::contentHash(const ComputedStyle &value) noexcept {
  return hashStyle(value);
}
} // namespace portals
