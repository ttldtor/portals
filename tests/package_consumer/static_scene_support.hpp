// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "examples/replay/terminal.hpp"
#include <portals/ui.hpp>

#include <iostream>

namespace staticConsumer {
using namespace portals;

inline constexpr LinearOptions FRAME{.gap = 1, .padding = {1, 1, 1, 1}};
inline constexpr ControlStyle CAPTION_STYLE{
  .normal = {.foreground = RgbColor{30, 180, 240}, .background = {}, .bold = true}};
inline constexpr SizePolicy FIELD_SIZE{.width = {.minimum = 4, .mode = SizeMode::FLEX, .weight = 2}, .height = {}};

struct Outcome {
  int clicks{};
  std::string applied;
  std::optional<ManagedError> actionError;
};

inline void report(const ManagedError &error) {
  std::cerr << "Scene failure in phase " << static_cast<int>(error.phase);

  if (const auto *cause = std::get_if<TreeError>(&error.cause)) {
    std::cerr << ": tree error " << static_cast<int>(*cause);
  }

  if (error.cleanup) {
    std::cerr << "; cleanup error " << static_cast<int>(*error.cleanup);
  }

  std::cerr << '\n';
}

// Both complete consumers use the same runtime, input, policies, errors and cleanup.
// This helper is part of each consumer's full compactness footprint.
template <class Factory> int runScene(int argc, char **argv, Factory factory) {
  const bool failure = argc == 2 && std::string_view(argv[1]) == "--failure";

  if (argc > 2 || (argc == 2 && !failure)) {
    std::cerr << "Usage: static scene consumer [--failure]\n";

    return 1;
  }

  example::ReplayTerminal terminal({{50,
                                     "\x01"
                                     "Ada\t\r",
                                     {}},
                                    {100, "", CellSize{36, 8}}},
                                   150, {40, 10});
  ui::SceneOptions options;
  ui::Ref<LineEdit> field;
  ui::Ref<Label> status;
  Outcome outcome;

  options.clock = terminal.clock();

  auto action = [field, status, failure, &outcome]() -> std::expected<void, TreeError> {
    if (failure) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }

    auto value = field.text().get();

    if (!value) {
      outcome.actionError = value.error();

      return std::unexpected(TreeError::CALLBACK_FAILED);
    }

    outcome.applied = *value;
    ++outcome.clicks;

    auto changed = status.text().set(*value).result();

    if (!changed) {
      outcome.actionError = changed.error();

      return std::unexpected(TreeError::CALLBACK_FAILED);
    }

    return {};
  };
  auto scene = ui::scene(factory(field, status, std::move(action)), options);
  StyleSheet theme;
  StyleRule buttonStyle;

  buttonStyle.selector.id = "apply";
  buttonStyle.declarations.foreground = RgbColor{240, 200, 20};
  theme.rules.push_back(buttonStyle);

  scene.theme(theme);

  const auto result = scene.run(terminal);

  if (failure) {
    const auto *cause = result ? nullptr : std::get_if<TreeError>(&result.error().cause);

    if (!cause || *cause != TreeError::INVALID_GEOMETRY || result.error().phase != ManagedPhase::INPUT ||
        !terminal.closed) {
      std::cerr << "Typed callback failure or cleanup was lost\n";

      return 1;
    }

    return 0;
  }

  if (!result || outcome.actionError) {
    report(outcome.actionError ? *outcome.actionError : result.error());

    return 1;
  }

  if (!terminal.closed || outcome.clicks != 1 || outcome.applied != "Ada" || field.text().get()) {
    std::cerr << "Static form behavior or scene lifetime differed\n";

    return 1;
  }

  std::cout << "frames=" << result->frames << " clicks=" << outcome.clicks << " applied=" << outcome.applied << '\n'
            << terminal.output;

  return 0;
}
} // namespace staticConsumer
