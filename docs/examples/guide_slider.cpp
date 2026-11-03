// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <format>

/** @brief Edit one scene-owned numeric value and observe live user changes.
 * @return Native runner status after terminal restoration and scene cleanup.
 */
int main() {
  using namespace portals;
  ui::Ref<Slider> slider;
  ui::Ref<Label> value;

  auto scene =
    ui::scene(ui::vbox(ui::slider(slider, 25), ui::label(value, "Value: 25"), ui::label("Arrows adjust | F10 quits")));

  return scene
    | ui::onChange(slider, [value](double number) {
        value.text() = std::format("Value: {:.0f}", number);
      })
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
}
