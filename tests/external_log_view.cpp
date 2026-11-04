// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../src/widgets/external_log_view.hpp"
#include "../src/widgets/log_view_state.hpp"
#include "support/examples/replay/terminal.hpp"
#include <doctest.h>
#include <portals/ui.hpp>
#include <portals/ui_log_source_model.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <source_location>
#include <thread>

namespace {
using namespace portals;

template <class T, class E>
T take(std::expected<T, E> value, std::source_location origin = std::source_location::current()) {
  INFO("checked call: ", std::string_view(origin.file_name()), ":", origin.line());

  if (!value) {
    if constexpr (std::same_as<E, ManagedError>) {
      INFO("managed phase=", static_cast<int>(value.error().phase), " cause alternative=", value.error().cause.index());
      if (const auto *tree = std::get_if<TreeError>(&value.error().cause)) {
        INFO("tree error=", static_cast<int>(*tree));
        REQUIRE(value);
      }
    } else if constexpr (std::is_enum_v<E>) {
      INFO("error=", static_cast<int>(value.error()));
      REQUIRE(value);
    }
  }

  REQUIRE(value);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

class Executor final : public BackgroundExecutor {
public:
  std::shared_ptr<BackgroundJob> job;
  std::uint64_t epoch{};
  std::size_t submissions{};
  bool accepting{true};
  std::optional<BackgroundError> failure;

  std::expected<BackgroundTicket, BackgroundError> submit(std::unique_ptr<BackgroundJob> &input) override {
    ++submissions;
    if (failure) {
      return std::unexpected(*failure);
    }

    if (!accepting || job) {
      return std::unexpected(BackgroundError::WOULD_BLOCK);
    }

    job = std::move(input);

    return BackgroundTicket(job);
  }

  void service() noexcept override {
  }

  std::uint64_t capacityEpoch() const noexcept override {
    return epoch;
  }

  void runOne() {
    auto work = std::exchange(job, {});

    if (!work) {
      return;
    }

    std::expected<void, AsyncError> published;
    std::thread worker([&] {
      work->run(work->stopToken());
      published = work->publish();
    });

    worker.join();
    CHECK((published || published.error() == AsyncError::CLOSED || published.error() == AsyncError::CANCELLED));
    ++epoch;
  }
};

struct Fixture {
  Executor executor;
  SchedulerTime now{};
  std::unique_ptr<Application> app =
    take(Application::create({.backgroundExecutor = &executor}, {[](void *context) noexcept {
                                                                   return *static_cast<SchedulerTime *>(context);
                                                                 },
                                                                 &now}));
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));
  ui::Ref<LogView> reference;
  Node<LogView> node;
  Node<ScrollArea> ancestor;
  CellSize size{80, 24};

  explicit Fixture(ui::LogSource source, bool tail = false, bool nested = false) {
    auto recipe = ui::logView(reference, std::move(source), {.followTail = tail});

    if (nested) {
      ui::Ref<ScrollArea> outer;

      take(ui::mount(*scene, ui::scrollArea(outer, ui::vbox(std::move(recipe).height(24),
                                                            ui::label("scrolling content").height(100)))));
      node = reference.node();
      ancestor = outer.node();
    } else {
      node = take(ui::mount(*scene, std::move(recipe)));
    }
  }

  ~Fixture() {
    scene.reset();
    executor.runOne();
    app.reset();
  }

  void pump() {
    take(app->scheduler().dispatch());
    take(scene->afterDispatch(*app));
    take(scene->prepare(*app, size));

    const auto bounds = take(CellRect::from({}, size));

    for (unsigned pass = 0; pass < 3; ++pass) {
      take(app->tree().consumeFrameRequest());
      std::optional<LayoutSnapshot> layout;

      take(app->tree().frame([&] {
        take(layoutTree(app->tree(), app->root(), bounds, bounds));
        layout.emplace(take(LayoutSnapshot::capture(app->tree(), app->root(), LayoutSnapshotProfile::GEOMETRY)));
      }));
      take(scene->prepareViews(*app, *layout));

      if (take(app->tree().consumeFrameRequest()) != Invalidation::LAYOUT) {
        break;
      }
    }

    take(scene->completedLayout(*app));
  }

  void settle(std::size_t iterations = 80) {
    for (std::size_t step = 0; step < iterations; ++step) {
      pump();
      executor.runOne();
    }

    pump();
  }

  void dispatchInput(InputEvent event) {
    if (!take(scene->input(*app, event))) {
      take(app->router().dispatch(event));
    }
  }

  void input(InputEvent event) {
    dispatchInput(std::move(event));

    settle(4);
  }

  std::uint64_t visibleOrdinal() {
    std::uint64_t result{};

    take(app->tree().read(node, [&](const LogView &widget) {
      const auto &state = *detail::LogViewAccess::state(widget).external;

      REQUIRE(state.frame);
      REQUIRE(state.offset < state.frame->anchors.size());
      result = state.frame->anchors[state.offset].ordinal;
    }));

    return result;
  }

  std::shared_ptr<const detail::ExternalLogFrame> frame() {
    std::shared_ptr<const detail::ExternalLogFrame> result;

    take(app->tree().read(node, [&](const LogView &widget) {
      result = detail::LogViewAccess::state(widget).external->frame;
    }));

    return result;
  }
};

class ShortPages final : public WindowProvider<LogLine> {
public:
  std::atomic<std::size_t> reads{};
  std::atomic<bool> uiRead{};
  const std::thread::id ui = std::this_thread::get_id();
  bool empty{};
  bool fail{};

  SourceCapabilities capabilities() const noexcept override {
    return {true, false, true};
  }

  std::expected<SourceReadReply<LogLine>, SourceError> load(const SourceReadCommand &command, PageBuilder<LogLine> &out,
                                                            std::stop_token stop) override {
    ++reads;

    if (std::this_thread::get_id() == ui) {
      uiRead = true;
    }

    if (stop.stop_requested()) {
      return std::unexpected(SourceError::CANCELLED);
    }

    if (fail) {
      return std::unexpected(SourceError::IO_ERROR);
    }

    auto position = command.anchor.kind == SourceAnchorKind::TAIL     ? std::uint64_t{100}
                    : command.anchor.kind == SourceAnchorKind::CURSOR ? command.anchor.cursor.token[0]
                                                                      : 0;
    const bool backward = command.direction == SourceDirection::BACKWARD;
    const auto first = backward && position ? position - 1 : position;
    auto next = first;

    if (empty) {
      ++next;
    } else if (first < 100 && (!backward || position)) {
      std::pmr::string text(out.workspace());

      text.append("record ");
      text.append(std::to_string(first));
      auto added = out.emplace({first + 1, 1}, 1, first, std::string_view(text), out.resource());

      if (!added) {
        return std::unexpected(added.error());
      }
      ++next;
    }

    const auto edge = !empty && (backward ? first == 0 : next == 100) ? SourceEdge::FINAL_END : SourceEdge::MORE;

    return out.ready({command.source, {first, 0}}, {command.source, {next, 0}}, edge);
  }
};

class RichRows final : public ModelSnapshot<std::string> {
public:
  mutable std::atomic<std::uint64_t> lastFirst{};
  mutable std::atomic<bool> uiRead{};
  const std::thread::id ui = std::this_thread::get_id();
  std::vector<ModelRow<std::string>> rows;

  explicit RichRows(std::uint64_t count = 500) : ModelSnapshot(7, count) {
    for (std::uint64_t index = 0; index < rowCount(); ++index) {
      rows.push_back({{index + 100, 3}, "rich record " + std::to_string(index)});
    }
  }

  ModelRows<std::string> read(RowRange range) const override {
    lastFirst = range.first;

    if (std::this_thread::get_id() == ui) {
      uiRead = true;
    }

    return {RangeState::READY, std::span(rows).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }
};

struct TemporaryLog {
  std::filesystem::path path =
    std::filesystem::temp_directory_path() /
    ("portals-external-log-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");

  ~TemporaryLog() {
    std::error_code error;

    std::filesystem::remove(path, error);
  }

  void write(std::string_view value, bool append = false) {
    std::ofstream output(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));

    output.write(value.data(), static_cast<std::streamsize>(value.size()));
    REQUIRE(output.good());
  }

  std::string utf8() const {
    const auto text = path.u8string();

    return {reinterpret_cast<const char *>(text.data()), text.size()};
  }
};
} // namespace

TEST_CASE("short provider pages fill a viewport across bounded source turns") {
  auto provider = std::make_shared<ShortPages>();
  auto source = ui::logSource(provider);
  Fixture fixture(source);

  fixture.settle();

  const auto frame = fixture.frame();

  REQUIRE(frame);
  CHECK(frame->rows.size() >= 22);
  CHECK(provider->reads > 4);
  CHECK(provider->reads <= 40);
  CHECK_FALSE(provider->uiRead.load());
  REQUIRE(source.budget());
  CHECK(source.budget()->usage().bytes <= source.budget()->options().maxBytes);
  CHECK(frame->footer->bytes().starts_with("Local window:"));
}

TEST_CASE("empty MORE pages stop at the source fairness limit") {
  auto provider = std::make_shared<ShortPages>();

  provider->empty = true;

  Fixture fixture(ui::logSource(provider));

  fixture.settle();

  CHECK(provider->reads == 4);

  auto diagnostics = take(fixture.scene->diagnostics());

  REQUIRE_FALSE(diagnostics.errors.empty());
  CHECK(diagnostics.errors.back().error.source == SourceError::CONTINUATION_LIMIT);
}

TEST_CASE("rich finite strings retain exact positional seek without locateRow") {
  auto rows = std::make_shared<RichRows>();
  std::shared_ptr<const ModelSnapshot<std::string>> snapshot = rows;
  auto source = ui::logSource(snapshot);
  Fixture fixture(source);

  fixture.settle(4);
  REQUIRE(source.rowCount() == 500);
  take(seekLogRow(fixture.app->tree(), fixture.node, 420));
  fixture.settle(4);

  const auto frame = fixture.frame();

  REQUIRE(frame);
  REQUIRE_FALSE(frame->rows.empty());
  CHECK(frame->rows.front()->bytes() == "rich record 420");
  CHECK(rows->lastFirst == 420);
  CHECK_FALSE(rows->uiRead.load());
  CHECK_FALSE(seekLogByte(fixture.app->tree(), fixture.node, 420));
}

TEST_CASE("explicit finite seek before the first preparation remains the initial intent") {
  for (const auto followTail : {false, true}) {
    CAPTURE(followTail);

    auto rows = std::make_shared<RichRows>();
    std::shared_ptr<const ModelSnapshot<std::string>> snapshot = rows;
    Fixture fixture(ui::logSource(snapshot), followTail);

    take(seekLogRow(fixture.app->tree(), fixture.node, 50));
    fixture.settle(6);

    REQUIRE(fixture.frame());
    CHECK(fixture.visibleOrdinal() == 50);
    CHECK(fixture.frame()->rows.front()->bytes() == "rich record 50");
    CHECK(take(fixture.scene->diagnostics()).errors.empty());
  }
}

TEST_CASE("finite codec workspace exhaustion preserves the page limit classification") {
  auto rows = std::make_shared<RichRows>();
  std::shared_ptr<const ModelSnapshot<std::string>> snapshot = rows;
  auto descriptor = ui::logSource(snapshot, [](const std::string &, std::pmr::string &text) {
    text.append(64, 'x');
  });
  auto data = ui::detail::ExternalLogAccess::data(descriptor);
  REQUIRE(data);
  auto scheduler = take(Scheduler::create());
  auto tree = take(WidgetTree::create(*scheduler));
  auto root = take(tree->root());
  WindowSourceOptions options;
  options.initialRevision = snapshot->revision();
  options.workspaceBytes = SourceArena::METADATA_BYTES + 1;
  auto source = take(WindowSource<LogLine>::create(*tree, root, data->provider, options));
  auto consumer = take(source->openConsumer());
  auto request = take(consumer.request({}, SourceDirection::FORWARD, 1));
  std::expected<SourceReadReply<LogLine>, SourceError> result = std::unexpected(SourceError::INVALID_REPLY);
  std::thread worker([&] {
    result = request.load();
  });

  worker.join();
  REQUIRE_FALSE(result);
  CHECK(result.error() == SourceError::PAGE_TOO_LARGE);
}

TEST_CASE("removing a node cancels retained work before late worker publication") {
  auto provider = std::make_shared<ShortPages>();
  auto source = ui::logSource(provider);
  Fixture fixture(source);

  fixture.pump();
  fixture.pump();
  REQUIRE(fixture.executor.job);
  take(fixture.scene->remove(fixture.node));
  fixture.executor.runOne();
  take(fixture.app->scheduler().dispatch());

  CHECK(source.budget()->usage().work == 0);
  CHECK(source.budget()->usage().pages == 0);
  CHECK(source.budget()->usage().consumers == 0);
}

TEST_CASE("source failure preserves the last committed prepared window") {
  auto provider = std::make_shared<ShortPages>();
  Fixture fixture(ui::logSource(provider));

  fixture.settle();

  const auto frame = fixture.frame();

  REQUIRE(frame);
  provider->fail = true;
  take(fixture.app->tree().edit(fixture.node, [](LogView &widget) {
    detail::LogViewAccess::state(widget).external->tailRequested = true;
  }));
  fixture.settle(4);

  CHECK(fixture.frame() == frame);

  auto diagnostics = take(fixture.scene->diagnostics());

  REQUIRE_FALSE(diagnostics.errors.empty());
  CHECK(diagnostics.errors.back().error.source == SourceError::IO_ERROR);
}

TEST_CASE("file factory defers open failures and keeps exact native diagnostics") {
  {
    auto app = take(Application::create());
    auto scene = take(ManagedScene::create(*app));
    auto mounted = ui::mount(*scene, ui::logView(ui::logFile(std::string_view{})));

    REQUIRE_FALSE(mounted);
    CHECK(mounted.error().source == SourceError::INVALID_ARGUMENT);
    REQUIRE(mounted.error().logFile);
    CHECK(mounted.error().logFile->operation == FileLogOperation::CREATE);
  }

  TemporaryLog absent;
  auto source = ui::logFile(absent.utf8());

  REQUIRE(source.fileModel());

  Fixture fixture(source);

  fixture.settle(4);

  auto diagnostics = take(fixture.scene->diagnostics());

  REQUIRE_FALSE(diagnostics.errors.empty());
  const auto &error = diagnostics.errors.back().error;

  CHECK(error.source == SourceError::IO_ERROR);
  REQUIRE(error.logFile);
  CHECK(error.logFile->error == FileLogError::OPEN_FAILED);
  CHECK(error.logFile->nativeCode != 0);
}

TEST_CASE("unchanged tail polls metadata without replacing prepared text") {
  TemporaryLog file;

  file.write("first\nsecond\n");

  Fixture fixture(ui::logFile(file.utf8()), true);

  fixture.settle(5);

  const auto initial = fixture.frame();

  REQUIRE(initial);
  REQUIRE(initial->records == 2);
  fixture.now = 250;
  fixture.settle(3);
  CHECK(fixture.frame() == initial);

  file.write("third\n", true);
  fixture.now = 500;
  fixture.settle(8);

  const auto grown = fixture.frame();

  REQUIRE(grown);
  CHECK(grown != initial);
  CHECK(grown->records == 3);
  CHECK(grown->rows.back()->bytes() == "third");
}

TEST_CASE("file truncation replaces the source namespace before installing new records") {
  TemporaryLog file;

  file.write("first long record\nsecond long record\n");

  Fixture fixture(ui::logFile(file.utf8()), true);

  fixture.settle(5);

  const auto initial = fixture.frame();

  REQUIRE(initial);
  file.write("new\n");
  fixture.now = 250;
  fixture.settle(10);

  const auto replacement = fixture.frame();

  REQUIRE(replacement);
  CHECK(replacement->before.source != initial->before.source);
  CHECK(replacement->records == 1);
  REQUIRE(replacement->rows.size() == 1);
  CHECK(replacement->rows.front()->bytes() == "new");
  CHECK(initial->rows.front()->bytes() == "first long record");
}

TEST_CASE("external defaults accept valid file records larger than rolling admission limits") {
  TemporaryLog file;

  file.write(std::string(6000, 'x') + "\n");

  auto model = take(FileLogModel::create(file.path));
  auto source = ui::logSource(model);

  REQUIRE(source.fileModel() == model);

  Fixture fixture(source);

  fixture.settle(5);

  const auto frame = fixture.frame();

  REQUIRE(frame);
  CHECK(frame->bytes == 6000);
  CHECK(frame->rows.size() > 22);
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}

TEST_CASE("long ASCII record preparation remains within the shared reservation") {
  constexpr std::size_t RECORD_BYTES = 32769;
  constexpr std::size_t CONTENT_COLUMNS = 80;
  TemporaryLog file;

  file.write(std::string(RECORD_BYTES, 'x') + "\n");

  auto source = ui::logFile(file.utf8(), FileLogOptions{.maxRecordBytes = 64 * 1024});
  Fixture fixture(source);

  fixture.size = {static_cast<std::int32_t>(CONTENT_COLUMNS + 2), 24};
  fixture.settle(6);

  const auto frame = fixture.frame();

  REQUIRE(frame);
  CHECK(frame->records == 1);
  CHECK(frame->bytes == RECORD_BYTES);
  CHECK(frame->width == CONTENT_COLUMNS);
  CHECK(frame->rows.size() == (RECORD_BYTES + CONTENT_COLUMNS - 1) / CONTENT_COLUMNS);
  CHECK(source.budget()->usage().bytes <= source.budget()->options().maxBytes);
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}

TEST_CASE("empty replacement clears the old generation and resumes following after append") {
  TemporaryLog file;

  file.write("old record\n");

  Fixture fixture(ui::logFile(file.utf8()), true);

  fixture.settle(5);

  const auto old = fixture.frame();

  REQUIRE(old);
  REQUIRE(old->records == 1);
  file.write("");
  fixture.now = 250;
  fixture.settle(10);

  const auto empty = fixture.frame();

  REQUIRE(empty);
  CHECK(empty->records == 0);
  CHECK(empty->rows.empty());
  CHECK(empty->before.source != old->before.source);
  CHECK(empty->before.token[1] != old->before.token[1]);
  CHECK(old->rows.front()->bytes() == "old record");

  for (unsigned poll = 0; poll < 4; ++poll) {
    fixture.now += 250;
    fixture.settle(3);
    CHECK(fixture.frame() == empty);
  }

  file.write("after empty\n", true);
  fixture.now += 250;
  fixture.settle(8);

  const auto appended = fixture.frame();

  REQUIRE(appended);
  CHECK(appended->before.source == empty->before.source);
  CHECK(appended->before.token[1] == empty->before.token[1]);
  REQUIRE(appended->rows.size() == 1);
  CHECK(appended->rows.front()->bytes() == "after empty");
  CHECK(old->rows.front()->bytes() == "old record");
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}

TEST_CASE("showing a log automatically resumes its cancelled initial read") {
  auto provider = std::make_shared<ShortPages>();
  Fixture fixture(ui::logSource(provider));

  fixture.pump();
  fixture.pump();
  REQUIRE(fixture.executor.job);
  CHECK_FALSE(fixture.frame());

  take(fixture.app->tree().setVisible(fixture.node, false));
  fixture.pump();
  fixture.pump();
  fixture.executor.runOne();
  CHECK_FALSE(fixture.frame());
  take(fixture.app->tree().setVisible(fixture.node, true));
  fixture.settle();

  const auto frame = fixture.frame();

  REQUIRE(frame);
  CHECK(frame->rows.size() >= 22);
  CHECK(frame->rows.front()->bytes() == "record 0");
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}

TEST_CASE("an ancestor clip pauses file polling and revealing the log resumes it") {
  TemporaryLog file;

  file.write("first\nsecond\n");

  Fixture fixture(ui::logFile(file.utf8()), true, true);

  fixture.settle(6);

  const auto initial = fixture.frame();

  REQUIRE(initial);
  REQUIRE(initial->records == 2);
  take(setScrollOffset(fixture.app->tree(), fixture.ancestor, ScrollAxis::VERTICAL, {40, 0}));
  fixture.settle(4);

  take(fixture.app->tree().read(fixture.node, [](const LogView &widget) {
    const auto &state = *detail::LogViewAccess::state(widget).external;

    CHECK(state.viewport.width > 0);
    CHECK(state.viewport.height > 0);
    CHECK_FALSE(state.visible);
  }));

  const auto submissions = fixture.executor.submissions;

  file.write("revealed\n", true);
  fixture.now += 1000;
  fixture.settle(6);
  CHECK(fixture.executor.submissions == submissions);
  CHECK(fixture.frame() == initial);

  take(setScrollOffset(fixture.app->tree(), fixture.ancestor, ScrollAxis::VERTICAL, {0, 0}));
  fixture.settle(4);
  fixture.now += 250;
  fixture.settle(8);

  const auto revealed = fixture.frame();

  REQUIRE(revealed);
  CHECK(fixture.executor.submissions > submissions);
  CHECK(revealed->records == 3);
  CHECK(revealed->rows.back()->bytes() == "revealed");
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}

TEST_CASE("finite resize keeps the selected logical record without identity lookup") {
  auto rows = std::make_shared<RichRows>();
  std::shared_ptr<const ModelSnapshot<std::string>> snapshot = rows;
  Fixture fixture(ui::logSource(snapshot));

  fixture.settle(4);
  take(setScrollOffset(fixture.app->tree(), fixture.node, ScrollAxis::VERTICAL, {5, 0}));
  fixture.pump();
  fixture.size = {40, 24};
  fixture.settle(5);

  const auto frame = fixture.frame();

  REQUIRE(frame);
  REQUIRE_FALSE(frame->rows.empty());
  CHECK(frame->rows.front()->bytes() == "rich record 5");
  CHECK(rows->lastFirst == 5);
}

TEST_CASE("executor backpressure retains one bounded intent and retries after capacity changes") {
  auto provider = std::make_shared<ShortPages>();
  auto source = ui::logSource(provider);
  Fixture fixture(source);

  fixture.executor.accepting = false;
  fixture.settle(4);
  CHECK(provider->reads == 0);
  REQUIRE(source.budget());
  CHECK(source.budget()->usage().work == 1);

  fixture.executor.accepting = true;
  ++fixture.executor.epoch;
  fixture.settle();

  REQUIRE(fixture.frame());
  CHECK(fixture.frame()->rows.size() >= 22);
}

TEST_CASE("terminal executor admission failure releases pending source credit once") {
  auto provider = std::make_shared<ShortPages>();
  auto source = ui::logSource(provider);
  Fixture fixture(source);

  fixture.executor.failure = BackgroundError::UNAVAILABLE;
  fixture.settle(4);

  const auto diagnostics = take(fixture.scene->diagnostics());

  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(diagnostics.errors.front().error.background == BackgroundError::UNAVAILABLE);
  CHECK(source.budget()->usage().work == 0);
  CHECK(source.budget()->usage().pages == 0);
  CHECK(provider->reads == 0);
}

TEST_CASE("small wheel steps cross cache boundaries and two hundred pages retain a bounded cache") {
  auto rows = std::make_shared<RichRows>(5000);
  std::shared_ptr<const ModelSnapshot<std::string>> snapshot = rows;
  auto source = ui::logSource(snapshot);
  Fixture fixture(source);

  fixture.settle(4);
  take(fixture.scene->focus(fixture.node));
  take(setScrollOffset(fixture.app->tree(), fixture.node, ScrollAxis::VERTICAL, {8, 0}));
  fixture.settle(3);
  fixture.input(
    MouseInputEvent{.column = 10, .row = 10, .button = MouseButton::WHEEL_DOWN, .action = MouseAction::SCROLL});
  CHECK(fixture.visibleOrdinal() == 11);
  fixture.input(
    MouseInputEvent{.column = 10, .row = 10, .button = MouseButton::WHEEL_UP, .action = MouseAction::SCROLL});
  CHECK(fixture.visibleOrdinal() == 8);

  take(seekLogRow(fixture.app->tree(), fixture.node, 200));
  fixture.settle(4);

  for (unsigned page = 0; page < 100; ++page) {
    fixture.input(KeyEvent{InputKey::PAGE_DOWN});
    CHECK(fixture.frame()->rows.size() <= 60);
  }

  CHECK(fixture.visibleOrdinal() == 2300);

  for (unsigned page = 0; page < 100; ++page) {
    fixture.input(KeyEvent{InputKey::PAGE_UP});
    CHECK(fixture.frame()->rows.size() <= 60);
  }

  CHECK(fixture.visibleOrdinal() == 200);
  CHECK(source.budget()->usage().bytes <= source.budget()->options().maxBytes);
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}

TEST_CASE("consecutive page keys consume prior native intent before admitting the next source request") {
  for (const auto layoutBetweenKeys : {false, true}) {
    CAPTURE(layoutBetweenKeys);

    auto rows = std::make_shared<RichRows>();

    for (auto &row : rows->rows) {
      row.value.append(100, 'x');
    }

    std::shared_ptr<const ModelSnapshot<std::string>> snapshot = rows;
    Fixture fixture(ui::logSource(snapshot));

    fixture.settle(4);
    take(fixture.scene->focus(fixture.node));
    fixture.settle(3);
    REQUIRE(fixture.frame()->rows.size() == 60);
    fixture.dispatchInput(KeyEvent{InputKey::PAGE_DOWN});

    if (layoutBetweenKeys) {
      fixture.pump();
      take(fixture.app->tree().read(fixture.node, [](const LogView &widget) {
        const auto &state = *detail::LogViewAccess::state(widget).external;

        CHECK(state.offset == 21);
        CHECK(state.navigationChanged);
      }));
    }

    fixture.dispatchInput(KeyEvent{InputKey::PAGE_DOWN});
    REQUIRE(fixture.executor.job);
    fixture.settle(6);

    CHECK(fixture.visibleOrdinal() == 21);
    CHECK(fixture.frame()->before.token[0] == 21);
    CHECK(take(fixture.scene->diagnostics()).errors.empty());
  }
}

TEST_CASE("the application runner loads the default file tail without any input event") {
  class QuietTerminal final : public example::ReplayTerminal {
  public:
    explicit QuietTerminal(Executor &executor) : ReplayTerminal({}, 500), executor_(executor) {
    }

    TerminalRead read(TerminalDeadline deadline, std::stop_token stop) override {
      executor_.runOne();

      return ReplayTerminal::read(deadline, stop);
    }

  private:
    Executor &executor_;
  };

  TemporaryLog file;

  file.write("STARTUP_SENTINEL\n");

  Executor executor;
  QuietTerminal terminal(executor);
  auto app = take(Application::create({.backgroundExecutor = &executor}, terminal.clock()));
  auto scene = take(ManagedScene::create(*app));

  take(ui::mount(*scene, ui::logView(ui::logFile(file.utf8()))));

  const auto result = take(scene->run(terminal));

  CHECK(result.inputs == 0);
  CHECK(executor.submissions > 0);
  CHECK(terminal.output.find("STARTUP_SENTINEL") != std::string::npos);
  CHECK(terminal.closed);
}
