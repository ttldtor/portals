// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Show custom dialog content with a safe choice and optional frame close.
 * @return Application status after diagnostics and terminal cleanup.
 */
int main() {
  using namespace portals;

  ui::Ref<MessageDialog> question;
  ui::Ref<Button> ask;
  ui::Ref<Label> status;

  // clang-format off
  auto scene = ui::scene(
    ui::vbox(
      ui::label(status, "No choice yet"),
      ui::button(ask, "Choose")),
    ui::dialog(question, "Continue?",
      ui::vbox(
        ui::label("Use these settings?"),
        ui::label("You can cancel without changing anything.")),
      ui::DialogButtons::YES_NO).closeButton());

  return scene
    | ui::onClick(ask, [question, status] {
        question.open([status](const MessageResult &answer) {
          if (!answer) {
            status.text() = "Dialog failed";
            return;
          }

          switch (*answer) {
          case MessageChoice::YES:
            status.text() = "Yes";
            break;
          case MessageChoice::NO:
            status.text() = "No";
            break;
          default:
            status.text() = "Cancelled";
            break;
          }
        });
      })
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
