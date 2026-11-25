// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <concepts>
#include <portals/virtual_rows.hpp>
#include <utility>
static_assert(
  std::same_as<decltype(std::declval<const portals::VirtualRows<int> &>().rowCount()), std::optional<std::uint64_t>>);
