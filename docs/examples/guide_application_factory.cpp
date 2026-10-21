// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Build the complete form inside the checked application boundary.
 * @return Standard runner status after terminal cleanup and error reporting.
 */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::Ref<LineEdit> message;
    ui::Ref<Button> add;

    return ui::scene(ui::vbox(
      ui::lineEdit(message, ""),
      ui::button(add, "Add Hello"),
      ui::button("Exit", [] { ui::quit(); })))
      | ui::onClick(add, [message] {
          message.text() += "Hello!";
        })
      | ui::onKey(key::F10, ui::quit);
  } | ui::runApp();
  // clang-format on
}
