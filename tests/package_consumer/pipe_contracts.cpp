// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "tests/support/examples/replay/terminal.hpp"
#include <portals/ui.hpp>

#include <iostream>

int main() {
  using namespace portals;

  example::ReplayTerminal terminal({{50, "\x1bOQ", {}}}, 100, {30, 4});
  ui::SceneOptions options;
  ui::Ref<LineEdit> field;
  bool undone{};

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::lineEdit(field, "initial"), options);
  auto configure = ui::configure([field](ManagedScene &) {
    return field.text().set("configured").result();
  });

  scene | configure | ui::onKey(ui::key(InputKey::F2), [field, &undone] {
    auto command = field.undo();
    const auto text = field.text().get();

    undone = command && text && *text == "initial";

    return command;
  });

  if (scene.managed() || !terminal.output.empty() || terminal.closed) {
    return 2;
  }

  const auto status = scene | ui::runApp(terminal);

  if (status != 0) {
    return status;
  }

  if (!terminal.closed || field.text().get() || !undone) {
    return 3;
  }

  std::cout << "closed=1; stale=1; undone=1\n" << terminal.output;

  return 0;
}
