// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../../../examples/dashboard/dashboard.hpp"
#include "../../support/examples/dashboard/dashboard.hpp"
#include "../../support/examples/replay/terminal.hpp"
#include "../../support/vt_oracle.hpp"
#include <array>
#include <iostream>
#include <string>

namespace {
std::string visibleText(std::string_view bytes, portals::CellSize size) {
  test_support::VtOracle oracle(size.width, size.height);
  oracle.feed(bytes);

  std::string text;
  for (const auto &cell : oracle.snapshot().cells) {
    if (!cell.tail) {
      text += cell.glyph;
    }
  }
  return text;
}

void publicThemeSelector() {
  using namespace portals;
  const auto run = [](std::vector<example::ReplayTerminal::Input> input) {
    example::ReplayTerminal terminal(std::move(input), 1000);
    ui::SceneOptions runtime;
    runtime.clock = terminal.clock();
    dashboard::Options options;
    options.rate = 0;
    dashboard::FluentScene scene(options, runtime);
    const auto result = scene.scene().run(terminal);

    if (!result || !terminal.closed) {
      throw std::runtime_error("Public dashboard selector failed runtime/cleanup");
    }
    return visibleText(terminal.output, {80, 24});
  };
  const std::vector<example::ReplayTerminal::Input> apply{
    {100, "\x0b", {}}, {200, "\x14", {}}, {300, "\x1b[B", {}}, {350, "\x1b[B", {}}, {400, "\r", {}}};
  auto committed = apply;
  committed.push_back({900, "\x1b[21~", {}});
  const auto applied = run(std::move(committed));
  auto cancelled = apply;
  cancelled.insert(cancelled.end(), {{500, "\x0b", {}},
                                     {550, "\x14", {}},
                                     {600, "\x1b[B", {}},
                                     {650, "\x1b", {}},
                                     {800, "\x1bOQ", {}},
                                     {900, "\x1b[21~", {}}});
  const auto restored = run(std::move(cancelled));

  if (applied.find("Committed Turbo") == std::string::npos || restored.find("Committed Modern") == std::string::npos ||
      restored.find("Instrument") == std::string::npos || restored.find("Themes") == std::string::npos) {
    throw std::runtime_error(
      "Public dashboard selector apply/cancel/F2 did not preserve committed theme and market views");
  }
}
} // namespace

/** @brief Verify public market semantics alongside the unchanged legacy request-lifecycle oracle.
 * @return Zero after preserved data/input behavior, selector integration and transport cleanup; one on mismatch.
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
      options.stress = false;
      ApplicationOptions policies;

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

      if (!actual || !before.closed || !after.closed || actual->inputs != expected.inputs || actual->frames == 0) {
        std::cerr << "public dashboard changed input or transport cleanup; scenario=" << scenario.name << '\n';
        return 1;
      }
      CellSize viewport{80, 24};
      for (const auto &input : scenario.input) {
        if (input.resize) {
          viewport = *input.resize;
        }
      }
      const auto publicVisible = visibleText(after.output, viewport);
      for (const auto caption : {"Portals market", "Inspect", "Themes", "Committed", "Instrument"}) {
        if (publicVisible.find(caption) == std::string::npos) {
          throw std::runtime_error("Public dashboard lost a required control or market view");
        }
      }

      if (scenario.load == LoadExpectation::NONE) {
        if (publicVisible.find("Replay note") == std::string::npos ||
            publicVisible.find("Committed Turbo") == std::string::npos) {
          throw std::runtime_error("Public dashboard lost note editing or F2 theme behavior");
        }
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
      const bool rendered = visible.find(settled ? "SYM-0999999" : "Loading") != std::string::npos &&
                            publicVisible.find(settled ? "SYM-0999999" : "Loading") != std::string::npos;

      if (!requested || !lifecycle || !rendered || loading.activeJobs != 0 || loading.cachedPages != 0) {
        std::cerr << "tail-load scenario did not exercise its required request lifecycle; scenario=" << scenario.name
                  << " misses=" << loading.cacheMisses << " peak_jobs=" << loading.peakJobs
                  << " useful=" << loading.usefulLoads << " cancelled=" << loading.cancelledLoads
                  << " pending=" << baseline.pendingVisible() << " rendered=" << rendered << '\n'
                  << "public final screen: " << publicVisible << '\n';

        return 1;
      }

      std::cout << scenario.name << ": misses=" << loading.cacheMisses << " peak_jobs=" << loading.peakJobs
                << " useful=" << loading.usefulLoads << " cancelled=" << loading.cancelledLoads << '\n';
    }

    publicThemeSelector();
    std::cout << "fluent dashboard: market semantics, selector, completed tail load and pending shutdown verified\n";

    return 0;
  } catch (const std::exception &error) {
    std::cerr << "fluent dashboard comparison: " << error.what() << '\n';

    return 1;
  }
}
