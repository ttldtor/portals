// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Load the built-in bounded LogView and bind append and recovery buttons.
 * @return Standard runner status after diagnostics and terminal cleanup.
 */
int main() {
  using namespace portals;

  constexpr auto NEXT_BATCH = "A complete batch\nkeeps its own line boundaries";

  auto scene = ui::scene(R"scene(
    scene {
      VBox {
        Label { "Bounded in-memory log | scroll up to pause | F10 closes" }
        LogView#log {
          text="Ready for input" maxLines=128 maxBytes=65536
          maxBatchBytes=8192 maxRecordBytes=4096
        }
        HBox {
          Button#append { "Append batch" }
          Button#clear { "Clear" }
          Button#tail { "Return to tail" }
        }
      }
    }
  )scene", {.format = ui::SceneResourceFormat::CONFIGURATION});

  auto log = scene.get<LogView>("log");
  auto append = scene.get<Button>("append");
  auto clear = scene.get<Button>("clear");
  auto tail = scene.get<Button>("tail");

  return scene
    | ui::onClick(append, [=] {
        log.append(NEXT_BATCH);
      })
    | ui::onClick(clear, [=] {
        log.clear();
      })
    | ui::onClick(tail, [=] {
        log.tail();
      })
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
}
