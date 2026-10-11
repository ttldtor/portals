// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../examples/custom_widget/fluent_meter.hpp"
#include "../examples/custom_widget/meter.hpp"
#include "../examples/gallery/fluent_form.hpp"
#include "../examples/gallery/form.hpp"
#include "../examples/gallery/managed_report.hpp"
#include "../examples/replay/terminal.hpp"
#include "support/gallery_before.hpp"
#include "support/meter_before.hpp"
#include "support/vt_oracle.hpp"
#include <iostream>
#include <sstream>

namespace {
using namespace portals;
using example::take;

// These shared predicates must remain usable during constant evaluation.
static_assert(example::phaseName(ApplicationPhase::PREPARE) == "prepare");
static_assert(example::phaseName(static_cast<ApplicationPhase>(-1)) == "unknown");
constexpr InputEvent QUIT_PRESS = KeyEvent{.key = InputKey::F10, .action = KeyAction::PRESS};
constexpr InputEvent QUIT_RELEASE = KeyEvent{.key = InputKey::F10, .action = KeyAction::RELEASE};

static_assert(example::quitKey(QUIT_PRESS));
static_assert(!example::quitKey(QUIT_RELEASE));

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

template <class Scene> Capture runMeter(std::size_t theme) {
  example::ReplayTerminal terminal({{50, "\x1b[C\x1b[C", {}}, {100, "\x1bOQ", {}}, {200, "\x1b[21~", {}}}, 300,
                                   {80, 8});
  auto app = take(Application::create({}, terminal.clock()));
  Scene scene(*app, theme);
  const auto stats = take(scene.run(terminal));

  if (!terminal.closed) {
    throw std::runtime_error("Meter did not close its transport");
  }

  return {std::move(terminal.output), stats};
}

void meterThemes() {
  constexpr CellSize SIZE{80, 8};

  for (std::size_t theme = 0; theme < example::THEME_PRESETS.size(); ++theme) {
    const auto before = runMeter<meter_before::MeterScene>(theme);
    const auto after = runMeter<meter_example::MeterScene>(theme);

    if (before.bytes != after.bytes || before.stats.frames != after.stats.frames) {
      throw std::runtime_error("Scalar-state meter changed visible output or frame count");
    }

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

  std::vector<example::ReplayTerminal::Input> held;

  for (std::uint64_t press = 1; press <= 20; ++press) {
    held.push_back({press * 25, "\x1b[C", {}});
  }

  auto toBound = held;

  toBound.resize(10);

  const auto capture = [&](std::vector<example::ReplayTerminal::Input> input) {
    example::ReplayTerminal bounded(std::move(input), 600, SIZE);
    auto boundedApp = take(Application::create({}, bounded.clock()));
    meter_example::MeterScene boundedScene(*boundedApp);
    const auto stats = take(boundedScene.run(bounded));

    if (!bounded.closed) {
      throw std::runtime_error("Bounded meter did not close its transport");
    }

    return Capture{std::move(bounded.output), stats};
  };
  const auto reached = capture(std::move(toBound));
  const auto repeated = capture(std::move(held));

  // Ten presses reach 100; later clamped sets must not change output or redraw.
  if (reached.bytes != repeated.bytes || reached.stats.frames != repeated.stats.frames) {
    throw std::runtime_error("Clamped scalar state redraws unchanged presentation");
  }
}

Capture fluentForm(const std::vector<example::ReplayTerminal::Input> &script, CellSize size, std::size_t theme,
                   ColorMode colors, gallery::Composition composition) {
  example::ReplayTerminal terminal(script, 500, size);
  ui::SceneOptions options;

  options.clock = terminal.clock();
  options.application.writer.colorMode = colors;

  gallery::FluentForm form(options, theme, composition);
  const auto stats = take(form.run(terminal));

  if (!terminal.closed) {
    throw std::runtime_error("Fluent form did not close its transport");
  }

  return {std::move(terminal.output), stats, form.calls};
}

void owningFormEquivalence() {
  const std::array scripts{
    std::vector<example::ReplayTerminal::Input>{},
    std::vector<example::ReplayTerminal::Input>{{50,
                                                 "\x01"
                                                 "Ada\r\t \t\r",
                                                 {}},
                                                {150, "\x1bOQ", {}},
                                                {300, "\x1b[21~", {}}},
    std::vector<example::ReplayTerminal::Input>{{50, "\x1b[<0;2;5M\x1b[<0;2;5m", {}}, {150, "\x1b[21~", {}}},
    std::vector<example::ReplayTerminal::Input>{{50,
                                                 "\x01"
                                                 "\xe7\x95\x8c\r",
                                                 {}},
                                                {150, "", CellSize{12, 4}},
                                                {300, "\x1b[21~", {}}},
    // Modified F2 and F10 retain legacy admission through the typed onInput fallback.
    std::vector<example::ReplayTerminal::Input>{{50, "\x1b[1;2Q", {}}, {150, "\x1b[21;2~", {}}},
    std::vector<example::ReplayTerminal::Input>{{50, "\x03", {}}}};

  for (std::size_t theme = 0; theme < example::THEME_PRESETS.size(); ++theme) {
    for (auto colors : {ColorMode::TRUE_COLOR, ColorMode::ANSI16}) {
      for (const auto &script : scripts) {
        constexpr CellSize SIZE{80, 24};
        example::ReplayTerminal terminal(script, 500, SIZE);
        ApplicationOptions options;

        options.writer.colorMode = colors;

        auto app = take(Application::create(options, terminal.clock()));
        gallery::Form before(*app, theme);
        const auto stats = take(before.run(terminal));

        for (auto composition : {gallery::Composition::METHODS, gallery::Composition::PIPE}) {
          const auto after = fluentForm(script, SIZE, theme, colors, composition);

          if (!terminal.closed || terminal.output != after.bytes || stats.frames != after.stats.frames ||
              before.calls != after.calls) {
            throw std::runtime_error("Owning form changed focus, output, frames, actions or cleanup");
          }
        }
      }
    }
  }
}

void owningMeterEquivalence() {
  std::vector<example::ReplayTerminal::Input> held;

  for (std::uint64_t press = 1; press <= 20; ++press) {
    held.push_back({press * 25, "\x1b[C", {}});
  }

  const std::array scripts{
    std::vector<example::ReplayTerminal::Input>{},
    std::vector<example::ReplayTerminal::Input>{
      {50, "\x1b[C\x1b[C\x1b[D", {}}, {100, "", CellSize{50, 8}}, {150, "\x1bOQ", {}}, {200, "\x1b[21~", {}}},
    std::vector<example::ReplayTerminal::Input>{
      {50, "\x1b[1;2C", {}}, {100, "\x1b[1;2D", {}}, {150, "\x1b[1;2Q", {}}, {200, "\x1b[21;2~", {}}},
    std::vector<example::ReplayTerminal::Input>{{50, "\x03", {}}}, held};

  for (std::size_t theme = 0; theme < example::THEME_PRESETS.size(); ++theme) {
    for (auto colors : {ColorMode::TRUE_COLOR, ColorMode::ANSI16}) {
      for (const auto &script : scripts) {
        constexpr CellSize SIZE{80, 8};
        example::ReplayTerminal terminal(script, 600, SIZE);
        ApplicationOptions options;

        options.writer.colorMode = colors;

        auto app = take(Application::create(options, terminal.clock()));
        meter_example::MeterScene before(*app, theme);
        const auto stats = take(before.run(terminal));

        for (auto composition : {meter_example::Composition::METHODS, meter_example::Composition::PIPE}) {
          example::ReplayTerminal replay(script, 600, SIZE);
          ui::SceneOptions sceneOptions;

          sceneOptions.application = options;
          sceneOptions.clock = replay.clock();

          meter_example::FluentMeterScene after(sceneOptions, theme, composition);
          const auto fluentStats = take(after.run(replay));

          if (!terminal.closed || !replay.closed || terminal.output != replay.output ||
              stats.frames != fluentStats.frames) {
            throw std::runtime_error("Owning meter changed output, frames, bounds, theme or cleanup");
          }
        }
      }
    }
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
    owningFormEquivalence();
    owningMeterEquivalence();

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
