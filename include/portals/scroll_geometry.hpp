// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <expected>
#include <limits>
#include <portals/frame.hpp>
#include <portals/widget_types.hpp>

namespace portals {
/** @brief Logical scrolling axis; X increases right and Y increases down. */
enum class ScrollAxis {
  HORIZONTAL, ///< Columns.
  VERTICAL    ///< Rows.
};

/** @brief Presence policy, independent of whether the content can scroll. */
enum class ScrollBarPolicy {
  NEVER, ///< Do not allocate or paint a bar.
  AUTO,  ///< Show a bar only when its logical extent exceeds the viewport.
  ALWAYS ///< Show a bar even when its thumb cannot move.
};

/** @brief Explicit relationship between bars, content and an optional frame. */
enum class ScrollPlacement {
  RESERVED, ///< Bars occupy one-cell gutters outside the logical viewport.
  OVERLAY,  ///< Bars obscure edge cells of the unchanged logical viewport.
  ON_FRAME  ///< Bars occupy the right/bottom frame lines, excluding corners.
};

/** @brief Owning exact integral position and sub-cell remainder; no affinity or invalidation. */
struct ScrollOffset {
  std::uint64_t raster{}; ///< Exact logical cell index, including indices above 2^53.
  double fraction{};      ///< Finite remainder in [0,1).
};

/** @brief Owning one-axis logical metrics, independent of materialized content. */
struct ScrollAxisMetrics {
  std::uint64_t extent{};   ///< Total logical cells.
  std::uint64_t viewport{}; ///< Visible logical cells; zero admits only position zero.
};

/** @brief Logical content extent; ordinary retained layout still has int32 cell bounds. */
struct ScrollExtent {
  std::uint64_t width{};  ///< Columns.
  std::uint64_t height{}; ///< Rows.
};

/** @brief Derived viewport-start range, preserving the existing ScrollState count contract. */
struct ScrollRange {
  std::uint64_t maximum{}; ///< Inclusive maximum integral offset; no fraction at this endpoint.
  std::uint64_t count{1};  ///< Number of permitted integral starts; always positive.
};

/** @brief Compute an exact range without unsigned subtraction or addition overflow.
 * @param metrics Owning logical extent and viewport; every uint64 value is supported.
 * @return Inclusive maximum and count=maximum+1; zero viewport produces {0,1}.
 * @note Any thread; constexpr, allocation-free, no mutation or invalidation.
 */
[[nodiscard]] constexpr ScrollRange scrollRange(ScrollAxisMetrics metrics) noexcept {
  const auto maximum =
    metrics.viewport != 0 && metrics.extent > metrics.viewport ? metrics.extent - metrics.viewport : 0;

  return {maximum, maximum + 1};
}

/** @brief Validate an offset remainder without losing its integral index.
 * @param offset Owning intended position; the integral component is unrestricted.
 * @return True for a finite remainder in [0,1); false for NaN/infinity/out-of-range values.
 */
[[nodiscard]] constexpr bool validScrollOffset(ScrollOffset offset) noexcept {
  return offset.fraction >= 0 && offset.fraction < 1;
}

/** @brief Compute one page movement using guarded unsigned subtraction.
 * @param viewport Logical visible cells; zero means movement is unavailable.
 * @param overlap Cells shared by consecutive pages, including values greater than viewport.
 * @return Zero for an empty viewport; otherwise max(viewport-overlap,1).
 */
[[nodiscard]] constexpr std::uint64_t scrollPageStep(std::uint64_t viewport, std::uint64_t overlap = 1) noexcept {
  if (viewport == 0) {
    return 0;
  }

  return viewport > overlap ? viewport - overlap : 1;
}

namespace detail {
/** @brief Exact quotient and remainder used by bounded scroll geometry arithmetic. */
struct ScrollDivision {
  std::uint64_t quotient{};  ///< Integral quotient of the bounded product division.
  std::uint64_t remainder{}; ///< Nonnegative remainder, strictly smaller than the divisor.
};

// Exact a*b/d for a<=d and d>0. A quotient/remainder recurrence avoids a wide product.
// Every prefix quotient is bounded by that prefix of b; doubled remainders use subtraction.
constexpr ScrollDivision scrollMultiplyDivide(std::uint64_t a, std::uint64_t b, std::uint64_t d) noexcept {
  if (a == d) {
    return {b, 0};
  }

  ScrollDivision result;

  for (int bit = 63; bit >= 0; --bit) {
    result.quotient *= 2;

    if (result.remainder >= d - result.remainder) {
      result.remainder -= d - result.remainder;
      ++result.quotient;
    } else {
      result.remainder += result.remainder;
    }

    if (((b >> bit) & 1) != 0) {
      if (result.remainder >= d - a) {
        result.remainder -= d - a;
        ++result.quotient;
      } else {
        result.remainder += a;
      }
    }
  }

  return result;
}

// Preserve floor(fraction*units) exactly from binary64, including subnormals.
constexpr std::uint64_t scrollFractionUnits(double fraction, std::uint64_t units) noexcept {
  static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits == 53);
  constexpr std::uint32_t MANTISSA_BITS = 52;
  constexpr std::uint32_t EXPONENT_BIAS = 1023;
  constexpr std::uint64_t EXPONENT_MASK = 0x7ff;
  constexpr auto INTEGER_BITS = std::numeric_limits<std::uint64_t>::digits;
  const auto bits = std::bit_cast<std::uint64_t>(fraction);
  const auto exponent = static_cast<std::uint32_t>((bits >> MANTISSA_BITS) & EXPONENT_MASK);
  auto mantissa = bits & ((std::uint64_t{1} << MANTISSA_BITS) - 1);
  const auto shift = exponent ? MANTISSA_BITS + EXPONENT_BIAS - exponent : MANTISSA_BITS + EXPONENT_BIAS - 1;

  if (exponent != 0) {
    mantissa |= std::uint64_t{1} << MANTISSA_BITS;
  }

  const auto divisionShift = std::min(shift, static_cast<std::uint32_t>(INTEGER_BITS - 1));
  const auto remainingShift = shift - divisionShift;

  if (remainingShift >= INTEGER_BITS) {
    return 0;
  }

  return scrollMultiplyDivide(mantissa, units, std::uint64_t{1} << divisionShift).quotient >> remainingShift;
}

// Position and travel share arbitrary integral units; the logical raster remains exact.
[[nodiscard]] constexpr ScrollOffset preciseScrollOffsetForThumb(std::uint64_t position, std::uint64_t travel,
                                                                 std::uint64_t maximum) noexcept {
  if (travel == 0 || maximum == 0) {
    return {};
  }

  const auto value = scrollMultiplyDivide(std::min(position, travel), maximum, travel);
  constexpr double MAXIMUM_FRACTION = 0x1.fffffffffffffp-1;
  const auto fraction = static_cast<double>(value.remainder) / static_cast<double>(travel);

  // Rounded division can reach one; do not carry it into the exact integral quotient.
  return {value.quotient, std::min(fraction, MAXIMUM_FRACTION)};
}
} // namespace detail

/** @brief Owning one-cell-track thumb geometry; values are relative to the track start. */
struct ScrollThumb {
  std::int32_t start{};  ///< First occupied track cell.
  std::int32_t length{}; ///< Occupied cells, zero only for an empty track.
  std::int32_t travel{}; ///< Maximum thumb-start cell; zero disables dragging.
};

/** @brief Map exact logical position to a thumb without a floating-point index round trip.
 * @param metrics Logical range; every uint64 extent/viewport is supported.
 * @param offset Intended integral position, clamped to the viewport-start range.
 * @param trackLength Nonnegative number of track cells.
 * @param minimumThumb Positive minimum occupied cells, capped by the track length.
 * @return Owning geometry, or INVALID_ARGUMENT for negative length/nonpositive minimum.
 * @note Any thread; constexpr and bounded integer work, with no allocation or invalidation.
 */
[[nodiscard]] constexpr std::expected<ScrollThumb, TreeError> scrollThumb(ScrollAxisMetrics metrics,
                                                                          std::uint64_t offset,
                                                                          std::int32_t trackLength,
                                                                          std::int32_t minimumThumb = 1) noexcept {
  if (trackLength < 0 || minimumThumb <= 0) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  if (trackLength == 0) {
    return ScrollThumb{};
  }

  const auto range = scrollRange(metrics);

  if (range.maximum == 0) {
    return ScrollThumb{0, trackLength, 0};
  }

  const auto proportional = detail::scrollMultiplyDivide(std::min(metrics.viewport, metrics.extent),
                                                         static_cast<std::uint64_t>(trackLength), metrics.extent);
  const auto length =
    std::clamp(static_cast<std::int32_t>(proportional.quotient), std::min(minimumThumb, trackLength), trackLength);
  const auto travel = trackLength - length;
  const auto start =
    detail::scrollMultiplyDivide(std::min(offset, range.maximum), static_cast<std::uint64_t>(travel), range.maximum);

  return ScrollThumb{static_cast<std::int32_t>(start.quotient), length, travel};
}

/** @brief Owning thumb interval in eighths of a track cell; no references or invalidation. */
struct ScrollFractionalThumb {
  std::int64_t startEighths{};  ///< Inclusive start, relative to the trimmed track.
  std::int64_t lengthEighths{}; ///< Integral cell length multiplied by eight.
  std::int64_t travelEighths{}; ///< Exact maximum start, including the endpoint.
};

/** @brief Map an exact logical position to eighth-cell thumb placement.
 * @param metrics Owning logical range; every uint64 extent/viewport is supported.
 * @param offset Exact raster plus finite remainder in [0,1), clamped to the range.
 * @param trackLength Nonnegative trimmed track length, in cells.
 * @param minimumThumb Positive minimum thumb length, in cells, capped by the track.
 * @return Exact quantized interval, or INVALID_ARGUMENT for invalid geometry/remainder.
 * @note Any thread; constexpr bounded integer work, no allocation or mutation. Integral API is unchanged.
 */
[[nodiscard]] constexpr std::expected<ScrollFractionalThumb, TreeError>
scrollFractionalThumb(ScrollAxisMetrics metrics, ScrollOffset offset, std::int32_t trackLength,
                      std::int32_t minimumThumb = 1) noexcept {
  if (!validScrollOffset(offset)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  const auto integral = scrollThumb(metrics, offset.raster, trackLength, minimumThumb);
  if (!integral) {
    return std::unexpected(integral.error());
  }
  constexpr std::uint64_t EIGHTHS_PER_CELL = 8;
  const auto maximum = scrollRange(metrics).maximum;
  const auto travel = static_cast<std::uint64_t>(integral->travel) * EIGHTHS_PER_CELL;
  const auto length = static_cast<std::int64_t>(integral->length) * static_cast<std::int64_t>(EIGHTHS_PER_CELL);
  if (maximum == 0 || travel == 0) {
    return ScrollFractionalThumb{0, length, static_cast<std::int64_t>(travel)};
  }
  if (offset.raster >= maximum) {
    return ScrollFractionalThumb{static_cast<std::int64_t>(travel), length, static_cast<std::int64_t>(travel)};
  }
  const auto position = detail::scrollMultiplyDivide(offset.raster, travel, maximum);
  const auto fraction = detail::scrollFractionUnits(offset.fraction, travel);
  const auto remainder = fraction % maximum;
  // The discarded fractional part is <1; the saved integral remainder retains every possible carry.
  const auto start = position.quotient + fraction / maximum + (position.remainder >= maximum - remainder ? 1 : 0);
  return ScrollFractionalThumb{static_cast<std::int64_t>(start), length, static_cast<std::int64_t>(travel)};
}

/** @brief Inverse thumb mapping, rounded to nearest integer with ties toward the start.
 * @param position Signed intended thumb-start cell, clamped into [0,travel].
 * @param travel Nonnegative maximum thumb-start cell; zero returns position zero.
 * @param maximum Inclusive exact logical maximum, including UINT64_MAX.
 * @return Exact logical offset, or INVALID_ARGUMENT for negative travel.
 * @note Endpoints are exact; any thread, constexpr, bounded work and no invalidation.
 */
[[nodiscard]] constexpr std::expected<std::uint64_t, TreeError>
scrollOffsetForThumb(std::int64_t position, std::int32_t travel, std::uint64_t maximum) noexcept {
  if (travel < 0) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  if (position <= 0 || travel == 0 || maximum == 0) {
    return 0;
  }

  if (position >= travel) {
    return maximum;
  }

  const auto denominator = static_cast<std::uint64_t>(travel);
  auto value = detail::scrollMultiplyDivide(static_cast<std::uint64_t>(position), maximum, denominator);

  if (value.remainder > denominator - value.remainder) {
    ++value.quotient;
  }

  return value.quotient;
}

/** @brief Value-only placement inputs, without a frame painter or widget lifetime. */
struct ScrollLayoutOptions {
  ScrollBarPolicy horizontal{ScrollBarPolicy::AUTO};    ///< Horizontal presence policy.
  ScrollBarPolicy vertical{ScrollBarPolicy::AUTO};      ///< Vertical presence policy.
  ScrollPlacement placement{ScrollPlacement::RESERVED}; ///< Explicit bar placement.
  bool frame{};                                         ///< One-cell frame inset; ON_FRAME requires true.
  FrameSides frameSides{};                              ///< Present frame sides; ignored when frame is false.
};

/** @brief Owning local one-cell track; axis determines whether length is rows or columns. */
struct ScrollTrack {
  CellPoint origin;      ///< Local first track cell; zero for an absent track.
  std::int32_t length{}; ///< Nonnegative track cells, excluding any corner.
};

/** @brief Owning normalized placement result; every represented cell lies in the allocation.
 * The viewport defines the logical range. childClipSize excludes overlay ownership strips;
 * it therefore may be smaller than viewport only for explicitly selected OVERLAY.
 */
struct ScrollAreaGeometry {
  CellPoint viewportOrigin; ///< Local content origin after the optional frame inset.
  CellSize viewport;        ///< Nonnegative logical viewport extent.
  CellSize childClipSize;   ///< Nonnegative descendant clip size at viewportOrigin.
  ScrollTrack horizontal;   ///< Bottom horizontal track, excluding corners.
  ScrollTrack vertical;     ///< Right vertical track, excluding corners.
  CellPoint corner;         ///< Shared gutter/overlay corner when hasCorner is true.
  bool hasCorner{};         ///< False for ON_FRAME, whose corners belong to its outline.
  bool horizontalBar{};     ///< Policy result, even when a tiny allocation leaves no track.
  bool verticalBar{};       ///< Policy result, even when a tiny allocation leaves no track.
};

/** @brief Solve both bar policies together without cross-frame oscillation.
 * @param allocation Signed local allocation, normalized to nonnegative cell extents.
 * @param extent Fixed logical intrinsic content size; independent of gutters and offsets.
 * @param options Owning policies and optional one-cell frame selection.
 * @return Placement, or INVALID_ARGUMENT for unknown policies/placement, ON_FRAME without a frame,
 * or a possible ON_FRAME bar whose bottom/right side is absent.
 * @note Reserved AUTO uses a monotone least fixed point with at most two additions. Any thread;
 * constexpr, no allocation, callbacks, tree operations, mutation or invalidation.
 */
[[nodiscard]] constexpr std::expected<ScrollAreaGeometry, TreeError>
solveScrollLayout(CellSize allocation, ScrollExtent extent, ScrollLayoutOptions options = {}) noexcept {
  const auto validPolicy = [](ScrollBarPolicy policy) {
    return policy == ScrollBarPolicy::NEVER || policy == ScrollBarPolicy::AUTO || policy == ScrollBarPolicy::ALWAYS;
  };

  if (!validPolicy(options.horizontal) || !validPolicy(options.vertical) ||
      (options.placement != ScrollPlacement::RESERVED && options.placement != ScrollPlacement::OVERLAY &&
       options.placement != ScrollPlacement::ON_FRAME) ||
      (options.placement == ScrollPlacement::ON_FRAME &&
       (!options.frame || (options.horizontal != ScrollBarPolicy::NEVER && !options.frameSides.bottom) ||
        (options.vertical != ScrollBarPolicy::NEVER && !options.frameSides.right)))) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  const auto width = std::max(allocation.width, 0);
  const auto height = std::max(allocation.height, 0);
  const auto padding = options.frame ? options.frameSides.insets() : LayoutPadding{};
  const auto innerWidth = std::max(width - padding.left - padding.right, 0);
  const auto innerHeight = std::max(height - padding.top - padding.bottom, 0);
  ScrollAreaGeometry result;

  result.viewportOrigin = {std::min(width, padding.left), std::min(height, padding.top)};
  result.horizontalBar = options.horizontal == ScrollBarPolicy::ALWAYS;
  result.verticalBar = options.vertical == ScrollBarPolicy::ALWAYS;

  const auto viewport = [&] {
    return CellSize{std::max(innerWidth - (result.verticalBar ? 1 : 0), 0),
                    std::max(innerHeight - (result.horizontalBar ? 1 : 0), 0)};
  };

  // Start from forced bars on every solve. Monotone additions find the least fixed point.
  for (int pass = 0; pass < 3; ++pass) {
    const auto visible =
      options.placement == ScrollPlacement::RESERVED ? viewport() : CellSize{innerWidth, innerHeight};

    result.horizontalBar |=
      options.horizontal == ScrollBarPolicy::AUTO && extent.width > static_cast<std::uint64_t>(visible.width);
    result.verticalBar |=
      options.vertical == ScrollBarPolicy::AUTO && extent.height > static_cast<std::uint64_t>(visible.height);
  }

  result.viewport = options.placement == ScrollPlacement::RESERVED ? viewport() : CellSize{innerWidth, innerHeight};
  result.childClipSize = options.placement == ScrollPlacement::ON_FRAME ? result.viewport : viewport();

  if (options.placement == ScrollPlacement::ON_FRAME) {
    // Opposite sides sharing a collapsed row/column retain the legacy empty-track behavior.
    // A single bottom or right side can still own that physical row or column.
    if (result.horizontalBar && innerWidth != 0 && height > padding.top) {
      result.horizontal = {{padding.left, height - 1}, innerWidth};
    }

    if (result.verticalBar && innerHeight != 0 && width > padding.left) {
      result.vertical = {{width - 1, padding.top}, innerHeight};
    }

    return result;
  }

  if (result.horizontalBar && innerHeight != 0 && result.childClipSize.width != 0) {
    result.horizontal = {{result.viewportOrigin.x, result.viewportOrigin.y + innerHeight - 1},
                         result.childClipSize.width};
  }

  if (result.verticalBar && innerWidth != 0 && result.childClipSize.height != 0) {
    result.vertical = {{result.viewportOrigin.x + innerWidth - 1, result.viewportOrigin.y},
                       result.childClipSize.height};
  }

  result.hasCorner = result.horizontalBar && result.verticalBar && innerWidth != 0 && innerHeight != 0;

  if (result.hasCorner) {
    result.corner = {result.viewportOrigin.x + innerWidth - 1, result.viewportOrigin.y + innerHeight - 1};
  }

  return result;
}
} // namespace portals
