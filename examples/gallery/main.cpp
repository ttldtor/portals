// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../replay/terminal.hpp"
#include "form.hpp"
#include "gallery.hpp"
#include "managed_report.hpp"

int main(int argc, char **argv) {
  try {
    const auto parsed = example::parseSceneOptions(argc, argv, true);

    if (!parsed || parsed->help) {
      if (!parsed) {
        std::cerr << parsed.error() << '\n';
      }

      example::printSceneUsage(std::cout, "portals_gallery");

      return parsed ? 0 : 2;
    }

    portals::ApplicationOptions options;

    options.writer.colorMode = parsed->ansi16 ? portals::ColorMode::ANSI16 : portals::ColorMode::TRUE_COLOR;

    if (parsed->check) {
      gallery::Scene legacy;
      auto initial = legacy.render();
      legacy.stage(parsed->stage.empty() ? "disabled" : parsed->stage);

      auto final = legacy.render();

      if (parsed->stage.empty() && (legacy.calls != 2 || legacy.changes != 1 || legacy.disabled_calls != 0)) {
        return 1;
      }

      example::ReplayTerminal terminal({{20, "New name\r\t \t\r", {}}, {100, "\x1bOQ", {}}, {150, "\x1b[21~", {}}},
                                       250);
      auto app = example::take(portals::Application::create(options, terminal.clock()));
      gallery::Form form(*app, parsed->theme);

      return example::report(form.run(terminal));
    }

    auto app = example::take(portals::Application::create(options));
    gallery::Form form(*app, parsed->theme);

    return example::report(form.run());
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
  }
}
