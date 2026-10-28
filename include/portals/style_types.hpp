// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <array>
#include <optional>
#include <portals/cell_buffer.hpp>
#include <portals/scrollbar_skin.hpp>

namespace portals {
/** @brief Resolved frame parts; absent parts inherit the ordinary control state. */
struct FrameAppearance {
  std::optional<CellStyle> line{};  ///< Perimeter glyph appearance.
  std::optional<CellStyle> title{}; ///< Title text appearance.
  std::optional<CellStyle> ends{};  ///< Prefix/suffix appearance, including their spacing.

  /** @param other Value to compare. @return Whether all parts agree. */
  bool operator==(const FrameAppearance &other) const = default;
};

/** @brief Resolved scrollbar parts; absent values inherit the ordinary control state.
 * Colors and attributes are shared by both axes; glyphs are orientation-specific.
 */
struct ScrollbarAppearance {
  std::optional<CellStyle> track{};  ///< Track cells outside the thumb.
  std::optional<CellStyle> thumb{};  ///< Moving thumb cells.
  std::optional<CellStyle> ends{};   ///< Optional decorative endpoint cells.
  std::optional<CellStyle> corner{}; ///< Intersecting horizontal and vertical gutters.
  std::optional<CellStyle> arrows{}; ///< Functional end buttons, independent of decorative ends.

  std::optional<ScrollbarPattern> trackPattern{};      ///< Resolved independent glyph source.
  std::optional<ScrollbarThumbPattern> thumbPattern{}; ///< Resolved independent glyph source.
  std::optional<ScrollbarArrowPattern> arrowPattern{}; ///< Resolved independent glyph source.

  /** @param other Value to compare. @return Whether every part agrees. */
  bool operator==(const ScrollbarAppearance &other) const = default;
};

/** @brief Complete resolved thumb segment values, shared by both axes.
 * Missing declarations inherit the common thumb while resolving this value.
 */
struct ScrollbarThumbAppearance {
  CellStyle start{};  ///< First cell of a thumb of length at least two.
  CellStyle body{};   ///< Interior cells of a thumb of length at least three.
  CellStyle end{};    ///< Last cell of a thumb of length at least two.
  CellStyle single{}; ///< Only cell of a one-cell thumb.

  /** @param other Resolved values to compare. @return Whether all segment attributes agree. */
  bool operator==(const ScrollbarThumbAppearance &other) const = default;
};

/** @brief Owning resolved presentation; no actions or model state are encoded. */
struct ComputedStyle {
  CellStyle normal;        ///< Ordinary text and fill.
  CellStyle focused;       ///< Focused control text and fill.
  CellStyle disabled;      ///< Disabled control text and fill.
  CellStyle selected;      ///< Selected list/table row or editor range.
  CellStyle header;        ///< Fixed table heading.
  CellStyle caret;         ///< Editor software caret.
  std::int32_t minWidth{}; ///< Nonnegative intrinsic minimum in cells; zero leaves measurement unchanged.
  std::array<FrameAppearance, 6> frame{};         ///< Parts in normal/focused/disabled/selected/header/caret order.
  std::array<ScrollbarAppearance, 6> scrollbar{}; ///< Parts in normal/focused/disabled/selected/header/caret order.
  /** @brief Optional complete segment values in normal/focused/disabled/selected/header/caret order.
   * Engaged only for matching segment declarations or explicit widget defaults. Inline ownership keeps
   * resolution and copies allocation-free; absence retains the common thumb and avoids segment resolution.
   */
  std::optional<std::array<ScrollbarThumbAppearance, 6>> scrollbarThumbSegments{};
  /** @param other Owning value to compare.
   * @return True when every paint value and metric agrees.
   */
  bool operator==(const ComputedStyle &other) const = default;
};
} // namespace portals
