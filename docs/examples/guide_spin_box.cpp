// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::Ref<SpinBox> amount;
    ui::Ref<Label> status;

    return ui::scene(ui::vbox(LinearOptions{.gap = 1},
      ui::spinBox(amount, 25).range(0, 100).step(0.5),
      ui::label(status, "Type a number, or use Up/Down"),
      ui::button("Reset", [amount] {
        amount.value() = 25;
      })))
      | ui::onChange(amount, [status](double value) {
          status.text() = text("Accepted: {}", value);
        })
      | ui::onKey(key::F10, ui::quit);
  }
    | ui::runApp();
  // clang-format on
}
