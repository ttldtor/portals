// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "../../examples/custom_widget/fluent_meter.hpp"

namespace meter_example {
/** @brief Test-only adapter for typed statistics and custom replay transports. */
class FluentMeterScene {
public:
  /** @param policies Existing scene policies; borrowed clocks must outlive the run.
   * @param theme Initial theme preset index; invalid indices throw out_of_range.
   */
  explicit FluentMeterScene(ui::SceneOptions policies = {}, std::size_t theme = 0)
      : options_{std::string(example::THEME_PRESETS.at(theme).name),
                 policies.application.writer.colorMode == ColorMode::ANSI16},
        scene_(meter(ui::opts(options_), policies)) {
  }

  FluentMeterScene(const FluentMeterScene &) = delete;
  FluentMeterScene &operator=(const FluentMeterScene &) = delete;
  FluentMeterScene(FluentMeterScene &&) = delete;
  FluentMeterScene &operator=(FluentMeterScene &&) = delete;

  /** @return Statistics or typed setup/runtime/cleanup failure after native restoration. */
  auto run() {
    return scene_.run({});
  }

  /** @param terminal Borrowed transport opened and closed by the existing runner.
   * @return Statistics or original typed runtime and cleanup failures.
   */
  auto run(ApplicationTerminal &terminal) {
    return scene_.run(terminal, {});
  }

  /** @return Native final status, reported after terminal restoration. */
  int runApp() {
    return scene_ | ui::runApp();
  }

  /** @param terminal Borrowed custom transport living through this call.
   * @return Final status, reported after terminal restoration.
   */
  int runApp(ApplicationTerminal &terminal) {
    return scene_ | ui::runApp(terminal);
  }

private:
  example::Options options_;
  ui::Scene scene_;
};
} // namespace meter_example
