// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Show a shared command tree in an automatically hosted popup.
 * @return Application exit status after terminal cleanup.
 */
int main() {
  using namespace portals;

  ui::Ref<Button> opener;
  ui::Ref<Button> rightOpener;
  ui::Ref<Label> status;
  ui::Ref<PopupMenu> commands;
  ui::ActionSet actions;
  auto save = actions.action("save", "Save", [status] {
    status.text() = "Saved";
  });
  auto copy = actions.action("copy", "Save a copy…", [status] {
    status.text() = "Copy saved";
  });
  auto quit = actions.action("quit", "Quit", ui::quit);

  // clang-format off
  return ui::scene(
    ui::vbox(
      ui::label("Shared actions, nested panels and explicit keyboard mnemonics"),
      ui::hbox(
        ui::button(opener, "File").onClick([commands, opener] { commands.open(opener); }).fitWidth(),
        ui::spacer(),
        ui::button(rightOpener, "File at right").onClick([commands, rightOpener] {
          commands.open(rightOpener);
        }).fitWidth()),
      ui::button(save).fitWidth(),
      ui::label(status, "Open File at right, press M, then A. The submenu flips left; Escape returns.")),
    ui::popupMenu(commands, {
      ui::menuItem(save, U's'),
      ui::submenu("More", {ui::menuItem(copy, U'a')}, U'm'),
      ui::menuItem(quit, U'q')
    }).minWidth(18).maxWidth(56))
    | ui::onKey(key::F10, [commands, opener] { commands.open(opener); })
    | ui::onKey("Ctrl+S", save)
    | ui::runApp();
  // clang-format on
}
