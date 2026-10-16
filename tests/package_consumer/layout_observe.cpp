// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "tests/support/examples/replay/terminal.hpp"
#include <portals/ui.hpp>

int main() {
  using namespace portals;
  example::ReplayTerminal terminal({{50, "", CellSize{40, 8}}}, 100, {30, 6});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  ui::Ref<Label> label;
  int calls{};
  bool checked = true;
  auto scene =
    ui::scene(ui::label(label, "geometry"), options) | ui::onLayout(label, [&](const NodeGeometry &geometry) {
      ++calls;
      const auto current = label.geometry();
      const auto size = label.size();
      checked = checked && current && size && current->local == geometry.local && *size == geometry.local.size();
    });
  const auto result = scene.run(terminal);
  return result && terminal.closed && checked && calls == 2 && result->frames == 2 ? 0 : 1;
}
