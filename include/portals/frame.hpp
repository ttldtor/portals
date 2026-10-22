// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/layout.hpp>

namespace portals {
/** @brief Built-in frame line patterns, independent of terminal width policy. */
enum class FramePattern {
  SINGLE,         ///< Single lines and square corners.
  PLAIN = SINGLE, ///< Alias for single lines.
  ROUNDED,        ///< Single lines and rounded corners.
  DOUBLE,         ///< Double lines and square corners.
  HEAVY,          ///< Heavy lines and square corners.
  ASCII           ///< Portable plus, minus and vertical-bar glyphs.
};

/** @brief Present frame sides, also defining one-cell content insets. */
struct FrameSides {
  bool top{true};    ///< Reserve and paint the top row.
  bool right{true};  ///< Reserve and paint the right column.
  bool bottom{true}; ///< Reserve and paint the bottom row.
  bool left{true};   ///< Reserve and paint the left column.

  /** @return Corresponding nonnegative left/top/right/bottom cell padding. */
  constexpr LayoutPadding insets() const noexcept {
    return {left ? 1 : 0, top ? 1 : 0, right ? 1 : 0, bottom ? 1 : 0};
  }
};

/** @brief Alignment of the maximal whole-grapheme title prefix fitting the top edge. */
enum class FrameTitleAlignment {
  START,  ///< Place the visible prefix after the leading corner.
  CENTER, ///< Center the visible prefix between present corners.
  END     ///< Place the visible prefix before the trailing corner.
};

} // namespace portals
