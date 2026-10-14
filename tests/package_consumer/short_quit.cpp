// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "examples/replay/terminal.hpp"
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  for (const bool keyboard : {false, true}) {
    example::ReplayTerminal terminal(
      keyboard
        ? std::vector<example::ReplayTerminal::Input>{{50, "\t", {}}, {60, "\r", {}}, {80, "\x1b[21~", {}}}
        : std::vector<example::ReplayTerminal::Input>{{50, "\t", {}}, {60, "\r", {}}, {70, "\t", {}}, {80, "\r", {}}},
      150, {40, 6});
    ui::SceneOptions options;
    ui::Ref<LineEdit> message;
    ui::Ref<Button> add;
    ui::Ref<Button> exit;
    int notifications{}, reports{};
    bool edited{};

    options.clock = terminal.clock();

    auto scene =
      ui::scene(ui::vbox(ui::lineEdit(message, ""), ui::button(add, "Add Hello"), ui::button(exit, "Exit")), options);

    const auto code = scene | ui::onError([&](const ManagedDiagnostic &) {
                        ++notifications;
                      }) |
                      ui::reportOnError([&](const ui::SceneReport &) {
                        ++reports;
                      }) |
                      ui::onClick(add,
                                  [message, &edited] {
                                    message.text() += "Hello!";

                                    const auto value = message.text().get();

                                    edited = value && *value == "Hello!";
                                  }) |
                      ui::onClick(exit,
                                  [] {
                                    ui::quit();
                                  }) |
                      ui::onKey(key::F10, ui::quit) | ui::runApp(terminal);

    if (code || !edited || notifications || reports || !terminal.closed ||
        options.clock.read(options.clock.context) != 80 || message.text().get()) {
      return 1;
    }
  }

  return 0;
}
