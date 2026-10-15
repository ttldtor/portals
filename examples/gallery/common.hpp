// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <portals/application.hpp>
#include <stdexcept>

namespace example {
/** @brief Unwrap a finite example operation or stop setup with a generic exception.
 * @tparam T Expected success type: void, or a movable value returned by ownership transfer.
 * @tparam E Expected error type; the helper deliberately does not format its value.
 * @param result Owning operation result; no reference into it is retained.
 * @return Moved success value, or no value for void.
 * @throws std::runtime_error If the operation failed; original error detail is lost.
 * Argument construction, allocation and success-value move exceptions also propagate.
 * This example-only fail-fast policy is not a production error-handling recommendation.
 */
template <class T, class E>
T take(std::expected<T, E> result) {
  if (!result) {
    throw std::runtime_error("example setup/preparation failed");
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

/** @brief Validate and copy example text under the receiving view's display policy.
 * @param value Borrowed UTF-8 bytes, consumed synchronously.
 * @param policy Width policy captured by the owning result.
 * @return Independent validated text.
 * @throws std::runtime_error If validation fails; allocation exceptions also propagate.
 */
inline portals::GraphemeText text(std::string_view value, portals::WidthPolicy policy = {}) {
  return take(portals::GraphemeText::from(value, policy));
}

/** @brief Recognize the examples' F10 or Ctrl+C exit shortcut.
 * @param event Borrowed event; repeats are accepted and releases are ignored.
 * @return True for an exit shortcut; no event or held-key state is consumed.
 */
inline constexpr bool quitKey(const portals::InputEvent &event) {
  const auto *key = std::get_if<portals::KeyEvent>(&event);

  return key && key->action != portals::KeyAction::RELEASE && (key->key == portals::InputKey::F10 || (key->modifiers.control && key->scalar == U'c'));
}
} // namespace example
