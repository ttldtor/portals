// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

/** @brief Load a built-in FindBar and bind document and query buttons.
 * @return Standard runner status after diagnostics and terminal cleanup.
 */
int main() {
  using namespace portals;

  constexpr auto ENGLISH = "Portals keeps terminal interfaces small. Find Portals here.";
  constexpr auto RUSSIAN = "Портал открывает документ. Другой портал показывает результат.";

  auto scene = ui::scene(R"scene(
    scene {
      VBox {
        Label { "Literal search | F3 next | Shift+F3 previous | F10 closes" }
        FindBar#find { document="Portals keeps terminal interfaces small. Find Portals here." text="Portals" }
        HBox {
          Button#english { "English document" }
          Button#russian { "Russian document" }
        }
      }
    }
  )scene", {.format = ui::SceneResourceFormat::CONFIGURATION});

  auto find = scene.get<FindBar>("find");
  auto englishButton = scene.get<Button>("english");
  auto russianButton = scene.get<Button>("russian");

  return scene
    | ui::style(".find-bar-summary { bold: true; }")
    | ui::onClick(englishButton, [=] {
        find.document(ENGLISH);
        find.text() = "Portals";
      })
    | ui::onClick(russianButton, [=] {
        find.document(RUSSIAN);
        find.text() = "портал";
      })
    | ui::onKey(key::F3, [find] { find.next(); })
    | ui::onKey(key::SHIFT + key::F3, [find] { find.previous(); })
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
}
