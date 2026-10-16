// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "tests/support/examples/replay/terminal.hpp"
#include <portals/ui_style.hpp>

int main() {
  using namespace portals;

  example::ReplayTerminal terminal({{50, "\t", {}}, {60, "\r", {}}, {80, "\x1b[21~", {}}}, 150, {40, 6});
  ui::SceneOptions options;
  bool edited{};
  int errors{}, reports{};

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::vbox(ui::lineEdit("").id("message"), ui::button("Add").id("add")), options);
  auto message = scene.get("message");
  ui::Ref<Button> add = scene.get("add");

  const int code = scene | ui::style(add, "* { bold: true; }") | ui::onError([&](const ManagedDiagnostic &) {
                     ++errors;
                   }) |
                   ui::reportOnError([&](const ui::SceneReport &) {
                     ++reports;
                   }) |
                   ui::onClick(add,
                               [message, &edited] {
                                 message.text() += "Hello!";

                                 auto value = message.text().get();

                                 edited = value && *value == "Hello!";
                               }) |
                   ui::onKey(key::F10, ui::quit) | ui::runApp(terminal);

  if (code || !edited || errors || reports || !terminal.closed || message.text().get() ||
      options.clock.read(options.clock.context) != 80) {
    return 1;
  }

  example::ReplayTerminal untouched;
  auto invalid = ui::scene(ui::button("not the requested ID"));
  auto missing = invalid.get<Button>("missing");
  bool reported{};

  invalid | ui::onClick(missing, ui::quit) | ui::reportOnError([&](const ui::SceneReport &report) {
    reported =
      report.failure && report.failure->operation && std::string_view(report.failure->operation) == "scene.get";
  });

  if (invalid.runApp(untouched) != 1 || !reported || untouched.closed || !untouched.output.empty()) {
    return 2;
  }

  return 0;
}
