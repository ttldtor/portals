// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_style.hpp>

/** @brief Load a managed indicator and bind Start, Stop and Exit buttons.
 * @return Standard runner status after typed diagnostics and terminal restoration.
 */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    auto scene = ui::scene(R"(scene {
  VBox {
    Spinner#loading { preset = BRAILLE label = "Loading quotes" interval = 100 width = 30 }
    HBox {
      Button#start { "Start" }
      Button#stop { "Stop" }
    }
    Button#exit { "Exit" }
  }
})",
      ui::SceneResourceOptions{.format = ui::SceneResourceFormat::CONFIGURATION});
    auto loading = scene.get<Spinner>("loading");
    auto start = scene.get<Button>("start");
    auto stop = scene.get<Button>("stop");
    auto exit = scene.get<Button>("exit");

    return std::move(scene)
      | ui::style("Spinner { foreground: #62b4d7; }")
      | ui::onClick(start, [loading] { loading.start(); })
      | ui::onClick(stop, [loading] { loading.stop(); })
      | ui::onClick(exit, ui::quit)
      | ui::onKey("F10 | Ctrl+C", ui::quit);
  } | ui::runApp();
  // clang-format on
}
