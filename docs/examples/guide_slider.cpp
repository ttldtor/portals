// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

#include <format>

/** @brief Adjust settings with all built-in slider skins and an owning custom glyph set.
 * @return Native runner status after checked setup, terminal restoration and cleanup.
 */
int main() {
  using namespace portals;
  ui::Ref<Slider> volume, balance, brightness, contrast, custom;
  ui::Ref<Label> status;
  ui::Ref<ThemeSelector> themes;

  // clang-format off
  return ui::scene(
    ui::vbox(
      ui::label("Studio settings | drag a handle or use the keyboard"),
      ui::vbox(
        ui::hbox(ui::label("Volume (rounded)").width(24),
          ui::slider(volume, 35).skin("rounded").width(28)),
        ui::hbox(ui::label("Balance (line)").width(24),
          ui::slider(balance, 0).range(-100, 100).skin("line").width(28)),
        ui::hbox(ui::label("Brightness (blocks)").width(24),
          ui::slider(brightness, 70).skin("blocks").width(28)),
        ui::hbox(ui::label("Contrast (ascii)").width(24),
          ui::slider(contrast, 60).skin("ascii").width(28)))
        .framed("rounded", "Sound and display"),
      ui::vbox(
        ui::hbox(ui::label("Custom glyphs").width(24),
          ui::slider(custom, 50).glyphs({"·", "=", "◆"}).width(28)),
        ui::hbox(ui::label("Policy limit (read-only)").width(24),
          ui::slider(40).skin("rounded").width(28).readOnly()),
        ui::hbox(ui::label("HDR gain (unavailable)").width(24),
          ui::slider(80).skin("blocks").width(28).enabled(false)))
        .framed("rounded", "Profiles and appearance"),
      ui::hbox(
        ui::button("Reset", [=] {
          volume.value() = 35;
          balance.value() = 0;
          brightness.value() = 70;
          contrast.value() = 60;
          custom.value() = 50;
          status.text() = "Defaults restored";
        }),
        ui::button("Themes", [themes] { return themes.open(); })),
      ui::label(status, "Adjust a slider to preview its value."),
      ui::label("Tab: focus | arrows: adjust | Home/End: bounds | F10: quit"))
      .maxWidth(64),
    ui::themeSelector(themes, {
      {"nord", "Nord", nordTheme},
      {"latte", "Latte", catppuccinLatteTheme}}))
    | ui::theme(nordTheme())
    | ui::onChange(volume, [status](double value) {
        status.text() = std::format("Volume: {:.0f}%", value);
      })
    | ui::onChange(balance, [status](double value) {
        status.text() = std::format("Balance: {:+.0f}", value);
      })
    | ui::onChange(brightness, [status](double value) {
        status.text() = std::format("Brightness: {:.0f}%", value);
      })
    | ui::onChange(contrast, [status](double value) {
        status.text() = std::format("Contrast: {:.0f}%", value);
      })
    | ui::onChange(custom, [status](double value) {
        status.text() = std::format("Custom preview: {:.0f}%", value);
      })
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
