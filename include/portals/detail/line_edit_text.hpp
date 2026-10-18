// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <string_view>

namespace portals::detail {
/** @brief Detect the exact separators excluded by the one-line editor contract.
 * @param bytes Borrowed UTF-8 candidate; byte validation is performed separately.
 * @return True for CR, LF, LINE SEPARATOR or PARAGRAPH SEPARATOR, without allocating.
 */
constexpr bool hasLineSeparator(std::string_view bytes) noexcept {
  return bytes.find_first_of("\r\n") != std::string_view::npos || bytes.contains("\xe2\x80\xa8") ||
         bytes.contains("\xe2\x80\xa9");
}
} // namespace portals::detail
