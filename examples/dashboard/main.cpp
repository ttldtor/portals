// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "cli.hpp"

int main(int argc, char **argv) {
  using namespace portals;

  dashboard::Options data;
  dashboard::DisplayOptions display;
  auto lyraCli = dashboard::commandLine(data, display);
  dashboard::FluentScene dashboard(ui::opts(data));

  return dashboard.scene()
    | ui::applicationOptions([&] {
           data.replay = display.mode == "replay";
           ApplicationOptions options;

           options.framesPerSecond = display.framesPerSecond;
           options.writer.colorMode = display.ansi16 ? ColorMode::ANSI16 : ColorMode::TRUE_COLOR;

           return options;
         })
    | ui::reportOnError()
    | ui::runApp(argc, argv, ui::lyra::cli(lyraCli));
}
