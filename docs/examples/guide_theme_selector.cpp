// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <cstdio>
#include <new>

/** @brief Choose and preview a catalog theme in the existing modal runner.
 * @return Standard runner status after terminal cleanup and final diagnostics.
 */
int main() try {
  using namespace portals;
  ui::ThemeSelectionRef themes;
  ui::Ref<ThemeSelector> selector;

  // clang-format off
  return ui::scene(ui::overlayHost(
    ui::column(
      ui::label("Ctrl+K, Ctrl+T opens Themes. F10 exits from the main form."),
      ui::button("Themes", [selector] { return selector.open(); })),
    ui::themeSelector(selector, themes)))
    | ui::themeSelection(themes, {{"nord", "Nord", nordTheme}, {"latte", "Latte", catppuccinLatteTheme}})
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
  // clang-format on
} catch (const std::bad_alloc &) {
  std::fputs("Unable to allocate the theme selector.\n", stderr);
  return 1;
}
