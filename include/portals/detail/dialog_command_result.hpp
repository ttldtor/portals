// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/managed_diagnostics.hpp>

namespace portals::ui {
namespace detail {
struct RefAccess;
struct MessageAccess;
struct NotificationAccess;
} // namespace detail

/** @brief Owning checked command that also permits the admitted short callback form.
 * This is a narrow facade result, not the full std::expected interface. Copies preserve the
 * existing report receipt without retaining a scene. Ignoring it does no destructor reporting.
 * Short failures inside an admitted managed callback use scene diagnostics and the runner stop boundary.
 * Outside that boundary, inspect this result explicitly; diagnostics alone do not promise runner STOP.
 */
class DialogCommandResult {
public:
  /** @return True when the command succeeded, including a live equal no-op. */
  explicit operator bool() const {
    return result_.has_value();
  }

  /** @return Independent owning typed error, including its opaque reporting receipt.
   * @pre The command failed. The returned value remains usable after scene retirement.
   */
  ManagedError error() const {
    return result_.error();
  }

  /** @return Independent exact expected value for explicit checked helpers or monadic use.
   * Extracting it does not introduce a new accepted managed callback return type.
   */
  std::expected<void, ManagedError> result() const {
    return result_;
  }

private:
  friend struct detail::RefAccess;
  friend struct detail::MessageAccess;
  friend struct detail::NotificationAccess;

  explicit DialogCommandResult(std::expected<void, ManagedError> result) : result_(std::move(result)) {
  }

  std::expected<void, ManagedError> result_;
};
} // namespace portals::ui
