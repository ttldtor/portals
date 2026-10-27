// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_menu.hpp>

#include "tests/support/examples/replay/terminal.hpp"

#include <array>
#include <iostream>

int main() {
  using namespace portals;

  example::ReplayTerminal terminal(
    {{10, "\r", {}}, {20, "\x1b[B", {}}, {30, " ", {}}, {40, "\x13", {}}, {50, "\x11", {}}}, 70, {40, 8});
  ui::SceneOptions options;
  ui::Ref<Menu> menu;
  ui::ActionSet actions;
  int first{}, second{};
  auto save = actions.action("save", "Save 界🙂", [&] {
    ++first;
  });
  auto other = actions.action("other", "Other", [&] {
    ++second;
  });
  const std::array entries{save, other};

  options.clock = terminal.clock();

  auto scene =
    ui::scene(ui::vbox(ui::menu(menu, entries).fitWidth().minWidth(12).maxWidth(24).framed("rounded", "Commands"),
                       ui::button(save)),
              options) |
    ui::onKey("Ctrl+S", save) | ui::onKey("Ctrl+Q", ui::quit);
  const auto result = scene.run(terminal);

  if (!result || !terminal.closed || first != 2 || second != 1 || save.snapshot()) {
    std::cerr << "Menu consumer: run=" << static_cast<bool>(result) << " closed=" << terminal.closed
              << " first=" << first << " second=" << second << '\n';
    return 1;
  }

  return 0;
}
