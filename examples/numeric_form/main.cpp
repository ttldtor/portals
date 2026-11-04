// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

/** @brief Edit numeric settings with immediate and deferred publication.
 * @return Native application status after terminal restoration and scene cleanup.
 */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::Ref<SpinBox> quantity;
    ui::Ref<SpinBox> price;
    ui::Ref<SpinBox> discount;
    ui::Ref<Label> status;
    ui::Ref<ThemeSelector> selector;

    return ui::scene(
      ui::vbox(LinearOptions{.gap = 1},
        ui::label("Order settings"),
        ui::hbox(ui::label("Quantity").width(14),
          ui::spinBox(quantity, 4).range(1, 100).step(1)),
        ui::hbox(ui::label("Unit price").width(14),
          ui::spinBox(price, 12.5).range(0, 1000).step(0.25).editMode(EditMode::ON_COMPLETION)),
        ui::hbox(ui::label("Discount %").width(14),
          ui::spinBox(discount, 0).range(0, 100).step(0.5).editMode(EditMode::EXPLICIT)),
        ui::label("Quantity applies immediately; price on Enter or blur; discount on Enter."),
        ui::label("Up/Down steps; PageUp/PageDown step by 10. Escape restores only the draft."),
        ui::label("Ctrl+Z undoes; incomplete numbers stay visible. Tab visits each editor once."),
        ui::label(status, "Ready"),
        ui::hbox(LinearOptions{.gap = 2},
          ui::button("Reset", [=] {
            quantity.value() = 4;
            price.value() = 12.5;
            discount.value() = 0;
            status.text() = "Defaults staged; Enter confirms the discount draft";
          }),
          ui::button("Themes", [selector] { return selector.open(); }))).framed("rounded"),
      ui::themeSelector(selector, {
        {"nord", "Nord", nordTheme},
        {"latte", "Latte", catppuccinLatteTheme}}))
      | ui::onChange(quantity, [status](double value) {
          status.text() = text("Quantity accepted: {}", value);
        })
      | ui::onChange(price, [status](double value) {
          status.text() = text("Unit price accepted: {}", value);
        })
      | ui::onChange(discount, [status](double value) {
          status.text() = text("Discount accepted: {}%", value);
        })
      | ui::theme(nordTheme())
      | ui::onKey(key::F10, ui::quit);
  }
    | ui::runApp();
  // clang-format on
}
