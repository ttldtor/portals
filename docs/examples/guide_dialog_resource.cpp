// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Open and dismiss a resource-declared modal dialog.
 * @return Standard runner status after diagnostics and terminal cleanup.
 */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    auto scene = ui::scene(R"(scene {
  OverlayHost {
    VBox {
      Label { "Close button closes Details; F10 exits main form." }
      Button#detailsOpen { "Details" }
      Button#exit { "Exit" }
    }
    Dialog#details {
      title = "Connection settings"
      VBox {
        LineEdit { "Primary feed" }
        CheckBox { text = "Reconnect automatically"; state = CHECKED }
        Button#close { "Close" }
      }
    }
  }
})",
      ui::SceneResourceOptions{.format = ui::SceneResourceFormat::CONFIGURATION});
    auto details = scene.get<Dialog>("details");
    auto detailsOpen = scene.get<Button>("detailsOpen");
    auto exit = scene.get<Button>("exit");
    auto close = scene.get<Button>("close");

    return std::move(scene)
      | ui::onClick(detailsOpen, [details] { details.open(); })
      | ui::onClick(exit, ui::quit)
      | ui::onClick(close, [details] { details.dismiss(); })
      | ui::onKey("F10", ui::quit);
  } | ui::runApp();
  // clang-format on
}
