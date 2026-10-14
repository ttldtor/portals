// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "examples/replay/terminal.hpp"
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  example::ReplayTerminal terminal({{10, "\x0b", {}}, {20, "\x0b", {}}, {30, "\x0b", {}}, {40, "k", {}}}, 100, {40, 8});
  ui::SceneOptions options;
  int compact{}, independent{}, released{};

  options.clock = terminal.clock();
  options.managed.keySequenceTimeout = 25;

  auto scene = ui::scene(ui::label("ready"), options) |
               ui::onKey("Ctrl+K+K",
                         [&] {
                           ++compact;
                         }) |
               ui::onKey("Ctrl+K,K", [&] {
                 ++independent;
               });
  const auto result = scene.run(terminal);

  if (!result || !terminal.closed || compact != 1 || independent != 1) {
    return 1;
  }

  auto app = Application::create({}, terminal.clock());

  if (!app) {
    return 2;
  }

  auto owner = ManagedScene::create(**app);

  if (!owner) {
    return 3;
  }

  const auto registered = (*owner)->onKey(ui::release("A,B"), [&] {
    ++released;
  });

  if (!registered) {
    return 4;
  }

  const auto first = (*owner)->input(**app, KeyEvent{InputKey::CHARACTER, U'A', {}, KeyAction::RELEASE});
  const auto ignored = (*owner)->input(**app, KeyEvent{InputKey::CHARACTER, U'B'});
  const auto second = (*owner)->input(**app, KeyEvent{InputKey::CHARACTER, U'B', {}, KeyAction::RELEASE});
  const auto closed = (*owner)->close();

  return first && *first && ignored && !*ignored && second && *second && released == 1 && closed ? 0 : 5;
}
