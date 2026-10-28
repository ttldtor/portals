// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

#include <array>

/** @brief Choose a destination and delivery method; draft text and accepted rows stay distinct. */
int main() {
  using namespace portals;

  static constexpr std::array THEMES{nordTheme, gruvboxTheme, catppuccinLatteTheme};
  ui::Ref<ComboBox> city;
  ui::Ref<DropDown> delivery;
  ui::Ref<DropDown> palette;
  ui::Ref<Label> chosen;
  ui::Ref<Label> deliveryInfo;

  // clang-format off
  auto scene = ui::scene(ui::vbox(
    ui::label("Choose a destination and delivery method.").id("heading"),
    ui::hbox(ui::label("City").width(12),
      ui::comboBox(city, {
        "Amsterdam", "Athens", "Auckland", "Austin", "Bangkok", "Barcelona", "Beijing", "Berlin",
        "Bogotá", "Boston", "Bratislava", "Brussels", "Budapest", "Buenos Aires", "Cairo", "Cape Town",
        "Chicago", "Copenhagen", "Delhi", "Dublin", "Edinburgh", "Helsinki", "Hong Kong", "Istanbul",
        "Jakarta", "Kyiv", "Lisbon", "Ljubljana", "London", "Madrid", "Melbourne", "Mexico City",
        "Montréal", "München", "Nairobi", "Oslo", "Paris", "Prague", "Reykjavík", "Riga", "Rome",
        "São Paulo", "Seoul", "Singapore", "Stockholm", "Sydney", "Tallinn", "Tokyo", "Toronto",
        "Vancouver", "Vienna", "Vilnius", "Warsaw", "Wellington", "Zürich"}).width(40)),
    ui::hbox(ui::label("Delivery").width(12),
      ui::dropDown(delivery, {"Courier", "Pickup point", "Postal service", "Digital delivery"},
        {.initial = 0}).width(40)),
    ui::hbox(ui::label("Palette").width(12),
      ui::dropDown(palette, {"Nord", "Gruvbox", "Catppuccin Latte"}, {.initial = 0}).width(40)),
    ui::label(chosen, "City: no selection").id("selected"),
    ui::label(deliveryInfo, "Delivery: waiting for the initial choice"),
    ui::hbox(
      ui::button("Use Amsterdam", [city] { return city.select(0); }),
      ui::button("Clear selection", [city] { return city.clearSelection(); }),
      ui::button("Quit", [] { ui::quit(); })),
    ui::label("Type a city prefix, then Down / Enter. Esc closes the popup; Ctrl+Z undoes text."),
    ui::label("The list scrolls through every city. Tab moves focus; F10 exits.")
  ).width(78).framed("rounded", "Shipping desk"));

  return scene
    | ui::theme(nordTheme())
    | ui::style("#heading { bold: true; } #selected { bold: true; }")
    | ui::onSelect(city, [chosen](ChoiceSelection value) {
        chosen.text() = value ? text("City: {}", value.text()) : text("City: no selection");
      })
    | ui::onSelect(delivery, [deliveryInfo](ChoiceSelection value) {
        deliveryInfo.text() = value ? text("Delivery: {}", value.text()) : text("Delivery: no selection");
      })
    | ui::onSelect(palette, [&scene](ChoiceSelection value) {
        if (value) {
          scene.theme(THEMES[value.index()]());
        }
      })
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
  // clang-format on
}
