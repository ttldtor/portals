// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  auto log = ui::logFile("application.log");

  // clang-format off
  return ui::scene(ui::logView(log).framed("rounded", "Application log"))
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
