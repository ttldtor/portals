// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_style.hpp>

/** @brief Load a framed scrolling form and style its independent parts.
 * @return Standard runner status after diagnostics and terminal cleanup.
 */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    auto scene = ui::scene(R"scene(scene {
      VBox {
        Label { "Choose a section; use the wheel or drag the thumb." }
        ScrollArea {
          frame="rounded" frame.title="Sections" frame.titleDecoration=SQUARE
          placement=ON_FRAME scrollbar { pattern=FRAME } width=28 height=6
          VBox {
            Button { "Overview" } Button { "Accounts" }
            Button { "Orders" } Button { "Positions" }
            Button { "History" } Button { "Connections" }
            Button { "Preferences" } Button { "About" }
          }
        }
        Button#exit { "Exit" }
      }
    })scene", ui::SceneResourceOptions{.format = ui::SceneResourceFormat::CONFIGURATION});

    auto exit = scene.get<Button>("exit");

    return std::move(scene)
      | ui::style(R"pss(
          ScrollArea {
            frame-line-foreground: #7d8797;
            frame-title-foreground: #62b4d7;
            frame-ends-foreground: #62b4d7;
            frame-title-bold: true;
            scrollbar-track-foreground: #7d8797;
            scrollbar-thumb-foreground: #62b4d7;
            scrollbar-thumb-bold: true;
          }
        )pss")
      | ui::onClick(exit, ui::quit)
      | ui::onKey("F10", ui::quit);
  } | ui::runApp();
  // clang-format on
}
