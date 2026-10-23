// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_style.hpp>

/** @brief Run a native ordinary scroll area with standard input and cleanup.
 * @return Standard runner exit status after terminal restoration and typed diagnostics.
 */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    return ui::scene(ui::vbox(
      ui::label("Choose a section; use the wheel or drag the thumb."),
      ui::scrollArea(ui::vbox(
        ui::button("Overview"),
        ui::button("Accounts"),
        ui::button("Orders"),
        ui::button("Positions"),
        ui::button("History"),
        ui::button("Connections"),
        ui::button("Preferences"),
        ui::button("About")))
        .framed("rounded", "Sections")
        .titleDecoration("square")
        .scrollOnFrame()
        .width(28)
        .height(6),
      ui::button("Exit", [] { ui::quit(); })))
      | ui::style(R"pss(
          ScrollArea {
            frame-line-foreground: #7d8797;
            frame-title-foreground: #62b4d7;
            frame-ends-foreground: #62b4d7;
            frame-title-bold: true;
          }
        )pss")
      | ui::onKey("F10", ui::quit);
  } | ui::runApp();
  // clang-format on
}
