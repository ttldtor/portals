// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

/** @brief Reuse commands in a menu, a button and explicit keyboard bindings. */
int main() {
  using namespace portals;

  ui::Ref<LineEdit> draft;
  ui::Ref<Label> saved;
  ui::ActionSet actions;

  // clang-format off
  auto save = actions.action("save", ui::Action{
    {.caption = "Save note", .shortcut = "Ctrl+S"},
    [draft, saved] { saved.text() = draft.text(); }});
  auto clear = actions.action("clear", ui::Action{
    {.caption = "Clear editor", .shortcut = "Ctrl+L"},
    [draft] { draft.text() = ""; }});
  auto exit = actions.action("exit", ui::Action{
    {.caption = "Quit", .shortcut = "F10"}, [] { ui::quit(); }});

  auto scene = ui::scene(ui::vbox(
    ui::label("One command, several ways to use it").id("heading"),
    ui::lineEdit(draft, "Remember the milk").width(56),
    ui::hbox(
      ui::menu({save, clear, exit}).width(28).framed("rounded", "Commands"),
      ui::vbox(
        ui::label("Saved note"),
        ui::label(saved, "Nothing saved yet").width(28),
        ui::button(save))),
    ui::label("Tab: focus   Up/Down: menu   Enter: select   F10: quit")
  ).width(64).framed("rounded", "Command desk"));

  return scene
    | ui::theme(nordTheme())
    | ui::style("#heading { bold: true; }")
    | ui::onKey("Ctrl+S", save)
    | ui::onKey("Ctrl+L", clear)
    | ui::onKey(key::F10, exit)
    | ui::runApp();
  // clang-format on
}
