// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Request one of three document actions through the ordinary application loop.
 * @return Existing runner status after diagnostics and terminal cleanup.
 */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::Ref<MessageDialog> unsaved;
    ui::Ref<Column> form;
    ui::Ref<Label> status;
    ui::Ref<Button> close;
    return ui::scene(
      ui::vbox(
        ui::label("Unsaved document"),
        ui::label("Editing").reference(status),
        ui::button("Close document").reference(close),
        ui::label("F10 exits from the main form; Escape cancels the dialog"))
        .reference(form),
      ui::unsavedChangesDialog(unsaved, "Unsaved changes", "Save this document before closing it?"))
      | ui::onClick(close, [unsaved, form, status] {
          unsaved.open(form, [status](const MessageResult &answer) {
            if (!answer) {
              return;
            }
            switch (*answer) {
            case MessageChoice::SAVE:
              status.text() = "Save requested";
              break;
            case MessageChoice::DISCARD:
              status.text() = "Discard requested";
              break;
            case MessageChoice::CANCEL:
              status.text() = "Keep editing";
              break;
            default:
              break;
            }
          });
        })
      | ui::onKey("F10", ui::quit);
  }
    | ui::runApp();
  // clang-format on
}
