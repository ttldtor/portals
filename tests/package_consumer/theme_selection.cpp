// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/theme_selection.hpp>

#include <portals/ui.hpp>

#include "tests/support/examples/replay/terminal.hpp"

#include <iostream>

namespace {
portals::StyleSheet theme(portals::RgbColor foreground) {
  portals::StyleRule rule;
  rule.declarations.foreground = foreground;

  return {.rules = {rule}};
}
} // namespace

int main() try {
  using namespace portals;
  example::ReplayTerminal terminal(
    {{100, "\x1bOQ", {}}, {200, "\x1bOR", {}}, {300, "\x1bOS", {}}, {400, "\x1b[15~", {}}, {500, "\x1b[21~", {}}}, 2000,
    {40, 8});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  ui::ThemeSelectionRef themes;

  std::size_t factories{};
  std::size_t steps{};
  bool restored{};
  std::shared_ptr<const ui::ThemeSelectionSnapshot> committed;

  auto scene = ui::scene(ui::column(ui::label("Installed theme model"), ui::button("Action")), options);
  scene |
    ui::themeSelection(themes, {{"red", "Red",
                                 [&] {
                                   ++factories;

                                   return theme({220, 10, 10});
                                 }},
                                {"blue", "Blue",
                                 [&] {
                                   ++factories;

                                   return theme({10, 10, 220});
                                 }}}) |
    ui::onKey("F2",
              [&] {
                ++steps;

                return themes.preview("red");
              }) |
    ui::onKey("F3",
              [&] {
                ++steps;

                return themes.preview("blue");
              }) |
    ui::onKey("F4",
              [&] {
                ++steps;

                return themes.cancel();
              }) |
    ui::onKey("F5",
              [&]() -> std::expected<void, ManagedError> {
                auto snapshot = themes.snapshot();

                if (!snapshot) {
                  return std::unexpected(snapshot.error());
                }

                restored = (*snapshot)->status == ui::ThemeSelectionStatus::IDLE && !(*snapshot)->lastCommittedId;

                auto previewed = themes.preview("red");

                if (!previewed) {
                  return previewed;
                }

                ++steps;

                auto applied = themes.commit();

                if (!applied) {
                  return applied;
                }

                auto result = themes.snapshot();

                if (!result) {
                  return std::unexpected(result.error());
                }

                committed = *result;

                return {};
              }) |
    ui::onKey("F10", ui::quit);

  auto result = scene.run(terminal);
  auto snapshot = themes.snapshot();
  const auto *closed = !snapshot ? std::get_if<StyleError>(&snapshot.error().cause) : nullptr;

  if (!result || factories != 3 || steps != 4 || !restored || !terminal.closed || !closed ||
      closed->cause != TreeError::CLOSED || !committed || committed->status != ui::ThemeSelectionStatus::IDLE ||
      committed->lastCommittedId != "red" || committed->catalog->choices.size() != 2) {
    std::cerr << "installed theme selection runner/action/cleanup contract failed\n";

    return 1;
  }

  return 0;
} catch (const std::exception &failure) {
  std::cerr << failure.what() << '\n';

  return 1;
}
