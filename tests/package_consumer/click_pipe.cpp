// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "tests/support/examples/replay/terminal.hpp"
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  example::ReplayTerminal terminal({{10, "\r", {}}, {20, "\x1b ", {}}, {30, "\x1bq", {}}}, 50, {40, 8});
  ui::SceneOptions options;
  ui::Ref<Label> message;
  ui::Ref<Button> button;
  int ordinary{}, modified{}, afterStop{}, shortcutCalls{};
  bool keyboardSource = true;

  options.clock = terminal.clock();

  auto scene = ui::scene(ui::vbox(ui::label(message, ""), ui::hbox(ui::button(button, "Hello"))), options) |
               ui::onClick(button,
                           [&] {
                             ++ordinary;
                             message.text() += "Hello!";
                           }) |
               ui::onClick(button,
                           [&](ui::ClickParams &params) {
                             keyboardSource = keyboardSource && params.source == ClickSource::KEYBOARD;

                             if (params.mods.has(key::ALT)) {
                               ++modified;
                               message.text() += "HELLO";
                               params.stop();
                             }
                           }) |
               ui::onClick(button, [&] {
                 ++afterStop;
               });

  scene | ui::onKey("F10 | Alt + Q | alt + q", [&](const KeyEvent &event) {
    ++shortcutCalls;

    if (event.key == InputKey::CHARACTER && event.modifiers.has(key::ALT)) {
      (void)scene.quit();
    }
  });

  const auto result = scene.run(terminal);

  return result && terminal.closed && ordinary == 2 && modified == 1 && afterStop == 1 && shortcutCalls == 1 &&
             keyboardSource
           ? 0
           : 1;
}
