// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_style.hpp>

/** @brief Let controls choose content or viewport sizing, then resize the form.
 * @return Standard runner status after diagnostics and terminal cleanup.
 */
int main() {
  using namespace portals;

  ui::Ref<LineEdit> name;
  ui::Ref<Label> saved;
  ui::Ref<Button> save, exit;

  // clang-format off
  auto scene = ui::scene(ui::vbox(
    ui::label("Profile"),
    ui::spacing(1),
    ui::hbox(ui::label("Name").width(8), ui::lineEdit(name, "Ada")),
    ui::checkbox("Alerts"),
    ui::gauge(0.65),
    ui::spacer(),
    ui::hbox(ui::button(save, "Save"), ui::spacing(2),
             ui::button(exit, "Exit"), ui::spacer()),
    ui::label(saved, "Nothing saved")));

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
