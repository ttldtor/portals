// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_style.hpp>

/** @brief Share initial editor settings without sharing live text or undo history.
 * @return Standard runner status after terminal cleanup and typed error reporting.
 */
int main() {
  using namespace portals;

  ui::ControlContext controls;
  controls.defaults("LineEdit", {.history = {.maxOperations = 100}})
    .define("ShortEdit", "LineEdit", {.history = {.maxOperations = 20}});

  ui::Ref<LineEdit> first;
  auto scene = ui::scene(
    ui::vbox(ui::label("Independent editors | Ctrl+Z undo | F10 quit"),
             ui::lineEdit(first, "").variant("ShortEdit"),
             ui::lineEdit("").variant("ShortEdit").settings({.history = {.maxOperations = 5}}),
             ui::threeState("Selection", CheckState::MIXED),
             ui::hbox(ui::button("Append").onClick([first] {
               first.text() += "Hello!";
             }),
                      ui::button("Quit").onClick(ui::quit))),
    {.controls = controls});

  return scene
    | ui::style("LineEdit { min-width: 30; } ShortEdit { bold: true; }")
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
}
