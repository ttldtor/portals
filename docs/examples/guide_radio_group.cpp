// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Keep one selected value with a short model-backed client.
 * @return Standard runner status after typed error reporting and terminal cleanup.
 */
int main() {
  using namespace portals;

  ui::Ref<RadioGroup> mode;
  ui::Ref<Label> selected;

  // clang-format off
  auto scene = ui::scene([=](ManagedScene &owner) {
    return ui::vbox(
      ui::label("Arrows move | Space/Enter selects | F10 closes"),
      ui::radioGroup(mode, owner.radioState({"Fast", "Balanced", "Quiet"})),
      ui::label(selected, "Fast"));
  });

  return scene
    | ui::onChange(mode, [selected](const RadioGroupValue &value) {
        selected.text() = value.selectedText();
      })
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
  // clang-format on
}
