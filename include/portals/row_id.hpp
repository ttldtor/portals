// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstdint>

namespace portals {
/** @brief Provider-assigned row identity, independent of position; increment generation on reuse. */
struct RowId {
  std::uint64_t value{};      ///< Nonzero identity within one model.
  std::uint64_t generation{}; ///< Nonzero generation; never wrap or reuse an exhausted pair.
  /** @brief Compare both identity components.
   * @param other Row identity to compare.
   * @return True when all compared fields are equal.
   */
  bool operator==(const RowId &other) const noexcept = default;
};
} // namespace portals
