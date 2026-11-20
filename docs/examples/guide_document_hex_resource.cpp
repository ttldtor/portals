// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  // clang-format off
  auto scene = ui::scene(R"scene(
    scene {
      DocumentView#reader {
        file="README.md" mode="hex" fastScroll=8
        hex { bytesPerRow=16 groupSize=8 uppercase=true showAscii=true autoFit=true }
        frame { enabled=true pattern=ROUNDED title="Hex document" }
      }
    }
  )scene", {.format = ui::SceneResourceFormat::CONFIGURATION});

  return scene
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
