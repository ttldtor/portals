// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "common.hpp"
#include <portals/managed.hpp>

namespace example {
// Render diagnostics only after run has restored the terminal. Keep the typed
// managed cause available rather than collapsing it into a generic exception.
inline int report(const std::expected<portals::ApplicationStats, portals::ManagedError> &result) {
  if (result) {
    return 0;
  }

  const auto &error = result.error();

  std::cerr << "Managed operation failed in phase " << static_cast<int>(error.phase) << ": ";
  std::visit(
    [](const auto &cause) {
      using T = std::decay_t<decltype(cause)>;

      if constexpr (std::same_as<T, portals::ApplicationError>) {
        report(std::expected<portals::ApplicationStats, portals::ApplicationError>(std::unexpected(cause)));
      } else if constexpr (std::same_as<T, portals::TreeError>) {
        std::cerr << "TreeError=" << static_cast<int>(cause);
      } else {
        std::cerr << "StyleError=" << static_cast<int>(cause.cause) << " " << cause.message;
      }
    },
    error.cause);

  if (error.cleanup) {
    std::cerr << " cleanup=" << static_cast<int>(*error.cleanup);
  }

  if (error.application) {
    std::cerr << " runner: ";
    report(std::expected<portals::ApplicationStats, portals::ApplicationError>(std::unexpected(*error.application)));
  }

  std::cerr << '\n';

  return 1;
}
} // namespace example
