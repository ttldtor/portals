// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Bind one bool and update it through the same observing property.
 * @return Standard native runner status after terminal restoration and typed reporting.
 */
int main() {
  using namespace portals;

  ui::Ref<Switch> network;
  ui::Ref<Label> status;
  // clang-format off
  auto scene = ui::scene([=](ManagedScene &owner) {
    return ui::vbox(
      ui::toggle(network, "Network", owner.state(false)),
      ui::button("Enable").onClick([network] { network.checked() = true; }),
      ui::label(status, "Waiting for a user change"));
  });

  return scene
    | ui::onChange(network, [status](bool on) { status.text() = on ? "User enabled network" : "User disabled network"; })
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
  // clang-format on
}
