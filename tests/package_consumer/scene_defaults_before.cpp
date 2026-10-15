// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "tests/support/examples/replay/terminal.hpp"
#include <portals/ui.hpp>

#include <cstdio>
#include <exception>
#include <stop_token>

int main() try {
  using namespace portals;

  for (bool keyboard : {false, true}) {
    example::ReplayTerminal terminal({{10, "\t\t\r", {}}, {20, keyboard ? "\x1b[21~" : "\t\r", {}}}, 100, {40, 6});
    ui::SceneOptions options;
    ui::Ref<LineEdit> message;
    std::stop_source stop;
    bool quitCalled{}, failed{};

    options.clock = terminal.clock();

    auto scene = ui::scene(ui::column(ui::label("Message"), ui::lineEdit(message, "Hello"),
                                      ui::row(ui::button("Append",
                                                         [message] {
                                                           message.text() += text(" {}", 123);
                                                         }),
                                              ui::button("Quit",
                                                         [&stop, &quitCalled, message] {
                                                           const auto value = message.text().get();

                                                           quitCalled = value && *value == "Hello 123";
                                                           stop.request_stop();
                                                         }))),
                           options);

    scene.onKey(ui::key(InputKey::F10), [&stop, &quitCalled, message] {
      const auto value = message.text().get();

      quitCalled = value && *value == "Hello 123";
      stop.request_stop();
    });
    scene.onError([&failed](const ManagedDiagnostic &diagnostic) {
      failed = true;
      std::fprintf(stderr, "UI error at %s:%u\n", diagnostic.origin.file_name(), diagnostic.origin.line());
    });

    const auto result = scene.run(terminal, stop.get_token());

    if (!result) {
      std::fprintf(stderr, "Scene failed in phase %d\n", static_cast<int>(result.error().phase));
    }

    if (!result || failed) {
      return 1;
    }

    if (!quitCalled || !terminal.closed || terminal.output.find("Hello 123") == std::string::npos ||
        message.text().get()) {
      return 2;
    }

    if (std::fwrite(terminal.output.data(), 1, terminal.output.size(), stdout) != terminal.output.size()) {
      return 3;
    }
  }

  return 0;
} catch (const std::exception &exception) {
  std::fprintf(stderr, "%s\n", exception.what());

  return 1;
}
