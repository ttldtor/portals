// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_password.hpp>

/** @brief Inspect a protected password without copying its bytes.
 * @return Standard runner status after typed diagnostics and terminal restoration.
 */
int main() {
  using namespace portals;

  ui::PasswordRef password;
  ui::Ref<Button> inspect;
  ui::Ref<Label> status;

  // clang-format off
  return ui::scene(ui::vbox(
    ui::password(password).placeholder("Password"),
    ui::button(inspect, "Inspect"),
    ui::label(status, "Waiting")))
    | ui::onClick(inspect, [password, status] {
        return password.withPlaintext([status](SecretView value) {
          status.text() = value.empty() ? "Empty" : "Supplied";
        });
      })
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
  // clang-format on
}
