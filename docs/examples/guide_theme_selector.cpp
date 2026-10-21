// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Choose and preview a catalog theme in the existing modal runner.
 * @return Standard runner status after terminal cleanup and final diagnostics.
 */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::Ref<ThemeSelector> selector;

    return ui::scene(
      ui::column(
        ui::label("Ctrl+K, Ctrl+T opens Themes. "
                  "F10 exits from the main form."),
        ui::button("Themes", [selector] {
          return selector.open();
        })),
      ui::themeSelector(selector, {
        {"nord", "Nord", nordTheme},
        {"latte", "Latte", catppuccinLatteTheme}}))
      | ui::onKey("F10", ui::quit);
  } | ui::runApp();
  // clang-format on
}
