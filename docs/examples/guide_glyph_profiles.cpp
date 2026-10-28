// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_style.hpp>

/** @return Ordinary content overflowing both axes of each panel. */
auto profileContent() {
  using namespace portals;

  return ui::vbox(ui::label("01 Overview / current workspace and connection"),
                  ui::label("02 Accounts / balances and pending settlements"),
                  ui::label("03 Orders / recent activity and open requests"),
                  ui::label("04 Positions / exposure and reference prices"),
                  ui::label("05 History / completed operations and exports"),
                  ui::label("06 Connections / endpoints and retry policy"),
                  ui::label("07 Preferences / presentation and key bindings"),
                  ui::label("08 Reports / saved views and export options"),
                  ui::label("09 Alerts / subscriptions and notifications"),
                  ui::label("10 Help / keyboard navigation and shortcuts"))
    .width(50);
}

/** @param title Literal title identifying the selected map.
 * @param profile Explicit map; NF selection requires a matching active terminal font.
 * @return Scroll area with its existing frame, arrows and two independent axis projections.
 */
auto profilePanel(std::string_view title, std::string_view profile) {
  using namespace portals;

  return ui::scrollArea(profileContent())
    .glyphProfile(profile)
    .glyphFallback("unicode")
    .framed("rounded", title)
    .titleDecoration("profile")
    .scrollOnFrame()
    .arrows()
    .width(28)
    .height(8);
}

/** @brief Compare named forms; the existing runner owns input, redraw and terminal cleanup.
 * @return Standard runner exit status after F10 or a reported application error.
 */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    return ui::scene(ui::vbox(
      ui::label("Named glyphs | Tab: panel | Arrows: scroll | F10"),
      ui::label("NF: select a Nerd Font Mono in the terminal first."),
      ui::grid(
        profilePanel("NF Rounded", "nf-rounded").classes("nf"),
        profilePanel("NF Chevron", "nf-chevron").classes("nf"),
        profilePanel("NF Slash", "nf-slash").classes("nf"),
        profilePanel("NF Backslash", "nf-backslash").classes("nf"),
        profilePanel("Unicode", "unicode"),
        profilePanel("ASCII", "ascii"))
        .columns(3).columnGap(2).rowGap(1).fitWidth().fitHeight(),
      ui::label("Vertical NF caps use Unicode inset forms; geometry stays the same.")))
      | ui::theme(nordTheme())
      // Example color overrides are independent of the selected glyph map.
      | ui::style(R"pss(
          ScrollArea {
            scrollbar-thumb-foreground: #88c0d0;
            scrollbar-thumb-start-foreground: #f2c879;
            scrollbar-thumb-end-foreground: #f2c879;
          }
          .nf { frame-title-foreground: #f2c879; }
        )pss")
      | ui::onKey(key::F10, ui::quit);
  } | ui::runApp();
  // clang-format on
}
