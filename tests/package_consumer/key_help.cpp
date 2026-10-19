// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/key_hints.hpp>
#include <portals/ui.hpp>

int main() try {
  using namespace portals;
  auto application = Application::create();
  if (!application) {
    return 1;
  }
  auto owner = ManagedScene::create(**application);
  if (!owner) {
    return 2;
  }
  auto &scene = **owner;
  ui::Ref<Button> document;
  ui::Ref<HelpView> help;
  auto mounted = ui::mount(scene, ui::column(ui::button(document, "Document",
                                                        [] {
                                                        }),
                                             ui::keyHints(help), ui::helpView(help)));
  if (!mounted) {
    return 3;
  }
  KeyInfo save(i18n("command.save"));
  save.owner = document.node();
  save.description = i18n("command.save.description");
  save.resolver = [](std::string_view key) -> std::expected<std::string, TextResourceError> {
    return std::string(key);
  };
  int calls{};
  auto binding = scene.onKey(ui::press("Ctrl+S"), std::move(save), [&] {
    ++calls;
  });
  auto opening = scene.onKey(ui::press("F1"), KeyInfo(TextValue("Help")), [help] {
    return help.open();
  });
  if (!binding || !opening || !scene.focus(document.node())) {
    return 4;
  }
  auto snapshot = scene.keyBindings();
  if (!snapshot || snapshot->bindings.size() != 2) {
    return 5;
  }
  auto entered = scene.input(**application, KeyEvent{InputKey::F1, 0, {}, KeyAction::PRESS});
  if (!entered || !*entered || !help.close()) {
    return 6;
  }
  if (!binding->setEnabled(false) || !binding->rebind(ui::pressRepeat("Ctrl+Shift+S")) || !binding->unbind()) {
    return 7;
  }
  auto updated = scene.keyBindings();
  if (!updated || updated->revision == snapshot->revision || calls) {
    return 8;
  }
  if (!binding->remove() || !scene.close() || snapshot->bindings.front().label.bytes() != "command.save") {
    return 9;
  }
  return 0;
} catch (const std::exception &) {
  return 10;
}
