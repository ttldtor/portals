// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Run a password-display field with a presentation-only prompt and an undoable Clear action.
 * @return Zero on orderly exit, one after the runner restores the terminal and reports a typed failure.
 */
int main() {
  using namespace portals;

  ui::Ref<LineEdit> password;

  // clang-format off
  return ui::scene(ui::vbox(
           ui::lineEdit(password, "",
                        {.options = {.displayMode = LineEditDisplayMode::PASSWORD, .placeholder = "Password"}}),
           ui::button("Clear",
                      [password] {
                        return password.clear();
                      })))
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
  // clang-format on
}
