// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/line_edit.hpp>

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

/** @param mode Immutable editor display policy.
 * @return Whether the enum identifies a supported presentation.
 */
constexpr bool validLineEditDisplayMode(LineEditDisplayMode mode) noexcept {
  return mode == LineEditDisplayMode::NORMAL || mode == LineEditDisplayMode::PASSWORD ||
         mode == LineEditDisplayMode::NO_ECHO;
}

/** @brief Prepare placeholder storage before publishing an internal managed model.
 * @param options Owning settings; placeholder bytes are released after successful preparation.
 * @return Stationary segmented presentation or exact text error; allocation failures propagate to the checked caller.
 * The caller checks display mode and line separators separately, which have no TextError category.
 */
inline std::expected<LineEditPlaceholder, TextError> prepareLineEditPlaceholder(LineEditOptions &options) {
  if (!options.placeholder || options.placeholder->empty()) {
    options.placeholder.reset();
    return LineEditPlaceholder{};
  }

  auto text = GraphemeText::from(*options.placeholder, options.widthPolicy, options.limits);

  if (!text) {
    return std::unexpected(text.error());
  }

  auto prepared = std::make_unique<GraphemeText>(std::move(*text));
  options.placeholder.reset();

  return LineEditPlaceholder{std::move(prepared)};
}
} // namespace portals::detail
