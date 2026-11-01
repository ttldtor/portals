// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_style.hpp>

/** @brief Keep a form near the top and its actions at the bottom while resizing.
 * @return Standard runner status after diagnostics and terminal cleanup.
 */
int main() {
  using namespace portals;

  ui::Ref<LineEdit> name;
  ui::Ref<Label> saved;
  ui::Ref<Button> save, exit;

  // clang-format off
  auto scene = ui::scene(ui::vbox(
    ui::hbox(ui::label("Profile"), ui::spacer(), ui::label("F10: quit")).height(1),
    ui::spacing(1),
    ui::hbox(ui::label("Name").width(8), ui::lineEdit(name, "Ada").flexX()).height(1),
    ui::hbox(ui::label("Saved").width(8), ui::label(saved, "")).height(1),
    ui::spacer(),
    ui::hbox(
      ui::button(save, "Save").fitWidth(), ui::spacing(2),
      ui::button(exit, "Exit").fitWidth(), ui::spacer(2),
      ui::label("Resize the terminal"), ui::spacer()).height(1)));

  return scene
    | ui::theme(nordTheme())
    | ui::onClick(save, [name, saved] {
        saved.text() = name.text();
      })
    | ui::onClick(exit, ui::quit)
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
