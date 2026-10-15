// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "tests/support/examples/replay/terminal.hpp"
#include <portals/ui.hpp>

#include <iostream>

namespace staticStylesConsumer {
using namespace portals;
inline constexpr ThemeTokens FIRST{{220, 225, 232}, {24, 28, 36}, {75, 180, 220}, {125, 130, 140}, {}, {75, 180, 220}};
inline constexpr ThemeTokens SECOND{{255, 255, 85}, {0, 0, 170}, {0, 170, 170}, {170, 170, 170}, {}, {255, 255, 255}};
inline constexpr RgbColor OVERRIDE{40, 60, 80};

inline void report(const ManagedError &error) {
  std::cerr << "Managed phase=" << static_cast<int>(error.phase);

  std::visit(
    [](const auto &cause) {
      using T = std::remove_cvref_t<decltype(cause)>;

      if constexpr (std::same_as<T, TreeError>) {
        std::cerr << " tree=" << static_cast<int>(cause);
      } else if constexpr (std::same_as<T, StyleError>) {
        std::cerr << " style=" << static_cast<int>(cause.cause) << ' ' << cause.message;
      } else {
        std::cerr << " application-phase=" << static_cast<int>(cause.phase) << " cause=" << cause.cause.index()
                  << " restoration=" << cause.restoration.has_value();
      }
    },
    error.cause);

  if (error.cleanup) {
    std::cerr << " cleanup=" << static_cast<int>(*error.cleanup);
  }

  std::cerr << '\n';
}

// This helper and the existing replay terminal are part of both complete client footprints.
template <class Factory> int run(int argc, char **argv, Factory initialize) {
  const bool failure = argc == 2 && std::string_view(argv[1]) == "--failure";

  if (argc > 2 || (argc == 2 && !failure)) {
    std::cerr << "Usage: static styles consumer [--failure]\n";

    return 1;
  }

  example::ReplayTerminal terminal({{50, "\r", {}}, {100, "\r", {}}, {150, "\r", CellSize{38, 8}}}, 200, {40, 10});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  using Controller = typename decltype(initialize(std::declval<ManagedScene &>()))::value_type;
  std::optional<Controller> controller;
  std::optional<ManagedError> actionError;
  ui::Ref<Button> button;
  std::size_t changes{};
  auto action = [&]() -> std::expected<void, TreeError> {
    auto result = changes == 0   ? controller->palette(SECOND)
                  : changes == 1 ? controller->background(OVERRIDE)
                                 : controller->width(12);

    if (!result) {
      actionError = result.error();

      return std::unexpected(TreeError::CALLBACK_FAILED);
    }

    ++changes;

    return {};
  };
  auto scene = ui::scene(ui::column({.gap = 1, .padding = {1, 1, 1, 1}}, ui::label("Theme parameters"),
                                    ui::button("Change").id("action").reference(button).onClick(action)),
                         options);
  scene.configure([&](ManagedScene &owner) -> std::expected<void, ManagedError> {
    auto created = initialize(owner);

    if (!created) {
      return std::unexpected(created.error());
    }

    controller = std::move(*created);

    if (failure) {
      auto invalid = controller->width(-1);

      return invalid ? std::expected<void, ManagedError>{} : std::unexpected(invalid.error());
    }

    return {};
  });
  const auto result = scene.run(terminal);

  if (failure) {
    const auto *cause = result ? nullptr : std::get_if<StyleError>(&result.error().cause);

    if (!cause || cause->cause != TreeError::INVALID_ARGUMENT || result.error().phase != ManagedPhase::STYLE ||
        button.text().get()) {
      std::cerr << "Typed style failure or scene cleanup was lost\n";

      return 1;
    }

    return 0;
  }

  if (!result || actionError) {
    report(actionError ? *actionError : result.error());

    return 1;
  }

  if (!terminal.closed || changes != 3 || button.text().get()) {
    std::cerr << "Live theme update or scene lifetime differed\n";

    return 1;
  }

  std::cout << "frames=" << result->frames << " changes=" << changes << '\n' << terminal.output;

  return 0;
}
} // namespace staticStylesConsumer
