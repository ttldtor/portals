// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace portals {
/** @brief Default UTF-8 byte cap for each supplied scrollbar glyph. */
inline constexpr std::size_t DEFAULT_SCROLLBAR_GLYPH_BYTES = 64;
/** @brief Opt-in scrollbar glyph presets; legacy calls retain their original appearance. */
enum class ScrollbarPattern {
  LEGACY,  ///< Existing shaded track and block thumb.
  FRAME,   ///< Actual frame lines, or single lines on an unframed control.
  MINIMAL, ///< Blank track and block thumb.
  ASCII    ///< Dot track and hash thumb.
};

/** @brief Sparse owning glyph overrides for one axis, validated before painting.
 * Every nonempty source must be exactly one printable one-cell grapheme under the receiving WidthPolicy.
 */
struct ScrollbarAxisGlyphs {
  std::optional<std::string> track{}; ///< Track override; an empty source is invalid.
  std::optional<std::string> thumb{}; ///< Thumb override; an empty source is invalid.
  std::optional<std::string> start{}; ///< Leading decoration; empty suppresses that end.
  std::optional<std::string> end{};   ///< Trailing decoration; empty suppresses that end.

  /** @brief Keep checked-library owning transfers fallible. */
  ~ScrollbarAxisGlyphs() = default;
};

/** @brief Owning scrollbar skin; no callbacks, borrowed views, timers or geometry changes.
 * Ends occupy existing track cells, below the thumb in paint priority; their clicks keep ordinary track behavior.
 * Both axes share part colors/attributes through the existing style resolver.
 */
struct ScrollbarSkin {
  ScrollbarPattern pattern{ScrollbarPattern::LEGACY}; ///< Built-in glyph source beneath sparse overrides.
  bool ends{}; ///< Paint decorative endpoints where the thumb does not cover them.
  std::size_t maxGlyphBytes{DEFAULT_SCROLLBAR_GLYPH_BYTES}; ///< Positive UTF-8 byte cap per supplied glyph.
  ScrollbarAxisGlyphs horizontal{};                         ///< Horizontal track, thumb and optional ends.
  ScrollbarAxisGlyphs vertical{};                           ///< Vertical track, thumb and optional ends.
  std::optional<std::string> corner{}; ///< Intersection glyph; omitted or empty keeps the existing fill.

  /** @brief Keep checked-library owning transfers fallible. */
  ~ScrollbarSkin() = default;
};
} // namespace portals
