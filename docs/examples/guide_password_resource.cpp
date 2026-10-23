// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_password.hpp>

/** @brief Load a protected password form and bind its behavior from C++.
 * @return Standard runner status after typed diagnostics and terminal restoration.
 */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    auto scene = ui::scene(R"(scene {
  VBox {
    Password#password { text = "" placeHolder = "Password" }
    Button#inspect { "Inspect" }
    Label#status { "Waiting" }
  }
})",
      ui::SceneResourceOptions{.format = ui::SceneResourceFormat::CONFIGURATION});
    auto password = scene.get<Password>("password");
    auto inspect = scene.get<Button>("inspect");
    auto status = scene.get<Label>("status");

    return std::move(scene)
      | ui::onClick(inspect, [password, status] {
          return password.withPlaintext([status](SecretView value) {
            status.text() = value.empty() ? "Empty" : "Supplied";
          });
        })
      | ui::onKey("F10", ui::quit);
  } | ui::runApp();
  // clang-format on
}
