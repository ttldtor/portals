// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstdint>
#include <expected>

namespace portals {

/** @brief Allocation-free numeric validation cause, preserved by managed diagnostics. */
enum class NumericError {
  NOT_FINITE,           ///< A value, bound or step is NaN or infinite.
  INVALID_RANGE,        ///< Bounds are unordered or their positive span is not finite.
  INVALID_STEP,         ///< The user increment is not positive.
  UNREPRESENTABLE_STEP, ///< A sub-range increment cannot advance at one of the bounds.
  OUT_OF_RANGE,         ///< An explicit value is outside the inclusive bounds.
  INITIAL_CONFLICT      ///< An external model was combined with an explicit numeric override.
};

/** @brief Owning inclusive bounds and user increment; validated together with a numeric value. */
struct NumericRange {
  double minimum{};    ///< Inclusive finite lower bound, default zero.
  double maximum{100}; ///< Inclusive finite upper bound with a finite positive span.
  double step{1};      ///< Positive representable user increment; external values need not lie on its grid.

  /** @param other Complete configuration to compare. @return True when all numeric fields are equal. */
  bool operator==(const NumericRange &other) const = default;
};

/** @brief Small owning numeric snapshot whose public operations preserve its finite range invariant.
 * No scene, widget, callback or mutable alias is retained. Values between user ticks are valid.
 */
class NumericValue {
public:
  /** @brief Construct zero in the default 0..100 range with step one. */
  NumericValue() = default;

  /** @param initial Explicit finite value in the inclusive range; never clamped or rounded.
   * @param range Finite ordered bounds and a positive representable user increment.
   * @return Valid owning value or the first allocation-free numeric validation cause.
   */
  static std::expected<NumericValue, NumericError> create(double initial, NumericRange range = {});

  /** @return The finite numeric value, with no owner or thread dependency. */
  double value() const noexcept {
    return value_;
  }

  /** @return An owning copy of the validated bounds and user increment. */
  NumericRange range() const noexcept {
    return range_;
  }

  /** @param candidate Finite replacement within these bounds; no snapping is performed.
   * @return New owning value or the original numeric cause; this value never changes.
   */
  std::expected<NumericValue, NumericError> withValue(double candidate) const;

  /** @param other Complete owning snapshot to compare. @return True when value and range are equal. */
  bool operator==(const NumericValue &other) const = default;

private:
  NumericValue(double value, NumericRange range) : value_(value), range_(range) {
  }

  double value_{};
  NumericRange range_{};
};

namespace detail {
/** @param value Valid owning starting point. @param direction Positive to increase, negative to decrease.
 * @param steps Positive number of increments; overflow saturates before arithmetic.
 * @return Finite in-range user value; programmatic off-grid offsets are preserved.
 */
double nudgeNumeric(const NumericValue &value, int direction, std::uint64_t steps = 1) noexcept;

/** @param range Valid numeric configuration. @param candidate Finite or saturated pointer candidate.
 * @return Nearest minimum-anchored tick or endpoint, with ties toward the larger point.
 * Constant work is independent of the number of ticks; no integer quotient conversion is performed.
 */
double snapNumeric(NumericRange range, double candidate) noexcept;
} // namespace detail
} // namespace portals
