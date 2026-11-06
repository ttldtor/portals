// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::Ref<LogView> log;
  auto application = ui::logFile("application.log");
  auto errors = ui::logFile("errors.log");

  // clang-format off
  return ui::scene(ui::vbox(
    ui::label("F2: application | F3: errors | F10: quit"),
    ui::label("PgUp: pause | End: follow | switching remembers each log"),
    ui::logView(log, application).framed("rounded", "Logs").height(12)))
    | ui::theme(nordTheme())
    | ui::onKey(key::F2, [=] { log.model(application); })
    | ui::onKey(key::F3, [=] { log.model(errors); })
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
