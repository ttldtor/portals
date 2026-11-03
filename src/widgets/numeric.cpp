// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/numeric.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace portals {
namespace {
static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits == 53 &&
              std::numeric_limits<double>::max_exponent == 1024);

// Finite binary64 values are integers in units of 2^-1074. 33 words also hold
// the sum of two maximum finite magnitudes; no floating quotient/index is used.
constexpr std::size_t DYADIC_WORDS = 33;
constexpr unsigned WORD_BITS = 64;
constexpr std::uint64_t FRACTION_MASK = (std::uint64_t{1} << 52) - 1;
constexpr std::uint64_t SIGN_MASK = std::uint64_t{1} << 63;

struct Dyadic {
  std::array<std::uint64_t, DYADIC_WORDS> words{};
  bool negative{};
};

int magnitudeOrder(const Dyadic &left, const Dyadic &right) noexcept {
  for (std::size_t index = DYADIC_WORDS; index-- > 0;) {
    if (left.words[index] != right.words[index]) {
      return left.words[index] < right.words[index] ? -1 : 1;
    }
  }

  return 0;
}

int dyadicOrder(const Dyadic &left, const Dyadic &right) noexcept {
  if (left.negative != right.negative) {
    return left.negative ? -1 : 1;
  }

  const auto order = magnitudeOrder(left, right);

  return left.negative ? -order : order;
}

Dyadic addMagnitude(Dyadic left, const Dyadic &right) noexcept {
  std::uint64_t carry{};

  for (std::size_t index = 0; index < DYADIC_WORDS; ++index) {
    const auto sum = left.words[index] + right.words[index];
    const auto carried = sum + carry;

    carry = sum < left.words[index] || carried < sum;
    left.words[index] = carried;
  }

  return left;
}

// The caller has already established left >= right in magnitude.
Dyadic subtractMagnitude(Dyadic left, const Dyadic &right) noexcept {
  std::uint64_t borrow{};

  for (std::size_t index = 0; index < DYADIC_WORDS; ++index) {
    const auto difference = left.words[index] - right.words[index];
    const auto borrowed = difference - borrow;

    borrow = left.words[index] < right.words[index] || difference < borrow;
    left.words[index] = borrowed;
  }

  return left;
}

Dyadic addDyadic(Dyadic left, Dyadic right) noexcept {
  if (left.negative == right.negative) {
    return addMagnitude(left, right);
  }

  const auto order = magnitudeOrder(left, right);

  return order == 0 ? Dyadic{} : order > 0 ? subtractMagnitude(left, right) : subtractMagnitude(right, left);
}

Dyadic subtractDyadic(Dyadic left, Dyadic right) noexcept {
  if (magnitudeOrder(right, {}) != 0) {
    right.negative = !right.negative;
  }

  return addDyadic(left, right);
}

Dyadic decodeDyadic(double value) noexcept {
  const auto bits = std::bit_cast<std::uint64_t>(value);
  const auto exponent = static_cast<unsigned>((bits >> 52) & 0x7ff);
  const auto significand = (bits & FRACTION_MASK) | (exponent ? std::uint64_t{1} << 52 : 0);
  const auto shift = exponent ? exponent - 1 : 0;
  const auto word = shift / WORD_BITS;
  const auto offset = shift % WORD_BITS;
  Dyadic result;

  result.negative = significand != 0 && (bits & SIGN_MASK) != 0;
  result.words[word] = significand << offset;

  if (offset != 0) {
    result.words[word + 1] = significand >> (WORD_BITS - offset);
  }

  return result;
}

unsigned dyadicBits(const Dyadic &value) noexcept {
  for (std::size_t index = DYADIC_WORDS; index-- > 0;) {
    if (value.words[index] != 0) {
      return static_cast<unsigned>(index) * WORD_BITS + std::bit_width(value.words[index]);
    }
  }

  return 0;
}

Dyadic shiftMagnitude(const Dyadic &value, unsigned shift) noexcept {
  Dyadic result;
  const auto words = shift / WORD_BITS;
  const auto bits = shift % WORD_BITS;

  for (std::size_t index = 0; index + words < DYADIC_WORDS; ++index) {
    result.words[index + words] |= value.words[index] << bits;

    if (bits != 0 && index + words + 1 < DYADIC_WORDS) {
      result.words[index + words + 1] |= value.words[index] >> (WORD_BITS - bits);
    }
  }

  return result;
}

Dyadic dyadicRemainder(Dyadic value, const Dyadic &step) noexcept {
  const auto valueBits = dyadicBits(value);
  const auto stepBits = dyadicBits(step);

  if (valueBits < stepBits) {
    return value;
  }

  auto shift = valueBits - stepBits;
  auto divisor = shiftMagnitude(step, shift);

  // Binary long remainder enumerates quotient bits, never grid ticks.
  for (;;) {
    if (magnitudeOrder(value, divisor) >= 0) {
      value = subtractMagnitude(value, divisor);
    }

    if (shift-- == 0) {
      return value;
    }

    std::uint64_t carry{};

    for (std::size_t index = DYADIC_WORDS; index-- > 0;) {
      const auto next = divisor.words[index] << (WORD_BITS - 1);

      divisor.words[index] = (divisor.words[index] >> 1) | carry;
      carry = next;
    }
  }
}

bool dyadicLowBits(const Dyadic &value, unsigned count) noexcept {
  const auto whole = count / WORD_BITS;
  const auto bits = count % WORD_BITS;

  for (unsigned index = 0; index < whole; ++index) {
    if (value.words[index] != 0) {
      return true;
    }
  }

  return bits != 0 && (value.words[whole] & ((std::uint64_t{1} << bits) - 1)) != 0;
}

double roundDyadic(const Dyadic &value) noexcept {
  const auto length = dyadicBits(value);
  const auto sign = value.negative ? SIGN_MASK : 0;

  if (length <= 52) {
    return std::bit_cast<double>(sign | value.words[0]);
  }

  auto shift = length - 53;
  const auto word = shift / WORD_BITS;
  const auto bits = shift % WORD_BITS;
  auto significand = value.words[word] >> bits;

  if (bits != 0 && word + 1 < DYADIC_WORDS) {
    significand |= value.words[word + 1] << (WORD_BITS - bits);
  }

  if (shift != 0) {
    const auto guard = shift - 1;
    const bool halfway = ((value.words[guard / WORD_BITS] >> (guard % WORD_BITS)) & 1) != 0;

    if (halfway && (dyadicLowBits(value, guard) || (significand & 1) != 0)) {
      ++significand;
    }
  }

  auto exponent = length - 52;

  if (significand == (std::uint64_t{1} << 53)) {
    significand >>= 1;
    ++exponent;
  }

  return std::bit_cast<double>(sign | (static_cast<std::uint64_t>(exponent) << 52) | (significand & FRACTION_MASK));
}
} // namespace

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

  const auto minimum = decodeDyadic(range.minimum);
  const auto maximum = decodeDyadic(range.maximum);
  const auto value = decodeDyadic(candidate);
  const auto step = decodeDyadic(range.step);
  auto distance = dyadicRemainder(subtractDyadic(value, minimum), step);
  auto selected = subtractDyadic(value, distance);
  auto upper = addDyadic(selected, step);

  if (dyadicOrder(upper, maximum) > 0) {
    upper = maximum;
  }

  const auto upperDistance = subtractDyadic(upper, value);

  // Compare exact distances; only the chosen real tick/endpoint is rounded once.
  return roundDyadic(magnitudeOrder(distance, upperDistance) < 0 ? selected : upper);
}
} // namespace portals
