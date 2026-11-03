// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/numeric.hpp>

#include <algorithm>
#include <cmath>

namespace portals {

std::expected<NumericValue, NumericError> NumericValue::create(double initial, NumericRange range) {
  if (!std::isfinite(initial) || !std::isfinite(range.minimum) || !std::isfinite(range.maximum) ||
      !std::isfinite(range.step)) {
    return std::unexpected(NumericError::NOT_FINITE);
  }

  const auto span = range.maximum - range.minimum;

  if (!(range.minimum < range.maximum) || !std::isfinite(span)) {
    return std::unexpected(NumericError::INVALID_RANGE);
  }

  if (!(range.step > 0)) {
    return std::unexpected(NumericError::INVALID_STEP);
  }

  if (range.step < span &&
      (!(range.minimum + range.step > range.minimum) || !(range.maximum - range.step < range.maximum))) {
    return std::unexpected(NumericError::UNREPRESENTABLE_STEP);
  }

  if (initial < range.minimum || initial > range.maximum) {
    return std::unexpected(NumericError::OUT_OF_RANGE);
  }

  return NumericValue(initial, range);
}

std::expected<NumericValue, NumericError> NumericValue::withValue(double candidate) const {
  if (!std::isfinite(candidate)) {
    return std::unexpected(NumericError::NOT_FINITE);
  }

  if (candidate < range_.minimum || candidate > range_.maximum) {
    return std::unexpected(NumericError::OUT_OF_RANGE);
  }

  return NumericValue(candidate, range_);
}

double detail::nudgeNumeric(const NumericValue &value, int direction, std::uint64_t steps) noexcept {
  const auto range = value.range();
  const auto endpoint = direction > 0 ? range.maximum : range.minimum;
  const auto distance = direction > 0 ? endpoint - value.value() : value.value() - endpoint;

  if (steps == 0 || direction == 0) {
    return value.value();
  }

  if (range.step >= distance / static_cast<double>(steps)) {
    return endpoint;
  }

  const auto amount = range.step * static_cast<double>(steps);

  return std::clamp(direction > 0 ? value.value() + amount : value.value() - amount, range.minimum, range.maximum);
}

double detail::snapNumeric(NumericRange range, double candidate) noexcept {
  if (!(candidate > range.minimum)) {
    return range.minimum;
  }

  if (!(candidate < range.maximum)) {
    return range.maximum;
  }

  const auto offset = candidate - range.minimum;
  const auto remainder = std::fmod(offset, range.step);
  const auto lower = std::clamp(candidate - remainder, range.minimum, range.maximum);
  const auto remaining = range.maximum - lower;
  const auto upper = range.step >= remaining ? range.maximum : std::min(lower + range.step, range.maximum);

  return candidate - lower < upper - candidate ? lower : upper;
}
} // namespace portals
