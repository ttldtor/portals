// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "editor.hpp"
#include <portals/ui_lyra.hpp>

/** @brief Parse launch options before preparing the three-mode editor. */
int main(int argc, char **argv) {
  using namespace portals;

  editor_history::Demo demo;

  return demo.scene()
    | ui::reportOnError()
    | ui::runApp(argc, argv, ui::lyra::cli(::lyra::cli{}));
}
