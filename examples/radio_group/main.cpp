// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

/** @brief Choose model-backed power and notification modes with ordinary keyboard/mouse input.
 * @return Standard native runner status after typed reporting and terminal restoration.
 */
int main() {
  using namespace portals;

  ui::Ref<RadioGroup> power;
  ui::Ref<RadioGroup> alerts;
  ui::Ref<Label> summary;

  // clang-format off
  auto scene = ui::scene([=](ManagedScene &owner) {
    return ui::vbox(
      ui::label("Device settings | Tab changes group | F10 closes"),
      ui::label("Arrows select a mode. Space keeps one selection. Click any available row."),
      ui::hbox(LinearOptions{.gap = 2},
        ui::radioGroup(power, owner.radioState(std::vector<RadioOption>{
          {"Fast"}, {"Balanced"}, {"Quiet"}, {"Unavailable", false}, {"Legacy", true, false}}))
          .id("power").classes({"settings"}).fitWidth().minWidth(24).maxWidth(40)
          .framed("rounded", "Power mode"),
        ui::vbox(LinearOptions{.gap = 1},
          ui::radioGroup(alerts, {"All messages", "Important only", "Silent"})
            .fitWidth().framed("rounded", "Notifications"),
          ui::radioGroup({"Automatic updates", "Manual updates"})
            .settings({.readOnly = true}).fitWidth().framed("rounded", "Read-only policy"))),
      ui::spacer(),
      ui::label(summary, "Power: Fast | Choose another mode to update this summary"),
      ui::label("Disabled choices stay visible; hidden choices occupy no row."));
  });

  return scene
    | ui::onChange(power, [summary](const RadioGroupValue &value) {
        summary.text() = text("Power: {}", value.selectedText());
      })
    | ui::onChange(alerts, [summary](const RadioGroupValue &value) {
        summary.text() = text("Notifications: {}", value.selectedText());
      })
    | ui::theme(nordTheme())
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
  // clang-format on
}
