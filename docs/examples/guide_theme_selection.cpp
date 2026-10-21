// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Preview catalog themes, apply a successful choice or restore the prior theme.
 * @return Existing runner status after terminal cleanup and final diagnostics.
 */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::ThemeSelectionRef themes;

    return ui::scene(ui::column(
      ui::label("Choose a theme to preview"),
      ui::button("Preview Nord", [themes] {
        return themes.preview("nord");
      }),
      ui::button("Preview Latte", [themes] {
        return themes.preview("latte");
      }),
      ui::button("Apply preview", [themes] {
        return themes.commit();
      }),
      ui::button("Cancel preview", [themes] {
        return themes.cancel();
      }),
      ui::label("Apply requires a preview. "
                "Cancel restores the prior theme. F10 exits.")))
      | ui::themeSelection(themes, {
          {"nord", "Nord", nordTheme},
          {"latte", "Latte", catppuccinLatteTheme}})
      | ui::onKey("F10", ui::quit);
  } | ui::runApp();
  // clang-format on
}
