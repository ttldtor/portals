// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "cli.hpp"

int main(int argc, char **argv) {
  using namespace portals;

  dashboard::Options data;
  dashboard::DisplayOptions display;
  auto lyraCli = dashboard::commandLine(data, display);

  // clang-format off
  return [&] {
    dashboard::FluentScene dashboard(ui::opts(data));

    auto scene = std::move(dashboard.scene());
    scene
      | ui::applicationOptions([&data, &display] {
          data.replay = display.mode == "replay";
          ApplicationOptions options;

          options.framesPerSecond = display.framesPerSecond;
          options.writer.colorMode = display.ansi16 ? ColorMode::ANSI16 : ColorMode::TRUE_COLOR;

          return options;
        });

    return scene;
  }
    | ui::runApp(argc, argv, ui::lyra::cli(lyraCli));
  // clang-format on
}
