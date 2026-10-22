// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_style.hpp>

/** @brief Compose a framed form, a determinate gauge and ordinary actions.
 * @return Standard runner status after terminal cleanup and typed error reporting.
 */
int main() {
  using namespace portals;

  ui::Ref<LineEdit> message;
  ui::Ref<Button> add;

  // clang-format off
  auto scene = ui::scene(ui::vbox(
    ui::label("Portals | F10 quit"),
    ui::lineEdit(message, ""),
    ui::gauge(0.65).width(24),
    ui::hbox(ui::button(add, "Add Hello"), ui::plainButton("Quit", [] { ui::quit(); }))).framed());

  return scene
    | ui::style(add, "*:focused { bold: true; }")
    | ui::onClick(add, [message] {
           message.text() += "Hello!";
         })
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
