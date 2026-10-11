// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "examples/replay/terminal.hpp"
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  example::ReplayTerminal terminal({}, 100, {32, 4});
  ui::SceneOptions options;
  ui::Ref<LineEdit> field;
  ui::Ref<Button> button;
  bool configured{};

  options.clock = terminal.clock();

  auto form = ui::scene(ui::column(ui::lineEdit(field, "initial"), ui::button(button, "Apply")), options);

  form.prepare().configure([&](ManagedScene &) {
    const auto initial = field.text().get();

    configured = initial && *initial == "initial";

    return button.onClick([field] {
      field.text() += "!";
    });
  });

  if (!configured || !form.checked() || !terminal.output.empty()) {
    return 1;
  }

  auto result = std::move(form) |
                ui::onKey(ui::key(InputKey::F2),
                          [field] {
                            return field.undo();
                          }) |
                ui::run(terminal);

  return result && terminal.closed && !field.text().get() ? 0 : 2;
}
