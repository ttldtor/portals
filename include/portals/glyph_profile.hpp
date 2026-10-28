// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstdint>

namespace portals {
/** @brief Explicit glyph map, independent of colors and geometry.
 * NF maps use Nerd Fonts 3 Powerline codepoints. Selecting one asserts host/font support;
 * the library neither detects missing font glyphs nor installs fonts.
 */
enum class GlyphProfile : std::uint8_t {
  DEFAULT,        ///< Preserve the control's existing glyph sources.
  UNICODE_GLYPHS, ///< Portable Unicode rounded/inset forms.
  ASCII,          ///< Portable ASCII forms.
  NF_ROUNDED,     ///< Powerline rounded horizontal caps, Unicode vertical inset caps.
  NF_CHEVRON,     ///< Powerline chevron horizontal caps, Unicode vertical inset caps.
  NF_SLASH,       ///< Powerline slash horizontal caps, Unicode vertical inset caps.
  NF_BACKSLASH    ///< Powerline backslash horizontal caps, Unicode vertical inset caps.
};

/** @brief Portable replacement when the selected map fails one-cell admission. */
enum class GlyphFallback : std::uint8_t {
  UNICODE_GLYPHS, ///< Unicode replacement, with ASCII when Unicode is not one cell.
  ASCII           ///< ASCII replacement; also selects ASCII vertical caps for NF maps.
};

/** @brief Complete logical selection; resolution and copies never allocate. */
struct GlyphSelection {
  GlyphProfile profile{GlyphProfile::DEFAULT};           ///< Existing sources unless explicitly selected.
  GlyphFallback fallback{GlyphFallback::UNICODE_GLYPHS}; ///< Explicit admission fallback.

  /** @param other Logical selection to compare. @return Whether both choices agree. */
  constexpr bool operator==(const GlyphSelection &other) const = default;
};
} // namespace portals
