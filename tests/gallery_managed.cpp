// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../examples/custom_widget/meter.hpp"
#include "../examples/gallery/form.hpp"
#include "../examples/gallery/managed_report.hpp"
#include "../examples/replay/terminal.hpp"
#include "support/gallery_before.hpp"
#include "support/vt_oracle.hpp"
#include <iostream>
#include <sstream>

namespace {
using namespace portals;
using example::take;

struct Capture {
  std::string bytes;
  ApplicationStats stats;
  int calls{};
};

template <class Form> Capture runForm(std::vector<example::ReplayTerminal::Input> script, CellSize size) {
  example::ReplayTerminal terminal(std::move(script), 500, size);
  auto app = take(Application::create({}, terminal.clock()));
  Form form(*app);

  const auto stats = [&] {
    if constexpr (requires { form.run(terminal); }) {
      return take(form.run(terminal));
    } else {
      return take(app->run(form, terminal));
    }
  }();

  if (!terminal.closed) {
    throw std::runtime_error("Gallery did not close its transport");
  }

  return {std::move(terminal.output), stats, form.calls};
}

void compare(const std::vector<example::ReplayTerminal::Input> &script, CellSize size, int expectedCalls) {
  const auto before = runForm<gallery_before::Form>(script, size);
  const auto after = runForm<gallery::Form>(script, size);

  if (before.bytes != after.bytes || before.stats.frames != after.stats.frames || before.calls != expectedCalls ||
      after.calls != expectedCalls) {
    throw std::runtime_error("Gallery migration changed output, frames or actions");
  }
}

void meterThemes() {
  constexpr CellSize SIZE{80, 8};

  for (std::size_t theme = 0; theme < example::THEME_PRESETS.size(); ++theme) {
    example::ReplayTerminal terminal({{50, "\x1b[C\x1b[C", {}}, {100, "\x1bOQ", {}}, {200, "\x1b[21~", {}}}, 300, SIZE);
    auto app = take(Application::create({}, terminal.clock()));
    meter_example::MeterScene scene(*app, theme);
    const auto stats = take(scene.run(terminal));
    test_support::VtOracle screen(SIZE.width, SIZE.height);

    screen.feed(terminal.output);

    const auto snapshot = screen.snapshot();
    const auto sheet = example::THEME_PRESETS[(theme + 1) % example::THEME_PRESETS.size()].make();
    const auto colors = resolveStyle(sheet, {.type = "Meter"});

    // Two Right actions advance 50 -> 60%; the bar ends exactly at column 48.
    if (!terminal.closed || stats.frames < 3 ||
        snapshot.cells[3 * SIZE.width + 47].style.background != colors.focused.background ||
        snapshot.cells[3 * SIZE.width + 48].style.background != colors.normal.background) {
      throw std::runtime_error("Custom model, bar extent, theme or cleanup differs");
    }
  }

  example::ReplayTerminal terminal({}, 500, SIZE);
  auto app = take(Application::create({}, terminal.clock()));
  meter_example::MeterScene scene(*app);

  if (take(scene.run(terminal)).frames != 1) {
    throw std::runtime_error("Idle custom adapter redraws without a change");
  }
}

void diagnosticFailures() {
  struct CaptureErrors {
    std::ostringstream output;
    std::streambuf *previous{std::cerr.rdbuf(output.rdbuf())};

    ~CaptureErrors() {
      std::cerr.rdbuf(previous);
    }
  } capture;

  constexpr int PRIMARY_CODE = 101;
  constexpr int RUNNER_CODE = 202;
  constexpr int RESTORATION_CODE = 303;
  const ManagedError error{
    .phase = ManagedPhase::INPUT,
    .cause = ApplicationError{ApplicationPhase::INPUT, TerminalError{TerminalErrorKind::SYSTEM, PRIMARY_CODE}, {}},
    .application = ApplicationError{ApplicationPhase::CLOSE, TerminalError{TerminalErrorKind::SYSTEM, RUNNER_CODE},
                                    TerminalError{TerminalErrorKind::SYSTEM, RESTORATION_CODE}},
    .cleanup = TreeError::CALLBACK_FAILED};
  const int result = example::report(std::expected<ApplicationStats, ManagedError>(std::unexpected(error)));
  const auto output = capture.output.str();

  if (result != 1 || !output.contains("native=101") || !output.contains("native=202") ||
      !output.contains("restoration.native=303") || !output.contains("cleanup=")) {
    throw std::runtime_error("Managed diagnostic lost the primary, runner, restoration or cleanup failure");
  }
}
} // namespace

int main() {
  try {
    compare({{50,
              "\x01"
              "Ada\r\t \t\r",
              {}},
             {150, "\x1bOQ", {}},
             {300, "\x1b[21~", {}}},
            {80, 24}, 1);
    compare({{50, "\x1b[<0;2;5M\x1b[<0;2;5m", {}}, {150, "\x1b[21~", {}}}, {40, 8}, 1);
    compare({{50,
              "\x01"
              "\xe7\x95\x8c\r",
              {}},
             {150, "", CellSize{12, 4}},
             {300, "\x1b[21~", {}}},
            {40, 8}, 0);
    compare({}, {80, 24}, 0);
    meterThemes();
    diagnosticFailures();

    const auto idle = runForm<gallery::Form>({}, {80, 24});

    if (idle.stats.frames != 1) {
      throw std::runtime_error("Idle gallery redraws without a change");
    }

    std::cout << "Gallery: keyboard, mouse, Unicode, resize, theme, idle and cleanup match\n";

    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
  }
}
