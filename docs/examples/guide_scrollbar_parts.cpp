// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_style.hpp>

/** @brief Ordinary content wider and taller than the two viewports.
 * @return A fresh content description for one scroll area.
 */
auto sections() {
  using namespace portals;

  return ui::vbox(ui::label("01 Overview / release checklist"), ui::label("02 Accounts / connection status"),
                  ui::label("03 Orders / pending requests"), ui::label("04 Positions / current exposure"),
                  ui::label("05 History / recent activity"), ui::label("06 Connections / retry policy"),
                  ui::label("07 Preferences / saved settings"), ui::label("08 About / version information"),
                  ui::label("09 Reports / export options"), ui::label("10 Alerts / active subscriptions"),
                  ui::label("11 Tools / diagnostics"), ui::label("12 Help / keyboard shortcuts"))
    .width(36);
}

/** @brief Compare independent presets and explicit thumb segments.
 * @return Standard runner status after diagnostics and terminal cleanup.
 */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    return ui::scene(ui::vbox(
      ui::label("Independent parts | Tab: area | Arrows: scroll | F10"),
      ui::grid(
        ui::scrollArea(sections()).width(26).height(10)
          .framed("double", "Block").titleDecoration("angle")
          .scrollOnFrame().arrows()
          .scrollTrack("frame").scrollThumb("block").scrollArrows("ascii"),
        ui::scrollArea(sections())
          .settings({.scrollbar = {
            .horizontal = {.thumbStart = "[", .thumbBody = "=", .thumbEnd = "]", .thumbSingle = "#"},
            .vertical = {.thumbStart = "▄", .thumbBody = "█", .thumbEnd = "▀", .thumbSingle = "█"}}})
          .id("segments").width(26).height(10)
          .framed("rounded", "Segments").titleDecoration("square")
          .scrollOnFrame().arrows())
        .columns(2).columnGap(2).fitWidth().fitHeight()))
      | ui::theme(nordTheme())
      | ui::style(R"pss(
          #segments {
            scrollbar-track-pattern: minimal;
            scrollbar-thumb-pattern: ascii;
            scrollbar-arrow-pattern: triangle;
            scrollbar-thumb-foreground: #62b4d7;
            scrollbar-thumb-bold: true;
            scrollbar-thumb-start-foreground: #f2c879;
            scrollbar-thumb-end-foreground: #f2c879;
            scrollbar-thumb-body-bold: false;
            scrollbar-thumb-single-foreground: #f2c879;
          }
        )pss")
      | ui::onKey(key::F10, ui::quit);
  } | ui::runApp();
  // clang-format on
}
