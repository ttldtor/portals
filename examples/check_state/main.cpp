// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Run a three-state checkbox; F10 exits with standard terminal cleanup.
 * @return Standard runner status after typed setup/runtime error reporting.
 */
int main() {
  using namespace portals;

  return ui::scene(ui::vbox(ui::label("CheckBox | Space changes state | F10 quit"),
                            ui::threeState("Selection", CheckState::MIXED)))
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
}
