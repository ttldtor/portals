// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_style.hpp>

/** @brief Load named editor variants while keeping application actions in C++.
 * @return Standard runner status after terminal cleanup and typed error reporting.
 */
int main() {
  using namespace portals;

  ui::SceneResourceOptions resource;
  resource.format = ui::SceneResourceFormat::CONFIGURATION;

  auto scene = ui::scene(R"(
    definitions {
      ShortEdit = LineEdit { history { maxOperations = 20 } }
    }
    defaults {
      LineEdit { history { maxOperations = 100 } }
    }
    scene {
      VBox {
        Label { text = "Independent editors | Ctrl+Z undo | F10 quit" }
        ShortEdit#first { text = "" }
        ShortEdit#second { text = "", history { maxOperations = 5 } }
        ThreeState { text = "Selection", state = MIXED }
        HBox {
          Button#add { text = "Append" }
          Button#exit { text = "Quit" }
        }
      }
    }
  )",
                         resource);
  auto first = scene.get<LineEdit>("first");

  return scene
    | ui::style("LineEdit { min-width: 30; } ShortEdit { bold: true; }")
    | ui::onClick(scene.get<Button>("add"), [first] {
           first.text() += "Hello!";
         })
    | ui::onClick(scene.get<Button>("exit"), ui::quit)
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
}
