// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "tests/support/examples/replay/terminal.hpp"
#include <portals/ui.hpp>

#include <cstdio>
#include <exception>

int main() {
  try {
    using namespace portals;

    for (bool keyboard : {false, true}) {
      example::ReplayTerminal terminal({{10, "\t\t\r", {}}, {20, keyboard ? "\x1b[21~" : "\t\r", {}}}, 100, {40, 6});
      ui::SceneOptions options;
      ui::Ref<LineEdit> message;
      bool quitCalled{};

      options.clock = terminal.clock();

      ui::Scene scene = ui::scene(ui::column(ui::label("Message"), ui::lineEdit(message, "Hello"),
                                             ui::row(ui::button("Append",
                                                                [message] {
                                                                  message.text() += text(" {}", 123);
                                                                }),
                                                     ui::button("Quit",
                                                                [&scene, &quitCalled, message] {
                                                                  const auto value = message.text().get();

                                                                  quitCalled = value && *value == "Hello 123";
                                                                  (void)scene.quit();
                                                                }))),
                                  options);

      scene.onKey(ui::key(InputKey::F10), [&scene, &quitCalled, message] {
        const auto value = message.text().get();

        quitCalled = value && *value == "Hello 123";
        (void)scene.quit();
      });

      const auto status = keyboard ? scene | ui::runApp(terminal) : ui::runApp(scene, terminal);

      if (status != 0 || !quitCalled || !terminal.closed || terminal.output.find("Hello 123") == std::string::npos ||
          message.text().get()) {
        return 2;
      }

      if (std::fwrite(terminal.output.data(), 1, terminal.output.size(), stdout) != terminal.output.size()) {
        return 3;
      }
    }

    // Compile and execute both independent channels against the installed SDK.
    example::ReplayTerminal terminal;
    ui::SceneOptions options;
    int reacted{}, reported{};

    options.maxConfigurations = 0;

    auto scene = ui::scene(ui::label("invalid setup"), options) | ui::reportOnError([&](const ui::SceneReport &report) {
                   if (report.failure && report.failure->phase == ManagedPhase::PREPARE) {
                     ++reported;
                   }
                 }) |
                 ui::onError([&](const ManagedDiagnostic &) {
                   ++reacted;
                 });

    if (ui::runApp(scene, terminal) != 1 || reacted != 1 || reported != 1 || terminal.closed) {
      return 4;
    }

    return 0;
  } catch (const std::exception &error) {
    std::fprintf(stderr, "scene construction: %s\n", error.what());

    return 1;
  }
}
