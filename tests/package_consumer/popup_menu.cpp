// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/popup_menu.hpp>
#include <portals/ui.hpp>

#include "tests/support/examples/replay/terminal.hpp"

#include <iostream>

int main() {
  using namespace portals;

  // Allow the body to arrange, then decode open/branch/leaf from one runner input chunk.
  example::ReplayTerminal terminal({{40, "\rma", {}}, {90, "\x11", {}}}, 120, {50, 10});
  ui::SceneOptions options;
  ui::Ref<Button> opener;
  ui::Ref<PopupMenu> menu;
  ui::ActionSet actions;
  int calls{}, opens{};
  std::size_t bodyOutput{};
  auto leaf = actions.action("leaf", "Leaf", [&] {
    ++calls;
  });

  options.clock = terminal.clock();

  auto scene = ui::scene(options, ui::button(opener, "Open").onClick([menu, opener, &opens, &bodyOutput, &terminal] {
                 ++opens;
                 bodyOutput = terminal.output.size();

                 return menu.open(opener).result();
               }),
                         ui::popupMenu(menu, {ui::submenu("More", {ui::menuItem(leaf, U'a')}, U'm')})) |
               ui::onKey("Ctrl+Q", ui::quit);
  const auto result = scene.run(terminal);

  if (!result || !terminal.closed || opens != 1 || calls != 1 || bodyOutput == 0) {
    std::cerr << "Popup consumer: run=" << static_cast<bool>(result) << " closed=" << terminal.closed
              << " opens=" << opens << " calls=" << calls << " body-output=" << bodyOutput;

    if (!result) {
      std::cerr << " phase=" << static_cast<int>(result.error().phase)
                << " cause-index=" << result.error().cause.index();

      if (const auto *cause = std::get_if<TreeError>(&result.error().cause)) {
        std::cerr << " tree=" << static_cast<int>(*cause);
      }
    }

    std::cerr << '\n';

    return 1;
  }

  std::cout << "Installed popup: one input chunk, one action, closed terminal\n";

  return 0;
}
