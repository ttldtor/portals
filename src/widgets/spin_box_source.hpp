// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <portals/numeric.hpp>
#include <string_view>

namespace portals::detail {
// Complete locale-independent finite decimal draft parsing, without allocation.
std::expected<double, NumericError> parseSpinBoxText(std::string_view text) noexcept;
} // namespace portals::detail
