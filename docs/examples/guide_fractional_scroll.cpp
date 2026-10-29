// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Show eighth-cell scrollbar edges with ordinary keyboard and mouse input.
 * @return Standard runner status after terminal restoration and error reporting.
 */
int main() {
  using namespace portals;

  // clang-format off
  return ui::scene(ui::vbox(
    ui::label("Use arrows or the wheel; watch the thumb edges. F10 exits."),
    ui::scrollArea(ui::vbox(
      ui::label("01 Overview -- the thumb can begin between terminal cells"),
      ui::label("02 Accounts -- both axes use the same prepared profile"),
      ui::label("03 Orders -- ordinary text stays on the cell grid"),
      ui::label("04 Positions -- one key press can move a partial edge"),
      ui::label("05 History -- arrows retain their ordinary step"),
      ui::label("06 Connections -- dragging retains its original anchor"),
      ui::label("07 Notifications -- no animation timer is needed"),
      ui::label("08 Reports -- the frame owns its rounded corners"),
      ui::label("09 Activity -- the thumb has a separate block shape"),
      ui::label("10 Preferences -- explicit theme colors keep priority"),
      ui::label("11 Appearance -- an eighth is a coverage step"),
      ui::label("12 Languages -- whole graphemes remain intact"),
      ui::label("13 Storage -- the scroll position keeps its exact integer"),
      ui::label("14 Security -- a partial edge does not change focus"),
      ui::label("15 Diagnostics -- ordinary errors reach the runner"),
      ui::label("16 Help -- try both horizontal and vertical movement"),
      ui::label("17 About -- F10 or Exit restores the terminal"))
      .width(78))
      .framed("rounded", "Fractional thumb: 1/8 cell")
      .scrollOnFrame().arrows()
      .scrollbar("fractional-block")
      .width(54).height(10),
    ui::button("Exit", [] { ui::quit(); })))
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
  // clang-format on
}
