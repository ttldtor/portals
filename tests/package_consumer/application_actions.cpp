// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "tests/support/examples/replay/terminal.hpp"
#include <portals/ui.hpp>
#include <portals/ui_action.hpp>

int main() {
  using namespace portals;

  example::ReplayTerminal terminal({{10, "\r", {}}, {20, "\x13", {}}, {30, "\x11", {}}}, 50, {40, 8});
  ui::SceneOptions options;
  ui::ActionSet actions;
  int saves{};
  auto save = actions.action("save", "Save", [&] {
    ++saves;
  });

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::vbox(ui::button(save), ui::button(save)), options) | ui::onKey("Ctrl+S", save) |
               ui::onKey("Ctrl+Q", ui::quit);
  const auto result = scene.run(terminal);
  const auto retired = save.snapshot();

  // The recipe must retain a factory-local declaration before its lazy managed mount.
  example::ReplayTerminal factoryTerminal({{10, "\r", {}}}, 30, {40, 8});
  ui::SceneOptions factoryOptions;
  int factoryCalls{};

  factoryOptions.clock = factoryTerminal.clock();

  const auto factoryStatus = [&] {
    ui::ActionSet local;
    auto localAction = local.action("local", "Local", [&] {
      ++factoryCalls;
    });

    return ui::scene(ui::vbox(ui::button(localAction)), factoryOptions);
  } | ui::runApp(factoryTerminal);

  return result && terminal.closed && saves == 2 && !retired && retired.error() == TreeError::CLOSED &&
             factoryStatus == 0 && factoryTerminal.closed && factoryCalls == 1
           ? 0
           : 1;
}
