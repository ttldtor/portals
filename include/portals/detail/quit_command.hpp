// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <concepts>
#include <source_location>

namespace portals::ui {
namespace detail {
/** @brief Exact overload-selection target for a bare short command in existing callback templates. */
struct QuitCommand {
  /** @brief Source-location-aware short command signature used for exact overload selection. */
  using Function = void (*)(std::source_location);

  /** @param command Exact short command; no default arguments survive a function pointer.
   * @param origin Implicit conversion site, replaced by an explicit registration origin when supplied.
   */
  QuitCommand(Function command, std::source_location origin = std::source_location::current()) noexcept
      : command(command), origin(origin) {
  }

  /** @brief Invoke the selected short command with its retained registration origin. */
  void operator()() const {
    command(origin);
  }

  Function command;            ///< Exact source-location-aware command target.
  std::source_location origin; ///< Original registration site, never an invocation-internal location.
};

/** @tparam Callback Existing owning action.
 * @param callback Only the internal bare-command adapter receives a registration origin.
 * @param origin Original registration site, preserved through pipe deferral.
 */
template <class Callback> void commandOrigin(Callback &callback, std::source_location origin) noexcept {
  if constexpr (std::same_as<Callback, QuitCommand>) {
    callback.origin = origin;
  }
}
} // namespace detail

} // namespace portals::ui
