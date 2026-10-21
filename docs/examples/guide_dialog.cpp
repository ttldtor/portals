// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Open an editable modal panel without a nested event loop.
 * @return Standard runner status after terminal cleanup and typed diagnostics.
 */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::Ref<Dialog> details;
    return ui::scene(
      ui::vbox(
        ui::label("Unhandled Esc closes Details; F10 exits from the main form."),
        ui::button("Details", [details] { details.open(); }),
        ui::button("Exit", [] { ui::quit(); })),
      ui::dialog(details, "Connection settings", ui::vbox(
        ui::lineEdit("Primary feed"),
        ui::checkbox("Reconnect automatically", true),
        ui::button("Close", [details] { details.dismiss(); }))))
      | ui::onKey("F10", ui::quit);
  } | ui::runApp();
  // clang-format on
}
