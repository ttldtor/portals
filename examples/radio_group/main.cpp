// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

/** @brief Choose model-backed power and notification modes with ordinary keyboard/mouse input.
 * @return Standard native runner status after typed reporting and terminal restoration.
 */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::Ref<RadioGroup> power;
    ui::Ref<RadioGroup> alerts;
    ui::Ref<Label> summary;
    ui::Ref<ThemeSelector> selector;

    return ui::scene(
      ui::vbox(
        ui::hbox(LinearOptions{.gap = 2},
          ui::label("Device settings | Tab changes group | F10 closes"),
          ui::button("Themes", [selector] { return selector.open(); })),
        ui::label("Arrows move. Space/Enter selects. Notifications use immediate selection."),
        ui::hbox(LinearOptions{.gap = 2},
          ui::radioGroup(std::vector<RadioOption>{
            {"Fast"}, {"Balanced"}, {"Quiet"}, {"Unavailable", false}, {"Legacy", true, false}})
            .reference(power).id("power").classes({"settings"}).fitWidth().minWidth(24).maxWidth(40)
            .framed("rounded", "Power mode"),
          ui::vbox(LinearOptions{.gap = 1},
            ui::radioGroup(alerts, {"All messages", "Important only", "Silent"})
              .immediate().fitWidth().framed("rounded", "Notifications"),
            ui::radioGroup({"Automatic updates", "Manual updates"})
              .readOnly().fitWidth().framed("rounded", "Read-only policy"))),
        ui::spacer(),
        ui::label(summary, "Power: Fast | Choose another mode to update this summary"),
        ui::label("Disabled choices stay visible; hidden choices occupy no row.")),
      ui::themeSelector(selector, {
        {"nord", "Nord", nordTheme},
        {"latte", "Latte", catppuccinLatteTheme}}))
      | ui::onChange(power, [summary](const RadioGroupValue &value) {
          summary.text() = text("Power: {}", value.selectedText());
        })
      | ui::onChange(alerts, [summary](const RadioGroupValue &value) {
          summary.text() = text("Notifications: {}", value.selectedText());
        })
      | ui::theme(nordTheme())
      | ui::onKey(key::F10, ui::quit);
  } | ui::runApp();
  // clang-format on
}
