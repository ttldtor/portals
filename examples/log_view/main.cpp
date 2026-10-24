// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <format>

int main() {
  using namespace portals;
  ui::Ref<LogView> log;
  auto scene = ui::scene(ui::vbox(
    ui::label("Recent events | wheel/PageUp reads history | F10 closes"),
    ui::logView(log, "Ready\n", {.limits = {.maxLines = 12}}).height(8),
    ui::hbox(
      ui::button("Append batch").onClick([log, batch = 0]() mutable {
        log.append(std::format("Batch {}\nПринят пакет\n界 cafe\u0301 👩‍💻 — a longer event record wraps as the terminal becomes narrower.\n", ++batch));
      }),
      ui::button("Return to tail").onClick([log] {
        log.tail();
      }),
      ui::button("Clear").onClick([log] {
        log.clear();
      }))));

  return scene
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
}
