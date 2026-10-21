// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Explore nested ordinary forms with bars sharing their own frame edges.
 * @return Standard runner status after terminal cleanup and typed diagnostics.
 */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    constexpr ScrollAreaOptions FRAMED{.placement = ScrollPlacement::ON_FRAME, .frame = BorderOptions{}};
    return ui::scene(ui::vbox(
      ui::label("Tab changes focus; wheel over either frame; F10 exits."),
      ui::scrollArea(ui::vbox(
        ui::label("Connection settings"),
        ui::scrollArea(ui::vbox(
          ui::lineEdit("Primary feed"),
          ui::lineEdit("Backup feed"),
          ui::checkbox("Use credentials", true),
          ui::checkbox("Restore subscriptions", true),
          ui::checkbox("Reconnect automatically", true),
          ui::checkbox("Connection diagnostics")), FRAMED)
          | ui::height(6),
        ui::label("Application settings"),
        ui::checkbox("Dark appearance", true),
        ui::checkbox("Notifications", true),
        ui::checkbox("Keyboard hints", true),
        ui::checkbox("Usage statistics")), FRAMED)
        | ui::height(9),
      ui::button("Exit", [] { ui::quit(); })))
      | ui::onKey("F10", ui::quit);
  }
    | ui::runApp();
  // clang-format on
}
