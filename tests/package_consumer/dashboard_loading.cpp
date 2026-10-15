// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "tests/support/examples/dashboard/dashboard.hpp"
#include "tests/support/examples/replay/terminal.hpp"
#include <functional>

namespace {
class ObservedTerminal final : public example::ReplayTerminal {
public:
  using example::ReplayTerminal::ReplayTerminal;

  std::function<void()> beforeRead;

  portals::TerminalRead read(portals::TerminalDeadline deadline, std::stop_token stop) override {
    if (beforeRead) {
      beforeRead();
    }

    return example::ReplayTerminal::read(deadline, stop);
  }
};
} // namespace

int main() {
  using namespace portals;
  using example::take;

  constexpr std::uint64_t DELAY_MS = 250;
  constexpr std::uint64_t REPEAT_START_MS = 100;
  constexpr std::uint64_t REPEAT_END_MS = 800;
  constexpr std::uint64_t REPEAT_INTERVAL_MS = 10;
  std::vector<example::ReplayTerminal::Input> input;

  for (auto time = REPEAT_START_MS; time < REPEAT_END_MS; time += REPEAT_INTERVAL_MS) {
    input.push_back({time, "\x1b[B", {}});
  }

  ObservedTerminal terminal(std::move(input), REPEAT_END_MS + DELAY_MS + 100);
  auto app = take(Application::create({}, terminal.clock()));
  dashboard_test::Options options;

  options.rate = 0;
  options.delayMs = DELAY_MS;

  dashboard_test::Scene scene(*app, options);
  bool readyWhileHeld{};

  take(app->router().setFocus(scene.table()));
  take(setVirtualTableFirst(app->tree(), scene.table(), 10000));

  terminal.beforeRead = [&] {
    const auto clock = terminal.clock();
    const auto now = clock.read(clock.context);

    if (now < REPEAT_START_MS + DELAY_MS || now >= REPEAT_END_MS) {
      return;
    }

    take(app->tree().read(scene.table(), [&](const auto &table) {
      readyWhileHeld |= table.rangeState() == RangeState::READY;
    }));
  };

  const auto result = app->run(scene, terminal);
  const auto loading = scene.loadingMetrics();

  if (!result || !terminal.closed || !readyWhileHeld || !scene.coherent() ||
      loading.peakPages > options.loading.cachePages || loading.peakJobs > options.loading.backendJobs ||
      loading.peakBytes > options.loading.cacheBytes || loading.activeJobs || loading.cachedPages) {
    return 1;
  }

  return 0;
}
