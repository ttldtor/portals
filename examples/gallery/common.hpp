// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <iostream>
#include <portals/application.hpp>
#include <stdexcept>

namespace example {
template <class T, class E> T take(std::expected<T, E> result) {
  if (!result) {
    throw std::runtime_error("example setup/preparation failed");
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

inline portals::GraphemeText text(std::string_view value, portals::WidthPolicy policy = {}) {
  return take(portals::GraphemeText::from(value, policy));
}

inline std::string_view phaseName(portals::ApplicationPhase phase) noexcept {
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

inline bool quitKey(const portals::InputEvent &event) {
  const auto *key = std::get_if<portals::KeyEvent>(&event);

  return key && key->action != portals::KeyAction::RELEASE &&
         (key->key == portals::InputKey::F10 || (key->modifiers.control && key->scalar == U'c'));
}
} // namespace example
