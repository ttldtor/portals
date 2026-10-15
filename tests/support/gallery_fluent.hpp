// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "../../examples/gallery/fluent_form.hpp"

namespace gallery {
/** @brief Test-only stationary adapter; public form owns no test transport, counters or run wrappers. */
class FluentForm {
public:
  explicit FluentForm(portals::ui::SceneOptions policies = {}, std::size_t theme = 0)
      : options_{std::string(example::THEME_PRESETS.at(theme).name),
                 policies.application.writer.colorMode == portals::ColorMode::ANSI16},
        scene_(form(
          portals::ui::opts(options_),
          [this] {
            ++calls;
          },
          policies)) {
  }

  FluentForm(const FluentForm &) = delete;
  FluentForm &operator=(const FluentForm &) = delete;
  FluentForm(FluentForm &&) = delete;
  FluentForm &operator=(FluentForm &&) = delete;

  auto run(portals::ApplicationTerminal &terminal) {
    return scene_.run(terminal, {});
  }

  int runApp(portals::ApplicationTerminal &terminal) {
    return scene_ | portals::ui::runApp(terminal);
  }

  int runApp() {
    return scene_ | portals::ui::runApp();
  }

  int calls{};

private:
  example::Options options_;
  portals::ui::Scene scene_;
};
} // namespace gallery
