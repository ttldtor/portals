// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Keep the main form interactive while a help dialog is open.
 * @return Standard runner status after terminal cleanup and diagnostics.
 */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::Ref<Dialog> help;

    return ui::scene(
      ui::vbox(
        ui::label("F1: Help; F10: Exit"),
        ui::lineEdit("Keep editing here"),
        ui::button("Help", [help] { help.open(); })),
      ui::dialog(help, "Help", ui::vbox(
        ui::label("The main form stays available."),
        ui::button("Close", [help] { help.dismiss(); })))
        .nonModal())
      | ui::onKey("F1", [help] { help.open(); })
      | ui::onKey("F10", ui::quit);
  } | ui::runApp();
  // clang-format on
}
