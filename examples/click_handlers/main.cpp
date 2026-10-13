// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_style.hpp>

/** @brief Run a minimal lazy scene with a late click handler and a runtime stylesheet.
 * @return Zero on success, one on a typed setup/runtime failure after library cleanup/reporting.
 * Click Add Hello to append one undoable text change; F10 quits. This minimal program defines no CLI options.
 * The standard runner owns terminal restoration. Automated checks compile this example;
 * headless consumer tests exercise the same registration and event semantics separately.
 */
int main() {
  using namespace portals;

  ui::Ref<LineEdit> message;
  ui::Ref<Button> add;

  auto scene = ui::scene(ui::vbox(ui::lineEdit(message, ""), ui::button(add, "Add Hello")));

  return scene | ui::style("Button { bold: true; }") |
         ui::onClick(add,
                     [message] {
                       message.text() += "Hello!";
                     }) |
         ui::onKey(ui::key(InputKey::F10), ui::quit(scene.ref())) | ui::runApp();
}
