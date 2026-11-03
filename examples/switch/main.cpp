// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

/** @brief Demonstrate model-backed switches and concise programmatic property assignments.
 * @return Standard native runner status after terminal restoration and typed reporting.
 */
int main() {
  using namespace portals;
  ui::Ref<Switch> network;
  ui::Ref<Switch> policy;
  ui::Ref<Label> summary;
  auto changes = std::make_shared<unsigned>(0);

  // clang-format off
  auto scene = ui::scene([=](ManagedScene &owner) {
    return ui::vbox(LinearOptions{.gap = 1},
      ui::label("Device settings | Tab changes focus | F10 closes"),
      ui::label("Space, Enter or a click toggles the focused switch."),
      ui::toggle(network, "Network", owner.state(true)).id("network").classes({"settings"}).fitWidth(),
      ui::toggle("Notifications", false).fitWidth(),
      ui::toggle(policy, "Managed updates", true).settings({.readOnly = true}).fitWidth(),
      ui::toggle("Unavailable device", false).enabled(false).fitWidth(),
      ui::hbox(LinearOptions{.gap = 2},
        ui::button("Turn network on").onClick([network] { network.checked() = true; }),
        ui::button("Turn network off").onClick([network] { network.checked() = false; }),
        ui::button("Apply managed update").onClick([policy] { policy.checked() = false; })),
      ui::label(summary, "User changes: 0"),
      ui::label("Programmatic buttons also update read-only values; they do not increment the user counter."));
  });

  return scene
    | ui::onChange(network, [summary, changes](bool on) {
        summary.text() = text("Last user change: {} | User changes: {}", on ? "on" : "off", ++*changes);
      })
    | ui::theme(nordTheme())
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
  // clang-format on
}
