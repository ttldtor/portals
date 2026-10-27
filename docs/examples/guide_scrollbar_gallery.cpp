// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_style.hpp>

/** @brief Ordinary content deliberately wider and taller than each viewport. */
auto sections() {
  using namespace portals;

  return ui::vbox(ui::label("01 Overview / release checklist"), ui::label("02 Accounts / connection status"),
                  ui::label("03 Orders / pending requests"), ui::label("04 Positions / current exposure"),
                  ui::label("05 History / recent activity"), ui::label("06 Connections / retry policy"),
                  ui::label("07 Preferences / saved settings"), ui::label("08 About / version information"))
    .width(40);
}

/** @brief Compare on-frame scrollbars; Tab chooses an area, arrows scroll, F10 exits. */
int main() {
  using namespace portals;

  // clang-format off
  return ui::scene(ui::vbox(
    ui::label("Scrollbars on frames | Tab: area | Arrows: scroll | F10: quit"),
    ui::grid(
      ui::scrollArea(sections()).width(30).height(6)
        .framed("single", "Legacy").titleDecoration("square")
        .scrollOnFrame().scrollbar("legacy"),
      ui::scrollArea(sections()).width(30).height(6)
        .framed("double", "Frame").titleDecoration("angle")
        .scrollOnFrame().scrollbar("frame"),
      ui::scrollArea(sections()).width(30).height(6)
        .framed("rounded", "Minimal").titleDecoration("round")
        .scrollOnFrame().scrollbar("minimal"),
      ui::scrollArea(sections()).width(30).height(6)
        .framed("ascii", "ASCII").titleDecoration("spaced")
        .scrollOnFrame().scrollbar("ascii"))
      .columns(2).columnGap(2).rowGap(1).fitHeight()))
    | ui::theme(nordTheme())
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
  // clang-format on
}
