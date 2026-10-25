#include <portals/ui.hpp>
#include <portals/ui_style.hpp>
#include <stop_token>

int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::Ref<ThemeSelector> selector;

    return ui::scene(
      ui::column(
        ui::label("Preview palettes while editing. Apply saves the current choice; Cancel restores it."),
        ui::lineEdit("Keep typing while a theme is prepared"),
        ui::button("Themes", [selector] { return selector.open(); }),
        ui::label("Ctrl+K, Ctrl+T opens Themes. F10 exits.")),
      ui::themeSelector(selector, {
        {"nord", "Nord", nordTheme},
        {"latte", "Latte", catppuccinLatteTheme},
        ui::preparedTheme("contrast", "High contrast (prepared)",
          [](std::stop_token) -> std::expected<StyleSheet, StyleError> {
            // Finite immutable source; parse only on the explicit background path.
            return parsePss("* { foreground: #ffffff; background: #000000; }"
                            "Button { foreground: #ffff00; }");
          })}))
      | ui::style("LineEdit { foreground: #00ffff; }",
                  {.tier = StyleTier::USER})
      | ui::onKey("F10", ui::quit);
  }
  | ui::runApp();
  // clang-format on
}
