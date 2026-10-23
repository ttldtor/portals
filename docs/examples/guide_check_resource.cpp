// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Add a change handler to a resource-declared three-state CheckBox.
 * @return Standard runner status after diagnostics and terminal cleanup.
 */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    auto scene = ui::scene(R"(scene {
  VBox {
    CheckBox#selection { text = "Selection" cycle = THREE_STATE }
    Label#status { "Unchecked" }
  }
})",
      ui::SceneResourceOptions{.format = ui::SceneResourceFormat::CONFIGURATION});
    auto selection = scene.get<CheckBox>("selection");
    auto status = scene.get<Label>("status");

    return std::move(scene)
      | ui::onChange(selection, [status](CheckState value) {
          switch (value) {
          case CheckState::UNCHECKED:
            status.text() = "Unchecked";
            break;
          case CheckState::CHECKED:
            status.text() = "Checked";
            break;
          case CheckState::MIXED:
            status.text() = "Mixed";
            break;
          }
        })
      | ui::onKey("F10 | Ctrl+C", ui::quit);
  } | ui::runApp();
  // clang-format on
}
