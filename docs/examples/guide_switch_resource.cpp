// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Load ordinary Switch properties and attach native bool behavior.
 * @return Standard native runner status after terminal restoration and typed reporting.
 */
int main() {
  using namespace portals;
  ui::SceneResourceOptions options;
  options.format = ui::SceneResourceFormat::CONFIGURATION;
  auto scene = ui::scene(R"scene(
    scene { VBox {
      Switch#network { checked=true text="Network" classes="settings" }
      Switch#updates { checked=true text="Managed updates" readOnly=true }
      Button#enable { text="Enable network" }
      Label#status { text="Waiting for a user change" }
    } }
  )scene",
                         options);
  const auto network = scene.get<Switch>("network");
  const auto enable = scene.get<Button>("enable");
  const auto status = scene.get<Label>("status");
  // clang-format off
  return std::move(scene)
    | ui::onClick(enable,[network] { network.checked() = true; })
    | ui::onChange(network,[status](bool on) { status.text() = on ? "User enabled network" : "User disabled network"; })
    | ui::onKey(key::F10,ui::quit)
    | ui::runApp();
  // clang-format on
}
