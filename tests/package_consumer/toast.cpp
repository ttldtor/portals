// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#ifndef ERROR
#  define ERROR 12345
#  define PORTALS_TOAST_TEST_ERROR_DEFINED
#endif
#include <portals/toast.hpp>
#ifdef PORTALS_TOAST_TEST_ERROR_DEFINED
static_assert(ERROR == 12345);
#endif
#include <portals/ui.hpp>

#include "tests/support/examples/replay/terminal.hpp"

#include <iostream>
#include <stdexcept>

int main() try {
  using namespace portals;
  example::ReplayTerminal terminal(
    {{100, "\x1bOQ", {}}, {200, "\x1b[19~", {}}, {300, "\x1b", {}}, {400, "\x1b[20~", {}}, {500, "\x1b[21~", {}}}, 1000,
    {50, 12});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  ui::NotificationCenter notices;
  ui::Ref<Dialog> details;
  int actions{};
  bool dismissed{};
  auto scene = ui::scene(
    ui::overlayHost(ui::notifications(notices, ui::lineEdit("Keep editing")),
                    ui::dialog(details, "Details", ui::label("The application owns the full diagnostic history."))),
    options);
  scene.onKey("F2", [notices, details, &actions] {
    notices.show(ui::toast("Persistent diagnostic",
                           {.duration = std::nullopt, .dismissOnAction = false, .indicator = {.enabled = true}})
                   .action("Details", [details, &actions] {
                     ++actions;
                     details.open();
                   }));
  });
  scene.onKey("F8", [notices] {
    notices.activateLatest();
  });
  scene.onKey("F9", [notices, &dismissed] {
    notices.dismissLatest();
    const auto snapshot = notices.snapshot();
    dismissed = snapshot && (*snapshot)->items.empty();
  });
  scene.onKey("F10", ui::quit);
  if (scene.runApp(terminal) != 0 || actions != 1 || !dismissed || !terminal.closed || terminal.output.empty()) {
    throw std::runtime_error("installed notification runner/action/cleanup contract failed");
  }
  return 0;
} catch (const std::exception &failure) {
  std::cerr << failure.what() << '\n';
  return 1;
}
