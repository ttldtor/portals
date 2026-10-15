// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../../support/custom_widget_fluent.hpp"
#include <iostream>
#include "../../support/examples/gallery/options.hpp"
#include "../../support/examples/replay/terminal.hpp"

/** @brief Run the owning custom meter interactively or through finite replay.
 * @param argc Argument count including
 * the program name.
 * @param argv Borrowed option strings consumed during parsing.
 * @return Zero for success/help,
 * two for invalid options, one for setup or typed runtime failure.
 */
int main(int argc, char **argv) {
  try {
    const auto parsed = example::parseFixtureOptions(argc, argv);

    if (!parsed || parsed->help) {
      if (!parsed) {
        std::cerr << parsed.error() << '\n';
      }

      example::printSceneUsage(std::cout, "portals_custom_widget");

      return parsed ? 0 : 2;
    }

    portals::ApplicationOptions options;

    options.writer.colorMode = parsed->ansi16 ? portals::ColorMode::ANSI16 : portals::ColorMode::TRUE_COLOR;

    example::ReplayTerminal terminal(
      {{50, "\x1b[C\x1b[C", {}}, {100, "", portals::CellSize{50, 8}}, {150, "\x1bOQ", {}}, {200, "\x1b[21~", {}}}, 250);
    portals::ui::SceneOptions sceneOptions;

    sceneOptions.application = options;
    sceneOptions.clock = parsed->check ? terminal.clock() : portals::SchedulerClock{};

    meter_example::FluentMeterScene scene(sceneOptions, parsed->theme);

    return parsed->check ? scene.runApp(terminal) : scene.runApp();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
  }
}

