// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../../../examples/dashboard/dashboard.hpp"
#include "../../support/examples/dashboard/dashboard.hpp"
#include "../../support/examples/replay/terminal.hpp"
#include "../../support/vt_oracle.hpp"
#include <array>
#include <iostream>
#include <string>

/** @brief Compare the public fluent scene with the former compact managed composition.
 * @return Zero after equal output/frame behavior and transport cleanup; one on any mismatch.
 */
int main() {
  using namespace portals;

  try {
    enum class LoadExpectation {
      NONE,
      SETTLED,
      CANCELLED_AT_EXIT
    };

    struct Scenario {
      std::string_view name;
      std::vector<example::ReplayTerminal::Input> input;
      std::uint64_t delayMs;
      std::uint32_t rate;
      LoadExpectation load;
    };

    constexpr std::uint64_t END_OF_INPUT_MS = 1000;
    constexpr std::uint64_t LOAD_DELAY_MS = 250;
    const std::vector<example::ReplayTerminal::Input> originalInput{
      {100, "\x1bOQ", {}}, {200, "\t\x01Replay note\r", {}}, {300, "\t\r", {}}, {600, "", CellSize{100, 30}}};
    const std::array scenarios{
      Scenario{"original-immediate", originalInput, 0, 1000, LoadExpectation::NONE},
      Scenario{"original-delayed", originalInput, LOAD_DELAY_MS, 1000, LoadExpectation::NONE},
      // End reaches the last instrument while the table still has its initial focus.
      Scenario{"tail-load-settles", {{50, "\x1b[F", {}}}, LOAD_DELAY_MS, 0, LoadExpectation::SETTLED},
      Scenario{"tail-load-cancelled", {{950, "\x1b[F", {}}}, LOAD_DELAY_MS, 0, LoadExpectation::CANCELLED_AT_EXIT}};

    for (const auto &scenario : scenarios) {
      example::ReplayTerminal before(scenario.input, END_OF_INPUT_MS);
      example::ReplayTerminal after(scenario.input, END_OF_INPUT_MS);
      dashboard_test::Options options;
      ApplicationOptions policies;

      options.stress = false;
      options.delayMs = scenario.delayMs;
      options.rate = scenario.rate;
      policies.writer.colorMode = ColorMode::ANSI16;

      auto app = example::take(Application::create(policies, before.clock()));
      dashboard_test::Scene baseline(*app, options);
      const auto expected = example::take(app->run(baseline, before));
      ui::SceneOptions runtime;

      runtime.application = policies;
      runtime.clock = after.clock();

      dashboard::FluentScene scene(options, runtime);
      const auto actual = scene.scene().run(after);

      if (!actual || !before.closed || !after.closed || before.output != after.output ||
          expected.frames != actual->frames || expected.bytes != actual->bytes) {
        std::cerr << "fluent dashboard changed compact output, frame count or terminal cleanup; scenario="
                  << scenario.name << '\n';

        return 1;
      }

      if (scenario.load == LoadExpectation::NONE) {
        continue;
      }

      const auto loading = baseline.loadingMetrics();
      test_support::VtOracle oracle(80, 24);

      oracle.feed(before.output);

      const auto screen = oracle.snapshot();
      std::string visible;

      for (const auto &cell : screen.cells) {
        if (!cell.tail) {
          visible += cell.glyph;
        }
      }

      // The initial ready page cannot satisfy End; require real page work rather than only equal output.
      const bool requested = loading.cacheMisses > 0 && loading.peakJobs > 0;
      const bool settled = scenario.load == LoadExpectation::SETTLED;
      const bool lifecycle = settled ? loading.usefulLoads > 0 && !baseline.pendingVisible()
                                     : loading.cancelledLoads > 0 && baseline.pendingVisible();
      const bool rendered = visible.find(settled ? "SYM-0999999" : "Loading") != std::string::npos;

      if (!requested || !lifecycle || !rendered || loading.activeJobs != 0 || loading.cachedPages != 0) {
        std::cerr << "tail-load scenario did not exercise its required request lifecycle; scenario=" << scenario.name
                  << " misses=" << loading.cacheMisses << " peak_jobs=" << loading.peakJobs
                  << " useful=" << loading.usefulLoads << " cancelled=" << loading.cancelledLoads
                  << " pending=" << baseline.pendingVisible() << " rendered=" << rendered << '\n';

        return 1;
      }

      std::cout << scenario.name << ": misses=" << loading.cacheMisses << " peak_jobs=" << loading.peakJobs
                << " useful=" << loading.usefulLoads << " cancelled=" << loading.cancelledLoads << '\n';
    }

    std::cout << "fluent dashboard: original replay, completed tail load and pending shutdown match\n";

    return 0;
  } catch (const std::exception &error) {
    std::cerr << "fluent dashboard comparison: " << error.what() << '\n';

    return 1;
  }
}
