// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

/** @brief Share document commands between buttons, shortcuts and nested popup panels.
 * @return Application exit status after terminal cleanup.
 */
int main() {
  using namespace portals;

  ui::Ref<Button> left, right, bottom;
  ui::Ref<Label> status;
  ui::Ref<PopupMenu> commands;
  ui::Ref<ThemeSelector> themes;
  ui::ActionSet actions;
  auto save = actions.action("save", "Save", [status] {
    status.text() = "Document saved";
  });
  auto copy = actions.action("copy", "Save a copy", [status] {
    status.text() = "Copy saved";
  });
  auto markdown = actions.action("markdown", "Markdown", [status] {
    status.text() = "Export preview: Markdown";
  });
  auto csv = actions.action("csv", "CSV", [status] {
    status.text() = "Export preview: CSV";
  });

  // clang-format off
  return ui::scene(
    ui::vbox(
      ui::label("Document desk | shared actions and nested popup menus"),
      ui::hbox(
        ui::button(left, "File", [=] { commands.open(left); }).fitWidth(),
        ui::spacer(),
        ui::button(right, "File at right", [=] { commands.open(right); }).fitWidth()),
      ui::vbox(
        ui::label("Quarterly report"),
        ui::label("Revenue     125,000"),
        ui::label("Expenses     82,000"),
        ui::label("Profit       43,000"),
        ui::label("Export is a preview; this example writes no files."))
        .framed("rounded", "Document").height(9),
      ui::spacer(),
      ui::hbox(
        ui::button(bottom, "File below", [=] { commands.open(bottom); }).fitWidth(),
        ui::button(save).fitWidth(),
        ui::spacer(),
        ui::button("Themes", [themes] { return themes.open(); }).fitWidth()),
      ui::label(status, "Open File, then Export and Format. Escape returns to the opener."),
      ui::label("F2: File | F3: right | F4: below | Ctrl+S: save | F10: quit")),
    ui::popupMenu(commands, {
      ui::menuItem(save, U's'),
      ui::submenu("Export", {
        ui::submenu("Format", {ui::menuItem(markdown, U'm'), ui::menuItem(csv, U'c')}, U'f'),
        ui::menuItem(copy, U'a')
      }, U'e')
    }),
    ui::themeSelector(themes, {
      {"nord", "Nord", nordTheme},
      {"latte", "Latte", catppuccinLatteTheme}}))
    | ui::theme(nordTheme())
    | ui::onKey(key::F2, [=] { commands.open(left); })
    | ui::onKey(key::F3, [=] { commands.open(right); })
    | ui::onKey(key::F4, [=] { commands.open(bottom); })
    | ui::onKey("Ctrl+S", save)
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
