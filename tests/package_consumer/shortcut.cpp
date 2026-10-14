// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "examples/replay/terminal.hpp"
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  example::ReplayTerminal terminal({{10, "w", {}}, {20, "\x1bq", {}}}, 50, {40, 8});
  ui::SceneOptions options;
  int presses{};
  int nativeOnly{};
  int exits{};

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::label("ready"), options) |
               ui::onKey(ui::pressRepeat(std::string("W")),
                         [&] {
                           ++presses;
                         }) |
               ui::onKey(ui::repeat({key::UP, key::DOWN, key::LEFT, key::RIGHT}),
                         [&] {
                           ++nativeOnly;
                         }) |
               ui::onKey(ui::release({key::F10, key::ALT + key::Q}), [&] {
                 ++nativeOnly;
               });

  scene.onKey(ui::keyActions({key::F10, key::ALT + key::Q}, {KeyAction::PRESS, KeyAction::RELEASE}), [&] {
    ++exits;
  });

  const auto result = scene.run(terminal);

  return result && terminal.closed && presses == 1 && nativeOnly == 0 && exits == 1 ? 0 : 1;
}
