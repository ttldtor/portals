// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../replay/terminal.hpp"
#include "form.hpp"
#include "gallery.hpp"

int main(int argc, char **argv) {
  try {
    const std::string_view option = argc == 2 ? argv[1] : "";

    if (option == "--check" || option == "initial" || option == "keyboard" || option == "mouse" ||
        option == "disabled") {
      gallery::Scene legacy;
      auto initial = legacy.render();
      legacy.stage(option == "--check" ? "disabled" : option);

      auto final = legacy.render();

      if (option == "--check" && (legacy.calls != 2 || legacy.changes != 1 || legacy.disabled_calls != 0)) {
        return 1;
      }

      example::ReplayTerminal terminal({{20, "New name\r\t \t\r", {}}, {100, "\x1bOQ", {}}, {150, "\x1b[21~", {}}},
                                       250);
      auto app = example::take(portals::Application::create({}, terminal.clock()));
      gallery::Form form(*app);

      return example::report(app->run(form, terminal));
    }

    if (!option.empty() || argc > 2) {
      std::cerr << "Usage: portals_gallery [--check]\n";

      return 2;
    }

    auto app = example::take(portals::Application::create());
    gallery::Form form(*app);

    return example::report(app->run(form));
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
  }
}
