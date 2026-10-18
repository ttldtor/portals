// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Compose the same checkbox using explicit managed setup instead of its factory.
 * @return Standard runner status after typed setup/runtime error reporting and terminal cleanup.
 */
int main() {
  using namespace portals;

  auto selection = ui::control<CheckBox>("Selection", {}, {}).setup([](ManagedScene &scene, Node<CheckBox> node) {
    return scene.state(CheckState::MIXED).and_then([&scene, node](ManagedState<CheckState> state) {
      return scene.bind(node, state, {}, {.cycle = CheckCycle::THREE_STATE});
    });
  });

  return ui::scene(ui::vbox(ui::label("CheckBox | Space changes state | F10 quit"), std::move(selection))) |
         ui::onKey(key::F10, ui::quit) | ui::runApp();
}
