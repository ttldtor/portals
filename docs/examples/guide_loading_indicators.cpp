// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_style.hpp>

/** @brief Start and stop a retained loading indicator through an observing reference.
 * @return Standard runner status after typed diagnostics and terminal restoration.
 */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::Ref<Spinner> loading;

    return ui::scene(ui::vbox(
      ui::spinner(loading).preset(SpinnerPreset::BRAILLE).label("Loading quotes").width(30),
      ui::hbox(
        ui::button("Start", [loading] { loading.start(); }),
        ui::button("Stop", [loading] { loading.stop(); })),
      ui::button("Exit", [] { ui::quit(); })))
      | ui::style("Spinner { foreground: #62b4d7; }")
      | ui::onKey("F10 | Ctrl+C", ui::quit);
  } | ui::runApp();
  // clang-format on
}
