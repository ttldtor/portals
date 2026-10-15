// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <iostream>
#include <portals/application.hpp>

namespace example {
/** @brief Obtain a static diagnostic label without allocating.
 * @param phase Application phase to describe, including an unknown numeric value.
 * @return Static label; "unknown" for a value without a named phase.
 */
inline constexpr std::string_view phaseName(portals::ApplicationPhase phase) noexcept {
  using enum portals::ApplicationPhase;

  switch (phase) {
  case CREATE:
    return "create";
  case OPEN:
    return "open";
  case INPUT:
    return "input";
  case DISPATCH:
    return "dispatch";
  case PREPARE:
    return "prepare";
  case RENDER:
    return "render";
  case SUBMIT:
    return "submit";
  case FLUSH:
    return "flush";
  case STOP:
    return "stop";
  case CLOSE:
    return "close";
  case CONTROL_LATENCY:
    return "control latency observer";
  }

  return "unknown";
}

/** @brief Report a typed application failure on stderr for a CLI exit status.
 * @param result Borrowed runner result, including any independent restoration error.
 * @return Zero on success, one on failure; success produces no diagnostic.
 * Stream exceptions propagate if enabled by the caller.
 */
inline int report(const std::expected<portals::ApplicationStats, portals::ApplicationError> &result) {
  if (result) {
    return 0;
  }

  const auto &error = result.error();

  std::cerr << "Application failed in " << phaseName(error.phase) << ": ";
  std::visit(
    [](const auto &cause) {
      using T = std::decay_t<decltype(cause)>;

      if constexpr (std::same_as<T, portals::TerminalError>) {
        std::cerr << "TerminalError=" << static_cast<int>(cause.kind) << " native=" << cause.native_code
                  << " rollback=" << cause.restoration_code;
      } else if constexpr (std::same_as<T, portals::RenderError>) {
        std::cerr << "RenderError=" << static_cast<int>(cause.kind);
      } else if constexpr (std::same_as<T, portals::ApplicationFailure>) {
        std::cerr << "ApplicationFailure=" << static_cast<int>(cause);
      } else if constexpr (std::same_as<T, portals::TreeError>) {
        std::cerr << "TreeError=" << static_cast<int>(cause);
      } else if constexpr (std::same_as<T, portals::SchedulerError>) {
        std::cerr << "SchedulerError=" << static_cast<int>(cause);
      } else if constexpr (std::same_as<T, portals::InputError>) {
        std::cerr << "InputError=" << static_cast<int>(cause);
      } else {
        std::cerr << "FrameError=" << static_cast<int>(cause);
      }
    },
    error.cause);

  if (error.restoration) {
    std::cerr << " restoration.kind=" << static_cast<int>(error.restoration->kind)
              << " restoration.native=" << error.restoration->native_code;
  }

  std::cerr << '\n';

  return 1;
}

} // namespace example
