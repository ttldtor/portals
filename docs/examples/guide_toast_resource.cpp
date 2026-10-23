// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Keep notification commands in C++ while loading their body from a resource.
 * @return Standard runner status after diagnostics and terminal cleanup.
 */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::NotificationCenter notices;
    auto scene = ui::scene(ui::notifications(
      notices,
      ui::resource(R"(scene {
  VBox {
    LineEdit { "Draft" }
    Label#status { "Editing" }
    Button#saveRequest { "Request save" }
    Label { "F8 notification action; F9 dismiss; F10 exit" }
  }
})",
        ui::SceneResourceOptions{.format = ui::SceneResourceFormat::CONFIGURATION})));
    auto status = scene.get<Label>("status");
    auto save = scene.get<Button>("saveRequest");

    return std::move(scene)
      | ui::onClick(save, [notices, status] {
          status.text() = "Save requested";
          notices.show(ui::toast("Save requested").success()
            .action("Undo", [status] { status.text() = "Undo requested"; }));
        })
      | ui::onKey("F8", [notices] { notices.activateLatest(); })
      | ui::onKey("F9", [notices] { notices.dismissLatest(); })
      | ui::onKey("F10", ui::quit);
  } | ui::runApp();
  // clang-format on
}
