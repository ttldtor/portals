// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Keep editing while application-owned notifications offer an explicit Undo action.
 * @return Standard runner status after terminal cleanup and typed diagnostics.
 */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::NotificationCenter notices;
    ui::Ref<Label> status;
    ui::Ref<Button> save;
    return ui::scene(ui::notifications(
      notices, ui::column(
        ui::lineEdit("Draft"),
        ui::label(status, "Editing"),
        ui::button(save, "Request save"),
        ui::label("F8 notification action; F9 dismiss; F10 exit"))))
      | ui::onClick(save, [notices, status] {
          status.text() = "Save requested";
          notices.show(ui::toast("Save requested", {.severity = ToastSeverity::SUCCESS})
            .action("Undo", [status] { status.text() = "Undo requested"; }));
        })
      | ui::onKey("F8", [notices] { notices.activateLatest(); })
      | ui::onKey("F9", [notices] { notices.dismissLatest(); })
      | ui::onKey("F10", ui::quit);
  } | ui::runApp();
  // clang-format on
}
