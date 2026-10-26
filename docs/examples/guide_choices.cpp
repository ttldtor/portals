// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Select an owning source row without manual popup or binding management. */
int main() {
  using namespace portals;

  ui::Ref<ComboBox> city;
  ui::Ref<Label> chosen;

  // clang-format off
  auto scene = ui::scene(ui::vbox(
    ui::comboBox(city, {"London", "Prague", "Tokyo"}),
    ui::label(chosen, "Choose a city")));

  return scene
    | ui::onSelect(city, [chosen](ChoiceSelection value) {
        chosen.text() = value ? text("Selected: {}", value.text()) : text("No selection");
      })
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
  // clang-format on
}
