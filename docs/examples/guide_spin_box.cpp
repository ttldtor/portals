// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

#include <format>

int main() {
  using namespace portals;
  ui::Ref<SpinBox> quantity, priority, discount;
  ui::Ref<Label> status;
  ui::Ref<ThemeSelector> themes;

  // clang-format off
  return ui::scene(
    ui::vbox(
      ui::label("Order settings | type a value or use the step buttons"),
      ui::vbox(
        ui::hbox(ui::label("Quantity (standard)").width(24),
          ui::spinBox(quantity, 25).range(0, 100).skin("standard").width(22)),
        ui::hbox(ui::label("Priority (compact)").width(24),
          ui::spinBox(priority, 2).range(0, 5).skin("compact").width(22)),
        ui::hbox(ui::label("Discount (arrows)").width(24),
          ui::spinBox(discount, 10).range(0, 50).skin("arrows").width(22)))
        .framed("rounded", "Order"),
      ui::vbox(
        ui::hbox(ui::label("Shipping (read-only)").width(24),
          ui::spinBox(3).skin("standard").width(22).readOnly()),
        ui::hbox(ui::label("Reserved (unavailable)").width(24),
          ui::spinBox(12).skin("arrows").width(22).enabled(false)))
        .framed("rounded", "Availability"),
      ui::hbox(
        ui::button("Reset", [=] {
          quantity.value() = 25;
          priority.value() = 2;
          discount.value() = 10;
          status.text() = "Defaults restored";
        }),
        ui::button("Themes", [themes] { return themes.open(); })),
      ui::label(status, "Adjust an order value."),
      ui::label("Tab: focus | Up/Down: adjust | F10: quit"))
      .maxWidth(64),
    ui::themeSelector(themes, {
      {"nord", "Nord", nordTheme},
      {"latte", "Latte", catppuccinLatteTheme}}))
    | ui::theme(nordTheme())
    | ui::onChange(quantity, [status](double value) {
        status.text() = std::format("Quantity: {:.0f}", value);
      })
    | ui::onChange(priority, [status](double value) {
        status.text() = std::format("Priority: {:.0f}", value);
      })
    | ui::onChange(discount, [status](double value) {
        status.text() = std::format("Discount: {:.0f}%", value);
      })
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
