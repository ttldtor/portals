// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/examples/dashboard/dashboard.hpp"
#include "support/dashboard_before.hpp"
#include "support/examples/replay/terminal.hpp"
#include "support/vt_oracle.hpp"
#include <functional>
#include <iostream>
#include <source_location>

namespace {
using namespace portals;
using example::take;

void check(bool condition, std::source_location where = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error("dashboard check line " + std::to_string(where.line()));
  }
}

void losslessAndSaturated(bool replay) {
  example::ReplayTerminal terminal({}, 1000);
  ApplicationOptions application;
  application.scheduler.data_capacity = application.scheduler.data_budget = 1;
  application.scheduler.control_capacity = application.scheduler.control_budget = 1;
  auto app = take(Application::create(application, terminal.clock()));
  dashboard_test::Options options;
  options.rate = 10000;
  options.replay = replay;
  std::size_t ingressSamples{};
  options.ingressObserver = [](void *context, SchedulerTime) {
    ++*static_cast<std::size_t *>(context);
  };
  options.ingressContext = &ingressSamples;

  dashboard_test::Scene scene(*app, options);

  const auto result = take(app->run(scene, terminal));
  check(terminal.closed && scene.tickCount() == 10000 && scene.scheduledTicks() == 10000);
  check(ingressSamples > 0 && scene.stoppedAt().has_value() && scene.pendingTicks() == 0);
  check(scene.coherent() && result.frames <= 31 && scene.displayedRevision() > 1);
  check(scene.maxPreparedRows() <= 10 + 4);
  double first{}, last{}, high = -1e9, low = 1e9;

  for (std::uint64_t i = 0; i != 10000; ++i) {
    const auto price = replay ? example::REPLAY_PRICES[i % example::REPLAY_PRICES.size()]
                              : dashboard_test::syntheticPrice(i, options.seed);

    if (i == 0) {
      first = price;
    }

    last = price;
    high = std::max(high, price);
    low = std::min(low, price);
  }

  const auto aggregate = scene.aggregate();
  check(aggregate.open == first && aggregate.close == last && aggregate.high == high && aggregate.low == low);

  const auto metrics = app->scheduler().metrics();
  check(metrics.data.high_water == 1 && metrics.data.rejected > 0);
  test_support::VtOracle oracle(80, 24);
  oracle.feed(terminal.output);
  check(oracle.snapshot().cells[0].glyph == "P");
}

void pendingLatestOwnsRejectedValue() {
  auto scheduler = take(Scheduler::create({.data_capacity = 1, .data_budget = 1}));
  auto scope = take(scheduler->makeScope());
  auto occupied = take(scheduler->connect<int>(scope, [](auto) {
  }));
  std::uint64_t received{};
  auto destination = take(scheduler->connect<ModelSnapshot<dashboard_test::Quote>>(scope, [&](auto snapshot) {
    received = snapshot->revision();
  }));
  take(occupied.publisher.publish(std::make_shared<const int>(1)));

  std::shared_ptr<const ModelSnapshot<dashboard_test::Quote>> pending =
    std::make_shared<const dashboard_test::Snapshot>(9, 1, Ohlc{2, 5, 1, 3}, std::array<RowRange, 1>{{{0, 1}}});
  std::weak_ptr<const ModelSnapshot<dashboard_test::Quote>> observer = pending;
  auto rejected = dashboard_test::forwardLatest(destination.publisher, pending);
  check(!rejected && rejected.error() == SchedulerError::WOULD_BLOCK && pending && !observer.expired());
  take(scheduler->dispatch());
  take(dashboard_test::forwardLatest(destination.publisher, pending));
  check(!pending && !observer.expired());
  take(scheduler->dispatch());
  check(received == 9 && observer.expired());
}

class ObservedTerminal final : public example::ReplayTerminal {
public:
  using example::ReplayTerminal::ReplayTerminal;
  std::function<void()> beforeRead;

  TerminalRead read(TerminalDeadline deadline, std::stop_token stop) override {
    if (beforeRead) {
      beforeRead();
    }

    return example::ReplayTerminal::read(deadline, stop);
  }
};

void delayedRangesAndKeyboard() {
  ObservedTerminal terminal({{100, "\x1b[B", {}},
                             {150, "\x1bOQ", {}},
                             {500,
                              "\t\x01"
                              "Edited note\r",
                              {}}},
                            900);
  ApplicationOptions application;
  dashboard_test::Scene *observed{};
  bool sawPending{}, sawReady{};
  std::uint64_t firstReady{};

  struct Context {
    dashboard_test::Scene **scene;
    example::ReplayTerminal *terminal;
    bool *pending;
    bool *ready;
    std::uint64_t *time;
  };

  Context context{&observed, &terminal, &sawPending, &sawReady, &firstReady};
  application.observer = {[](void *data, ApplicationPhase phase, std::chrono::nanoseconds) {
                            auto &context = *static_cast<Context *>(data);

                            if (phase != ApplicationPhase::PREPARE) {
                              return;
                            }

                            if ((*context.scene)->pendingVisible()) {
                              *context.pending = true;
                            } else if (!*context.ready) {
                              *context.ready = true;
                              const auto clock = context.terminal->clock();
                              *context.time = clock.read(clock.context);
                            }
                          },
                          &context};
  auto app = take(Application::create(application, terminal.clock()));
  dashboard_test::Options options;
  options.rate = 0;
  options.delayMs = 250;
  dashboard_test::Scene scene(*app, options);
  observed = &scene;
  take(setVirtualTableFirst(app->tree(), scene.table(), 10000));
  bool finalReady{};

  terminal.beforeRead = [&] {
    take(app->tree().read(scene.table(), [&](const auto &table) {
      finalReady =
        table.rangeState() == RangeState::READY && table.firstRow() >= 10000 && table.selection().has_value();
    }));
  };

  take(app->run(scene, terminal));
  check(sawPending && sawReady && firstReady >= 260);
  check(scene.coherent() && scene.turbo());
  check(take(scene.note().snapshot())->value == "Edited note");
  check(finalReady);
  check(!app->tree().inspect(scene.table()));
}

void continuousNavigationKeepsRowsVisible(bool backwards, std::uint64_t delayMs = 0, bool presentDataState = false) {
  constexpr std::uint64_t FORWARD_FIRST = 240;
  constexpr std::uint64_t BACKWARD_FIRST = 300;
  const auto initialFirst = backwards ? BACKWARD_FIRST : FORWARD_FIRST;
  constexpr std::uint64_t INITIAL_PAGE_ROWS = 256;
  constexpr std::uint64_t REPEAT_START = 100;
  constexpr std::uint64_t REPEAT_INTERVAL = 10;
  constexpr std::uint64_t REPEAT_COUNT = 70;
  constexpr std::uint64_t RELEASE_AT = REPEAT_START + REPEAT_INTERVAL * REPEAT_COUNT;
  std::vector<example::ReplayTerminal::Input> input;

  for (std::uint64_t i = 0; i < REPEAT_COUNT; ++i) {
    input.push_back({REPEAT_START + i * REPEAT_INTERVAL, backwards ? "\x1b[A" : "\x1b[B", {}});
  }

  ObservedTerminal terminal(std::move(input), RELEASE_AT + 300);
  auto app = take(Application::create({}, terminal.clock()));
  dashboard_test::Options options;

  options.rate = 0;
  options.delayMs = delayMs;

  dashboard_test::Scene scene(*app, options, presentDataState);

  take(app->router().setFocus(scene.table()));
  take(setVirtualTableFirst(app->tree(), scene.table(), initialFirst));

  constexpr std::size_t SCREEN_WIDTH = 80;
  constexpr std::size_t TABLE_FIRST_ROW = 5;
  constexpr std::size_t CHART_FIRST_ROW = 14;
  constexpr std::size_t INSTRUMENT_WIDTH = 13;
  test_support::VtOracle oracle(SCREEN_WIDTH, 24);
  std::size_t consumed{}, heldSnapshots{}, blankSnapshots{}, blankCharts{};
  bool finalReady{};

  terminal.beforeRead = [&] {
    oracle.feed(std::string_view(terminal.output).substr(consumed));
    consumed = terminal.output.size();
    const auto clock = terminal.clock();
    const auto now = clock.read(clock.context);
    std::uint64_t first{};

    take(app->tree().read(scene.table(), [&](const auto &table) {
      first = table.visibleRange().first;
      finalReady =
        (backwards ? table.firstRow() < INITIAL_PAGE_ROWS : table.firstRow() > INITIAL_PAGE_ROWS) &&
        table.rangeState() == RangeState::READY &&
        table.selection() == RowId{backwards ? initialFirst - REPEAT_COUNT + 2 : initialFirst + REPEAT_COUNT, 1};
    }));

    if (now < REPEAT_START + delayMs || now >= RELEASE_AT || first < INITIAL_PAGE_ROWS) {
      return;
    }

    ++heldSnapshots;
    const auto frame = oracle.snapshot();
    bool hasData{};

    // At 80x24 the chart follows nine table body rows, below four controls and a header.
    for (std::size_t y = TABLE_FIRST_ROW; y < CHART_FIRST_ROW; ++y) {
      for (std::size_t x = 0; x < INSTRUMENT_WIDTH; ++x) {
        const auto &glyph = frame.cells[y * SCREEN_WIDTH + x].glyph;

        hasData = hasData || (glyph.size() == 1 && glyph[0] >= '0' && glyph[0] <= '9');
      }
    }

    blankSnapshots += !hasData;
    bool hasCandle{};

    for (const auto &cell : frame.cells | std::views::drop(SCREEN_WIDTH * CHART_FIRST_ROW)) {
      hasCandle |= cell.glyph == "\xe2\x96\x88" || cell.glyph == "\xe2\x94\x82" || cell.glyph == "\xe2\x94\x81";
    }

    blankCharts += !hasCandle;
  };

  take(app->run(scene, terminal));
  check(heldSnapshots >= 5);
  check(finalReady);
  check(!app->tree().inspect(scene.table()));

  if (delayMs ? blankSnapshots == heldSnapshots : blankSnapshots != 0) {
    throw std::runtime_error("continuous Down beyond initial page: " + std::to_string(blankSnapshots) + "/" +
                             std::to_string(heldSnapshots) + " held snapshots contain only loading placeholders");
  }

  if (delayMs ? blankCharts == heldSnapshots : blankCharts != 0) {
    throw std::runtime_error("continuous Down: " + std::to_string(blankCharts) + "/" + std::to_string(heldSnapshots) +
                             " held snapshots have no chart candles");
  }
}

void noOpDiagnosticAction() {
  auto diagnostics = std::make_shared<Diagnostics>();
  take(diagnostics->enable({2048}));
  ApplicationOptions options;
  options.scheduler.diagnostics = diagnostics;
  example::ReplayTerminal terminal({{100, "\t\t\r", {}}}, 300);
  auto app = take(Application::create(options, terminal.clock()));
  dashboard_test::Options data;
  data.rate = 0;
  dashboard_test::Scene scene(*app, data);
  take(app->run(scene, terminal));

  const auto trace = diagnostics->snapshot();
  std::uint64_t unchanged{};

  for (const auto &record : trace.records) {
    if (record.reason == DiagnosticReason::VALUE_UNCHANGED) {
      unchanged = record.context.correlationId;
    }
  }

  check(unchanged != 0);
  check(std::ranges::any_of(trace.records, [&](const auto &record) {
    return record.context.correlationId == unchanged && record.reason == DiagnosticReason::FRAME_NOT_REQUESTED;
  }));
}

void delayedLoadingSettles(bool hideDuringLoad, bool presentDataState = false) {
  constexpr std::uint64_t LOAD_DELAY_MS = 250;
  constexpr std::uint64_t HIDE_AT_MS = 100;
  constexpr std::uint64_t SHOW_AT_MS = 500;
  constexpr std::uint64_t IDLE_AT_MS = 800;
  ObservedTerminal terminal({}, 1200);
  auto app = take(Application::create({}, terminal.clock()));
  dashboard_test::Options options;

  options.rate = 0;
  options.delayMs = LOAD_DELAY_MS;

  dashboard_test::Scene scene(*app, options, presentDataState);
  bool hidden{}, shown{}, idleStarted{}, idleReady{};
  std::size_t idleBytes{};

  take(setVirtualTableFirst(app->tree(), scene.table(), 10000));

  terminal.beforeRead = [&] {
    const auto clock = terminal.clock();
    const auto now = clock.read(clock.context);

    if (hideDuringLoad && !hidden && now >= HIDE_AT_MS) {
      take(app->tree().setVisible(scene.table(), false));
      hidden = true;
    }

    if (hidden && !shown && now >= SHOW_AT_MS) {
      take(app->tree().setVisible(scene.table(), true));
      shown = true;
    }

    if (now < IDLE_AT_MS) {
      return;
    }

    if (!idleStarted) {
      idleStarted = true;
      idleBytes = terminal.output.size();

      take(app->tree().read(scene.table(), [&](const auto &table) {
        idleReady = table.rangeState() == RangeState::READY;
      }));
    }

    check(terminal.output.size() == idleBytes);
  };

  const auto result = take(app->run(scene, terminal));
  const auto loading = scene.loadingMetrics();

  check(terminal.closed && idleStarted && idleReady && (!hideDuringLoad || shown));
  check(result.frames < 10 && scene.coherent());
  check(loading.usefulLoads > 0 && loading.activeJobs == 0 && loading.cachedPages == 0);
}

void continuousNavigationWithMarketTicks(bool presentDataState = false) {
  constexpr std::uint64_t DELAY_MS = 250;
  constexpr std::uint64_t REPEAT_START_MS = 100;
  constexpr std::uint64_t REPEAT_END_MS = 1600;
  constexpr std::uint64_t REPEAT_INTERVAL_MS = 10;
  std::vector<example::ReplayTerminal::Input> input;

  for (auto now = REPEAT_START_MS; now < REPEAT_END_MS; now += REPEAT_INTERVAL_MS) {
    input.push_back({now, "\x1b[B", {}});
  }

  ObservedTerminal terminal(std::move(input), REPEAT_END_MS + DELAY_MS + 100);
  auto app = take(Application::create({}, terminal.clock()));
  dashboard_test::Options options;

  options.rate = 1000;
  options.delayMs = DELAY_MS;

  dashboard_test::Scene scene(*app, options, presentDataState);
  std::size_t readyWhileHeld{};
  std::size_t usefulWhileHeld{}, consumed{};
  constexpr std::size_t SCREEN_WIDTH = 80;
  constexpr std::size_t TABLE_FIRST_ROW = 5;
  constexpr std::size_t CHART_FIRST_ROW = 14;
  constexpr std::size_t INSTRUMENT_WIDTH = 13;
  test_support::VtOracle oracle(SCREEN_WIDTH, 24);

  take(app->router().setFocus(scene.table()));
  take(setVirtualTableFirst(app->tree(), scene.table(), 10000));

  terminal.beforeRead = [&] {
    oracle.feed(std::string_view(terminal.output).substr(consumed));
    consumed = terminal.output.size();
    const auto clock = terminal.clock();
    const auto now = clock.read(clock.context);

    if (now < REPEAT_START_MS + DELAY_MS || now >= REPEAT_END_MS) {
      return;
    }

    take(app->tree().read(scene.table(), [&](const auto &table) {
      readyWhileHeld += table.rangeState() == RangeState::READY;
    }));

    const auto frame = oracle.snapshot();
    bool hasData{}, hasCandle{};

    for (std::size_t y = TABLE_FIRST_ROW; y < CHART_FIRST_ROW; ++y) {
      for (std::size_t x = 0; x < INSTRUMENT_WIDTH; ++x) {
        const auto &glyph = frame.cells[y * SCREEN_WIDTH + x].glyph;
        hasData |= glyph.size() == 1 && glyph[0] >= '0' && glyph[0] <= '9';
      }
    }

    for (const auto &cell : frame.cells | std::views::drop(SCREEN_WIDTH * CHART_FIRST_ROW)) {
      hasCandle |= cell.glyph == "\xe2\x96\x88" || cell.glyph == "\xe2\x94\x82" || cell.glyph == "\xe2\x94\x81";
    }

    usefulWhileHeld += hasData && hasCandle;
  };

  take(app->run(scene, terminal));

  // Busy positional-selection intent has a separate preserved before/after regression.
  // This case proves loading under source revisions without claiming that issue is fixed.
  check(readyWhileHeld > 5 && usefulWhileHeld > 5 && scene.coherent() && scene.tickCount() > 1000);
}

void clockJumpRemainsBounded() {
  ApplicationOptions application;
  application.inputSlice = std::chrono::milliseconds(5000);
  example::ReplayTerminal terminal({{5000, "\x1bOQ", {}}}, 5000);

  auto app = take(Application::create(application, terminal.clock()));
  dashboard_test::Options options;
  options.rate = 10000;
  options.maxTicksPerPump = 37;
  dashboard_test::Scene scene(*app, options);

  const auto result = take(app->run(scene, terminal));
  check(scene.scheduledTicks() == 50000 && scene.tickCount() == 37);
  check(scene.turbo() && result.inputs == 1 && result.iterations <= 3);
}

void chartAppearance(bool ascii, bool turbo, ColorMode colorMode = ColorMode::TRUE_COLOR) {
  std::vector<example::ReplayTerminal::Input> input;

  if (turbo) {
    input.push_back({100, "\x1bOQ", {}});
  }

  example::ReplayTerminal terminal(std::move(input), 500);
  ApplicationOptions application;

  application.writer.colorMode = colorMode;

  auto app = take(Application::create(application, terminal.clock()));
  dashboard_test::Options options;

  options.ascii = ascii;

  dashboard_test::Scene scene(*app, options);

  take(app->run(scene, terminal));

  test_support::VtOracle oracle(80, 24);

  oracle.feed(terminal.output);

  const auto frame = oracle.snapshot();
  const auto expectedBackground = turbo                            ? RgbColor{0, 0, 170}
                                  : colorMode == ColorMode::ANSI16 ? RgbColor{0, 0, 0}
                                                                   : RgbColor{24, 28, 36};
  bool sawBody{}, sawWick{};

  // The lower ten rows are the chart; inspect empty cells and both brushes,
  // not merely the first label of an otherwise successful application run.
  for (const auto &cell : frame.cells | std::views::drop(80 * 14)) {
    check(cell.style.background == expectedBackground);
    sawBody |= cell.glyph == (ascii ? "#" : "\xe2\x96\x88");
    sawWick |= cell.glyph == (ascii ? "|" : "\xe2\x94\x82");
  }

  check(sawBody && sawWick);
}

void ordinaryAppearance(bool ascii, bool turbo, ColorMode colorMode, bool resized = false, std::uint32_t rate = 1000) {
  std::vector<example::ReplayTerminal::Input> input;

  if (turbo) {
    input.push_back({50, "\x1bOQ", {}});
  }

  input.push_back({100, "\x1b[B", {}});
  const auto editedNote = ascii ? "Edited note" : "Edited \xe7\x95\x8c e\xcc\x81";
  input.push_back({200, std::string("\t\x01") + editedNote, {}});
  input.push_back({250, "\t\r", {}});
  input.push_back({300, "\t", {}});
  for (std::uint64_t at = 350; at <= 550; at += 10) {
    input.push_back({at, "\x1b[B", {}});
  }
  if (resized) {
    input.push_back({560, "", CellSize{100, 30}});
  }
  ObservedTerminal terminal(std::move(input), 600);
  ApplicationOptions application;
  application.writer.colorMode = colorMode;
  auto app = take(Application::create(application, terminal.clock()));
  dashboard_test::Options options;
  options.rate = rate;
  // Keep the visual/focus fixture warm; the large delayed provider workload has its own exact gate.
  options.rows = 100;
  options.ascii = ascii;
  options.stress = false;
  dashboard_test::Scene scene(*app, options);
  test_support::VtOracle oracle(80, 24);
  std::size_t consumed{};
  std::size_t tableHeight{};
  CellSize viewport{80, 24};
  bool sawSelection{};

  terminal.beforeRead = [&] {
    const auto size = take(terminal.size());

    if (size != viewport) {
      oracle.resize(size.width, size.height);
      viewport = size;
    }

    oracle.feed(std::string_view(terminal.output).substr(consumed));
    consumed = terminal.output.size();
    take(app->tree().read(scene.table(), [&](const auto &table) {
      sawSelection |= table.selection().has_value();
      tableHeight = table.visibleRange().count + 1;
    }));
  };

  const auto result = take(app->run(scene, terminal));
  oracle.feed(std::string_view(terminal.output).substr(consumed));
  const auto frame = oracle.snapshot();
  const auto rowText = [&](std::size_t row) {
    std::string text;

    for (int x = 0; x < viewport.width; ++x) {
      const auto &cell = frame.cells[row * static_cast<std::size_t>(viewport.width) + x];

      if (!cell.tail) {
        text += cell.glyph;
      }
    }

    return text;
  };

  check(rowText(0).starts_with(" Portals market | F2 theme | F10 quit"));
  check(rowText(1).find(editedNote) != std::string::npos);
  check(rowText(1).find("Inspect") != std::string::npos);
  check(rowText(2).find(turbo ? "Turbo" : "Modern") != std::string::npos);
  check(rowText(2).find("ticks | Live") != std::string::npos);
  check(rowText(3).find("Instrument") != std::string::npos);
  check(tableHeight == (resized ? 18 : 14));
  check(rowText(3 + tableHeight) == std::string(viewport.width, ' '));
  bool sawBody{}, sawWick{}, sawDoji{};

  for (std::size_t y = 4 + tableHeight; y < static_cast<std::size_t>(viewport.height); ++y) {
    for (int x = 1; x < viewport.width - 1; ++x) {
      const auto &cell = frame.cells[y * static_cast<std::size_t>(viewport.width) + x];
      sawBody |= cell.glyph == (ascii ? "#" : "\xe2\x96\x88");
      sawWick |= cell.glyph == (ascii ? "|" : "\xe2\x94\x82");
      sawDoji |= cell.glyph == (ascii ? "#" : "\xe2\x94\x81");
    }
  }

  check(rate ? sawBody && sawWick : sawDoji);
  // Live data revisions retire positional intent before paint; compare busy navigation separately.
  check(rate != 0 || sawSelection);
  check(take(scene.note().snapshot())->value == editedNote);
  check(scene.coherent() && scene.tickCount() == rate * 600 / 1000);
  check(result.frames <= application.framesPerSecond * 600 / 1000 + 1);
  check(terminal.closed && !app->tree().inspect(scene.table()));
}

struct Presentation {
  SchedulerTime time{};
  std::uint64_t ticks{}, scheduled{}, revision{}, firstRow{};
  std::size_t preparedRows{};
  RangeState range{};
  std::optional<RowId> selected;
  std::string note;
  bool coherent{}, pending{}, turbo{};
  bool operator==(const Presentation &) const = default;
};

struct ComparedRun {
  std::string output, trace;
  std::vector<Presentation> checkpoints;
  std::vector<SchedulerTime> ingressLatencies;
  std::array<std::uint64_t, 5> stats;
  std::array<std::uint64_t, 10> data, control;
  std::array<std::uint64_t, 3> scheduler;
  std::array<double, 4> aggregate;
  std::optional<SchedulerTime> stoppedAt;
  std::uint64_t ticks{}, pendingTicks{};
  bool closed{}, tableRetired{};
  bool operator==(const ComparedRun &) const = default;
};

void compareRuns(const ComparedRun &before, const ComparedRun &after) {
  check(before.output == after.output);
  check(before.trace == after.trace);
  check(before.checkpoints == after.checkpoints);
  check(before.ingressLatencies == after.ingressLatencies);
  check(before.stats == after.stats);
  check(before.data == after.data && before.control == after.control && before.scheduler == after.scheduler);
  check(before.aggregate == after.aggregate);
  check(before.stoppedAt == after.stoppedAt && before.ticks == after.ticks &&
        before.pendingTicks == after.pendingTicks);
  check(before.closed == after.closed && before.tableRetired == after.tableRetired);
}

auto queueTotals(const SchedulerQueueMetrics &queue) {
  return std::array<std::uint64_t, 10>{queue.pending,     queue.high_water,   queue.accepted,  queue.rejected,
                                       queue.replaced,    queue.delivered,    queue.cancelled, queue.last_latency,
                                       queue.max_latency, queue.total_latency};
}

template <class Scene, class Options>
ComparedRun captureRun(bool replay, bool ascii, ColorMode colorMode, std::uint32_t rate, std::uint64_t delay,
                       bool saturated, std::uint64_t initialFirst, std::vector<example::ReplayTerminal::Input> input,
                       bool compact = false) {
  ComparedRun captured;
  ObservedTerminal terminal(std::move(input), 1000);
  auto diagnostics = std::make_shared<Diagnostics>();

  take(diagnostics->enable({8192}));

  ApplicationOptions application;

  application.writer.colorMode = colorMode;
  application.scheduler.diagnostics = diagnostics;

  if (saturated) {
    application.scheduler.data_capacity = application.scheduler.data_budget = 1;
    application.scheduler.control_capacity = application.scheduler.control_budget = 1;
  }

  auto app = take(Application::create(application, terminal.clock()));
  Options options;

  options.rate = rate;
  options.delayMs = delay;
  options.replay = replay;
  options.ascii = ascii;
  if constexpr (std::same_as<Options, dashboard_test::Options>) {
    options.stress = !compact;
  }
  options.ingressObserver = [](void *context, SchedulerTime latency) {
    static_cast<std::vector<SchedulerTime> *>(context)->push_back(latency);
  };
  options.ingressContext = &captured.ingressLatencies;

  Scene scene(*app, options);

  take(setVirtualTableFirst(app->tree(), scene.table(), initialFirst));

  terminal.beforeRead = [&] {
    const auto clock = terminal.clock();
    Presentation state{
      clock.read(clock.context), scene.tickCount(), scene.scheduledTicks(), scene.displayedRevision(), 0,
      scene.maxPreparedRows()};

    state.note = take(scene.note().snapshot())->value;
    state.coherent = scene.coherent();
    state.pending = scene.pendingVisible();
    state.turbo = scene.turbo();
    take(app->tree().read(scene.table(), [&](const auto &table) {
      state.firstRow = table.firstRow();
      state.range = table.rangeState();
      state.selected = table.selection();
    }));
    captured.checkpoints.push_back(std::move(state));
  };

  const auto result = take(app->run(scene, terminal));
  const auto metrics = app->scheduler().metrics();
  const auto aggregate = scene.aggregate();
  auto trace = diagnostics->snapshot();

  // Tree session IDs are deliberately process-unique; normalize only that identity namespace.
  for (auto &record : trace.records) {
    record.stableNodeId.session = record.stableNodeId.session ? 1 : 0;
  }

  captured.output = terminal.output;
  captured.trace = exportDiagnostics(trace);
  captured.stats = {result.iterations, result.frames, result.flushes, result.bytes, result.inputs};
  captured.data = queueTotals(metrics.data);
  captured.control = queueTotals(metrics.control);
  captured.scheduler = {metrics.timer_callbacks, metrics.callback_failures, metrics.cycles};
  captured.aggregate = {aggregate.open, aggregate.high, aggregate.low, aggregate.close};
  captured.stoppedAt = scene.stoppedAt();
  captured.ticks = scene.tickCount();
  captured.pendingTicks = scene.pendingTicks();
  captured.closed = terminal.closed;
  captured.tableRetired = !app->tree().inspect(scene.table());

  return captured;
}

void ordinaryBusyInputComparison() {
  std::vector<example::ReplayTerminal::Input> input{{100, "\x1b[B", {}},
                                                    {200,
                                                     "\t\x01"
                                                     "Edited note",
                                                     {}},
                                                    {250, "\t\r", {}},
                                                    {300, "\t", {}}};

  for (std::uint64_t at = 350; at <= 550; at += 10) {
    input.push_back({at, "\x1b[B", {}});
  }

  const auto before = captureRun<dashboard_before::Scene, dashboard_before::Options>(
    false, false, ColorMode::TRUE_COLOR, 1000, 0, false, 0, input);
  const auto after = captureRun<dashboard_test::Scene, dashboard_test::Options>(false, false, ColorMode::TRUE_COLOR,
                                                                                1000, 0, false, 0, input, true);

  check(before.checkpoints.size() == after.checkpoints.size());

  for (std::size_t i = 0; i < before.checkpoints.size(); ++i) {
    check(before.checkpoints[i].time == after.checkpoints[i].time);
    check(before.checkpoints[i].selected == after.checkpoints[i].selected);
    check(before.checkpoints[i].note == after.checkpoints[i].note);
  }

  check(before.ticks == after.ticks && before.aggregate == after.aggregate);
  check(after.closed && after.tableRetired && after.pendingTicks == 0);
}

void exactBorrowedSceneComparison(ColorMode mode) {
  std::vector<example::ReplayTerminal::Input> navigation;

  // Cross the initial cached page, then navigate backwards while table/chart work completes independently.
  for (std::uint64_t i = 0; i < 35; ++i) {
    navigation.push_back({100 + i * 10, "\x1b[B", {}});
  }

  navigation.push_back({500, "\x1b[1;7Q", {}}); // Modified F2 keeps the old modifier-insensitive semantics.
  navigation.push_back({550,
                        "\t\x01"
                        "Edited \xe7\x95\x8c e\xcc\x81\r",
                        {}});
  navigation.push_back({600, "\t\r", {}}); // Inspect publishes the unchanged note.
  navigation.push_back({650, "\t\x1b[A\x1b[H", {}});
  navigation.push_back({700, "", CellSize{100, 30}});

  for (const auto ascii : {false, true}) {
    for (const auto replay : {false, true}) {
      for (const auto saturated : {false, true}) {
        const auto before = captureRun<dashboard_before::Scene, dashboard_before::Options>(
          replay, ascii, mode, saturated ? 10000 : 1000, 250, saturated, 240, navigation);
        const auto after = captureRun<dashboard_test::Scene, dashboard_test::Options>(
          replay, ascii, mode, saturated ? 10000 : 1000, 250, saturated, 240, navigation);

        compareRuns(before, after);
        check(after.closed && after.tableRetired && after.stoppedAt.has_value());
      }
    }
  }

  if (mode != ColorMode::TRUE_COLOR) {
    return;
  }

  // Idle, immediate F10 and Ctrl+C exercise quiet pumping and early provider/terminal cleanup.
  for (const auto &input : {std::vector<example::ReplayTerminal::Input>{},
                            std::vector<example::ReplayTerminal::Input>{{100, "\x1b[21~", {}}},
                            std::vector<example::ReplayTerminal::Input>{{100, "\x03", {}}}}) {
    const auto before = captureRun<dashboard_before::Scene, dashboard_before::Options>(
      false, false, ColorMode::TRUE_COLOR, 0, 0, false, 0, input);
    const auto after = captureRun<dashboard_test::Scene, dashboard_test::Options>(false, false, ColorMode::TRUE_COLOR,
                                                                                  0, 0, false, 0, input);

    compareRuns(before, after);
    check(after.stats[1] <= 1 && after.ticks == 0 && after.closed && after.tableRetired);
  }
}

void exactModifiedShortcutComparison() {
  auto beforeApp = take(Application::create());
  auto afterApp = take(Application::create());
  dashboard_before::Scene before(*beforeApp, {.rate = 0});
  dashboard_test::Scene after(*afterApp, {.rate = 0});

  for (unsigned mask = 0; mask < 16; ++mask) {
    const InputModifiers modifiers{bool(mask & 1), bool(mask & 2), bool(mask & 4), bool(mask & 8)};

    for (const auto action : {KeyAction::PRESS, KeyAction::REPEAT, KeyAction::RELEASE}) {
      const InputEvent event = KeyEvent{InputKey::F2, {}, modifiers, action};

      check(take(before.input(*beforeApp, event)) == take(after.input(*afterApp, event)));
      check(before.turbo() == after.turbo());
    }
  }

  for (const auto action : {KeyAction::PRESS, KeyAction::REPEAT, KeyAction::RELEASE}) {
    for (const auto key : {KeyEvent{InputKey::F10, {}, {.alt = true}, action},
                           KeyEvent{InputKey::CHARACTER, U'c', {.shift = true, .control = true}, action}}) {
      check(take(before.input(*beforeApp, key)) == take(after.input(*afterApp, key)));
    }
  }
}

enum class TransportFailure {
  READ,
  SUBMIT,
  CLOSE
};

class FailingTerminal final : public example::ReplayTerminal {
public:
  explicit FailingTerminal(TransportFailure failure) : ReplayTerminal({}, 100), failure_(failure) {
  }

  TerminalRead read(TerminalDeadline deadline, std::stop_token stop) override {
    if (failure_ == TransportFailure::READ) {
      return {.status = TerminalStatus::FAILED, .error = TerminalError{TerminalErrorKind::INPUT_DECODE, 71}};
    }

    return ReplayTerminal::read(deadline, stop);
  }

  std::expected<void, TerminalError> submit(std::string bytes) override {
    if (failure_ == TransportFailure::SUBMIT) {
      return std::unexpected(TerminalError{TerminalErrorKind::OUTPUT_LIMIT, 72});
    }

    return ReplayTerminal::submit(std::move(bytes));
  }

  std::expected<void, TerminalError> close() noexcept override {
    auto closeResult = ReplayTerminal::close();

    if (failure_ == TransportFailure::CLOSE) {
      return std::unexpected(TerminalError{TerminalErrorKind::OUTPUT_UNCERTAIN, 73});
    }

    return closeResult;
  }

private:
  TransportFailure failure_;
};

template <class Scene, class Options> auto captureFailure(TransportFailure failure, Options options = {}) {
  FailingTerminal terminal(failure);
  auto app = take(Application::create({}, terminal.clock()));
  Scene scene(*app, options);
  const auto result = app->run(scene, terminal);

  check(!result && terminal.closed && scene.stoppedAt().has_value());
  check(!app->tree().inspect(scene.table()));
  check(take(scene.note().snapshot())->value == "Market note");

  const auto &error = result.error();
  const auto &cause = std::get<TerminalError>(error.cause);
  const auto stoppedAt = scene.stoppedAt();

  scene.stopProvider();
  check(scene.stoppedAt() == stoppedAt);

  return std::array<std::uint64_t, 8>{std::uint64_t(error.phase),
                                      std::uint64_t(cause.kind),
                                      std::uint64_t(cause.native_code),
                                      error.restoration ? std::uint64_t(error.restoration->kind) + 1 : 0,
                                      error.restoration ? std::uint64_t(error.restoration->native_code) : 0,
                                      *scene.stoppedAt(),
                                      scene.tickCount(),
                                      scene.pendingTicks()};
}

void exactTransportFailureComparison() {
  for (const auto failure : {TransportFailure::READ, TransportFailure::SUBMIT, TransportFailure::CLOSE}) {
    check((captureFailure<dashboard_before::Scene, dashboard_before::Options>(failure) ==
           captureFailure<dashboard_test::Scene, dashboard_test::Options>(failure)));
    check((captureFailure<dashboard_before::Scene, dashboard_before::Options>(failure) ==
           captureFailure<dashboard_test::Scene, dashboard_test::Options>(failure, {.stress = false})));
  }
}

void unrunSceneRetiresBorrowedCallbacks() {
  auto app = take(Application::create());
  ManagedState<std::string> observedNote;
  Node<VirtualTable<dashboard_test::Quote>> observedTable;

  {
    dashboard_test::Scene scene(*app, {.rate = 0});

    observedNote = scene.note();
    observedTable = scene.table();
  }

  check(!app->tree().inspect(observedTable));
  check(!observedNote.snapshot() && observedNote.snapshot().error() == TreeError::CLOSED);
  check(app->scheduler().metrics().data.pending == 0 && app->scheduler().metrics().control.pending == 0);
  take(app->scheduler().dispatch());
  check(app->scheduler().metrics().timer_callbacks == 0);
}
} // namespace

int main(int argc, char **argv) {
  try {
    if (argc > 2) {
      throw std::runtime_error("Expected one dashboard regression group");
    }

    const std::string_view group = argc == 2 ? argv[1] : "";

    if (group == "--data-presentation-loading") {
      constexpr std::uint64_t NETWORK_DELAY_MS = 250;

      continuousNavigationKeepsRowsVisible(false, 0, true);
      continuousNavigationKeepsRowsVisible(true, 0, true);
      continuousNavigationKeepsRowsVisible(false, NETWORK_DELAY_MS, true);
      continuousNavigationKeepsRowsVisible(true, NETWORK_DELAY_MS, true);
      delayedLoadingSettles(false, true);
      delayedLoadingSettles(true, true);
      continuousNavigationWithMarketTicks(true);

      return 0;
    }

    if (group == "--delayed-continuous") {
      constexpr std::uint64_t NETWORK_DELAY_MS = 250;

      continuousNavigationKeepsRowsVisible(false, NETWORK_DELAY_MS);
      continuousNavigationKeepsRowsVisible(true, NETWORK_DELAY_MS);
      delayedLoadingSettles(false);
      delayedLoadingSettles(true);
      continuousNavigationWithMarketTicks();

      return 0;
    }

    if (group == "--exact-truecolor" || group == "--exact-ansi16") {
      exactBorrowedSceneComparison(group == "--exact-truecolor" ? ColorMode::TRUE_COLOR : ColorMode::ANSI16);

      return 0;
    }

    if (group == "--lifecycle") {
      exactModifiedShortcutComparison();
      exactTransportFailureComparison();
      unrunSceneRetiresBorrowedCallbacks();

      return 0;
    }

    if (group == "--ordinary-truecolor" || group == "--ordinary-ansi16") {
      const auto mode = group == "--ordinary-truecolor" ? ColorMode::TRUE_COLOR : ColorMode::ANSI16;

      if (mode == ColorMode::TRUE_COLOR) {
        ordinaryBusyInputComparison();
        ordinaryAppearance(false, false, mode, false, 0);
      }

      for (const auto ascii : {false, true}) {
        ordinaryAppearance(ascii, false, mode);
        ordinaryAppearance(ascii, true, mode);
        ordinaryAppearance(ascii, true, mode, true);
        ordinaryAppearance(ascii, true, mode, true, 0);
      }

      return 0;
    }

    if (!group.empty()) {
      throw std::runtime_error("Unknown dashboard regression group");
    }

    // Preserve the original no-argument regression workload and its existing timeout.
    losslessAndSaturated(false);
    losslessAndSaturated(true);
    pendingLatestOwnsRejectedValue();
    delayedRangesAndKeyboard();
    continuousNavigationKeepsRowsVisible(false);
    continuousNavigationKeepsRowsVisible(true);
    clockJumpRemainsBounded();
    noOpDiagnosticAction();
    chartAppearance(false, false);
    chartAppearance(false, true);
    chartAppearance(true, false);
    chartAppearance(true, false, ColorMode::ANSI16);
    chartAppearance(false, true, ColorMode::ANSI16);
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
  }
}
