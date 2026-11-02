// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

/** @brief Show ordinary status sections and canonical key hints; F2 changes the status.
 * @return Standard runner status after typed error reporting and terminal cleanup.
 */
int main() {
  using namespace portals;

  ui::Ref<Label> status;
  ui::Ref<HelpView> help;

  // clang-format off
  auto scene = ui::scene(ui::vbox(
    ui::label("Status desk | F2 updates | F1 help | F10 quit"),
    ui::button("Mark saved").onClick([status] { status.text() = "Saved"; }),
    ui::helpView(help),
    ui::spacer(),
    ui::statusBar({.gap = 1},
      ui::label(status, "Ready"), ui::spacer(), ui::label("UTF-8"),
      ui::keyHints(help).maxWidth(36))));

  scene.onKey(ui::press("F2"), ui::keyInfo("Mark saved"), [status] { status.text() = "Saved"; });
  scene.onKey(ui::press("F1"), ui::keyInfo("Open help"), [help] { return help.open(); });
  scene.onKey(ui::press("Escape"), ui::keyInfo("Close help").scope(help), [help] { return help.close(); });
  scene.onKey(ui::press("F10"), ui::keyInfo("Quit"), [] { ui::quit(); });

  return scene
    | ui::theme(nordTheme())
    | ui::runApp();
  // clang-format on
}
