// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "scene.hpp"
#include <portals/ui_lyra.hpp>

/** @brief Run two synthetic Unicode file panels; --help never opens a terminal.
 * @param argc Process argument count, including the executable name.
 * @param argv Argument storage borrowed through synchronous CLI preflight and the runner.
 * @return Zero for success/help, two for invalid CLI, one for setup/runtime failure after terminal cleanup.
 */
int main(int argc, char **argv) {
  using namespace portals;
  int emojiWidth = 2;
  bool showHelp{};
  auto parser = lyra::cli()
    | lyra::opt(emojiWidth, "cells")["--emoji-width"]("Emoji cell width (default: 2)").choices(1, 2)
    | lyra::help(showHelp).description(
      "Compare synthetic Unicode file panels; no files are read or changed. "
      "Tab/Shift+Tab move focus, arrows select, F9 opens the panel, Escape closes it, F10 exits. "
      "The interactive run temporarily changes terminal modes and restores them on orderly exit. "
      "Glyph shaping and width depend on the terminal and font. "
      "Exit status: 0 for success/help, 2 for invalid CLI, 1 for setup or runtime failure.");

  return [&] {
    ui::SceneOptions options;
    options.application.widthPolicy.emoji = emojiWidth == 1 ? EmojiWidth::ONE : EmojiWidth::TWO;

    return unicode_panels::form(options);
  }
  | ui::runApp(argc, argv, ui::lyra::cli(parser, {.automaticHelp = false, .helpRequested = &showHelp}));
}
