// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  auto scene = ui::scene(R"scene(
    scene {
      DocumentView#reader {
        file="README.md" lineNumbers=true wrap=true
        frame { enabled=true pattern=ROUNDED title="Document" }
      }
    }
  )scene", {.format = ui::SceneResourceFormat::CONFIGURATION});

  return scene
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
}
