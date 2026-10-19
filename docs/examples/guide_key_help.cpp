// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <cstdio>
#include <portals/key_hints.hpp>
#include <portals/ui.hpp>

/** @brief Declare each command once; both views read the same canonical registrations.
 * @return Existing native runner status after terminal restoration and cleanup. */
int main() try {
  using namespace portals;
  ui::Ref<LineEdit> document;
  ui::Ref<Label> status;
  ui::Ref<Button> disableSave, rebindSave;
  ui::Ref<HelpView> help;
  ui::KeyBindingRef save;

  auto scene = ui::scene(ui::vbox(ui::lineEdit(document, "Document"), ui::label(status, "Ready"),
                                  ui::button(disableSave, "Disable Save"), ui::button(rebindSave, "Use Ctrl+Shift+S"),
                                  ui::keyHints(help), ui::helpView(help)));

  // clang-format off
  return scene
    | ui::onKey("Ctrl+S", ui::keyInfo("Save").reference(save).owner(document),
                [status] { status.text() = "Save requested"; })
    | ui::onKey("F1", ui::keyInfo("Help"), [help] { return help.open(); })
    | ui::onKey("Escape", ui::keyInfo("Close help").scope(help), [help] { return help.close(); })
    | ui::onKey("F10", ui::keyInfo("Quit"), ui::quit)
    | ui::onClick(disableSave, [save] { return save.setEnabled(false); })
    | ui::onClick(rebindSave, [save] { return save.rebind(ui::press("Ctrl+Shift+S")); })
    | ui::runApp();
  // clang-format on
} catch (const std::bad_alloc &) {
  std::fputs("Unable to allocate the form.\n", stderr);
  return 1;
}
