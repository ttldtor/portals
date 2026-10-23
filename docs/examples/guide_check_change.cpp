// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Add a successful-change handler to a three-state checkbox.
 * @return Standard runner status after diagnostics and terminal cleanup.
 */
int main() {
  using namespace portals;

  ui::Ref<CheckBox> selection;
  ui::Ref<Label> status;

  // clang-format off
  auto scene = ui::scene(ui::vbox(
    ui::checkbox(selection, "Selection").threeState(),
    ui::label(status, "Unchecked")));

  return scene
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
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
