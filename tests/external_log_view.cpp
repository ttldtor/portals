// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../src/widgets/external_log_view.hpp"
#include "../src/widgets/log_view_state.hpp"
#include "support/examples/replay/terminal.hpp"
#include <doctest.h>
#include <portals/ui.hpp>
#include <portals/ui_log_source_model.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
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

class NavigationProvider final : public WindowProvider<LogLine>, public LogBookmarkProvider {
public:
  ModelResource identity = take(ModelResource::create());
  const std::thread::id ui = std::this_thread::get_id();
  std::atomic<std::uint64_t> revision{1};
  mutable std::atomic<std::size_t> resolutions{};
  mutable std::atomic<bool> uiWork{};
  std::atomic<bool> pending{};
  std::atomic<bool> deleteSeven{};
  mutable std::mutex mutex;
  mutable std::weak_ptr<const WindowNotifier<LogLine>> readiness;
  mutable SourceStamp session;
  std::function<void()> beforeResolve;
  std::function<void()> stopped;
  mutable std::unique_ptr<std::stop_callback<std::function<void()>>> retainedStop;

  SourceCapabilities capabilities() const noexcept override {
    return {true, false, true};
  }

  ModelResource resource() const noexcept override {
    return identity;
  }

  std::expected<SourceReadReply<LogLine>, SourceError> load(const SourceReadCommand &command, PageBuilder<LogLine> &out,
                                                            std::stop_token stop) override {
    uiWork = uiWork || std::this_thread::get_id() == ui;

    if (command.dataRevision != revision) {
      return std::unexpected(SourceError::REVISION_CHANGED);
    }

    const auto position = command.anchor.kind == SourceAnchorKind::TAIL     ? 500
                          : command.anchor.kind == SourceAnchorKind::CURSOR ? command.anchor.cursor.token[0]
                                                                            : 0;
    const auto first = command.direction == SourceDirection::BACKWARD
                         ? position - std::min<std::uint64_t>(position, command.maxRows)
                         : position;
    auto next = first;

    while (next < 500 && next - first < command.maxRows) {
      if (stop.stop_requested()) {
        return std::unexpected(SourceError::CANCELLED);
      }

      if (deleteSeven && next == 7) {
        ++next;

        continue;
      }

      const auto text = "generic " + std::to_string(next);
      auto added = out.emplace(RowId{next + 1, 11}, command.dataRevision, next, text, out.resource());

      if (!added) {
        return std::unexpected(added.error());
      }

      ++next;
    }

    return out.ready({command.source, {first, 0}}, {command.source, {next, 0}},
                     next == 500 ? SourceEdge::FINAL_END : SourceEdge::MORE);
  }

  std::expected<ModelBookmark, SourceError> capture(const SourceReadCommand &command, const WindowRow<LogLine> &row,
                                                    BookmarkOptions options, std::stop_token stop) const override {
    uiWork = uiWork || std::this_thread::get_id() == ui;

    if (stop.stop_requested()) {
      return std::unexpected(SourceError::CANCELLED);
    }

    return ModelBookmark::create(identity, {PositionKind::ITEM, row.id, row.id.value - 1, command.dataRevision}, {},
                                 options);
  }

  std::expected<PositionReply, SourceError> resolve(const PositionRequest &request,
                                                    std::stop_token stop) const override {
    ++resolutions;
    uiWork = uiWork || std::this_thread::get_id() == ui;

    if (stop.stop_requested()) {
      return std::unexpected(SourceError::CANCELLED);
    }

    auto position = request.bookmark.position();
    const bool removed = deleteSeven && position.row == RowId{8, 11};

    position.revision = request.revision;

    if (removed) {
      position.row = {9, 11};
      position.offset = 8;
    }

    return PositionReply{identity,
                         request.session,
                         request.revision,
                         request.sequence,
                         pending   ? PositionResolution::PENDING
                         : removed ? PositionResolution::NEAREST
                                   : PositionResolution::EXACT,
                         position};
  }

  std::expected<PositionReply, SourceError> resolveLog(const PositionRequest &request,
                                                       std::weak_ptr<const WindowNotifier<LogLine>> endpoint,
                                                       std::stop_token stop) const override {
    {
      std::lock_guard lock(mutex);

      readiness = std::move(endpoint);
      session = request.session;
    }

    if (stopped) {
      retainedStop = std::make_unique<std::stop_callback<std::function<void()>>>(stop, stopped);
    }

    if (beforeResolve) {
      beforeResolve();
    }

    return resolve(request, stop);
  }

  std::expected<SourceAnchor, SourceError> readAnchor(const PositionReply &reply, SourceStamp source) const override {
    return SourceAnchor{SourceAnchorKind::CURSOR, {source, {reply.position.offset, 0}}, {}};
  }

  std::expected<PublishResult, SourceError> notify(std::uint64_t nextRevision) {
    std::lock_guard lock(mutex);
    auto endpoint = readiness.lock();

    if (!endpoint) {
      return std::unexpected(SourceError::CLOSED);
    }

    revision = nextRevision;

    return endpoint->notifyReady(session, nextRevision);
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

TEST_CASE("model replacement rejects invalid descriptors without changing content or reporting checked errors") {
  auto provider = std::make_shared<ShortPages>();
  Fixture fixture(ui::logSource(provider));

  fixture.settle();

  const auto original = fixture.frame();
  const auto node = fixture.reference.node();
  const auto rejected = fixture.reference.setModel({});

  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().source == SourceError::INVALID_ARGUMENT);
  CHECK(fixture.frame() == original);
  CHECK(fixture.reference.node() == node);
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
  CHECK_FALSE(fixture.reference.model({}));
  CHECK(take(fixture.scene->diagnostics()).errors.size() == 1);
  CHECK(fixture.frame() == original);
}

TEST_CASE("replacement before preparation and A to B to A keep only the last admitted model") {
  TemporaryLog a;
  TemporaryLog b;

  a.write("model A\n");
  b.write("model B\n");

  auto sourceA = ui::logFile(a.utf8());
  auto sourceB = ui::logFile(b.utf8());
  Fixture fixture(sourceA);

  take(fixture.reference.setModel(sourceB));
  take(setLogModel(*fixture.scene, fixture.node, sourceA));
  fixture.settle(6);

  REQUIRE(fixture.frame());
  CHECK(fixture.frame()->rows.front()->bytes() == "model A");
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}

TEST_CASE("paused file reading returns to its record after switching and append") {
  TemporaryLog a;
  TemporaryLog b;
  std::string content;

  for (unsigned index = 0; index < 180; ++index) {
    content += "record " + std::to_string(index) + "\n";
  }

  a.write(content);
  b.write("other resource\n");

  auto sourceA = ui::logFile(a.utf8());
  auto sourceB = ui::logFile(b.utf8());
  Fixture fixture(sourceA);

  fixture.settle();
  const auto position = content.find("record 80\n");

  take(seekLogByte(fixture.app->tree(), fixture.node, position));
  fixture.settle();
  REQUIRE(fixture.frame());
  REQUIRE(fixture.frame()->rows.front()->bytes() == "record 80");

  take(fixture.reference.setModel(sourceB));
  fixture.settle();
  a.write("appended while away\n", true);
  take(fixture.reference.setModel(sourceA));
  fixture.settle();

  REQUIRE(fixture.frame());
  CHECK(fixture.frame()->rows.front()->bytes() == "record 80");
  CHECK_FALSE(fixture.frame()->following);
}

TEST_CASE("generic cursor bookmarks restore without known count or row-seek capability") {
  auto provider = std::make_shared<NavigationProvider>();
  auto source = ui::logSource(provider);
  Fixture fixture(source);

  CHECK_FALSE(source.rowCount());
  CHECK_FALSE(provider->capabilities().seekRow);
  fixture.settle();
  take(setScrollOffset(fixture.app->tree(), fixture.node, ScrollAxis::VERTICAL, {7, 0}));
  fixture.settle();
  take(fixture.reference.setModel(ui::logSource(std::make_shared<ShortPages>())));
  fixture.settle();
  take(fixture.reference.setModel(source));
  fixture.settle();

  REQUIRE(fixture.frame());
  CHECK(fixture.frame()->rows.front()->bytes() == "generic 7");
  CHECK(provider->resolutions == 1);
  CHECK_FALSE(provider->uiWork);
  CHECK(take(logNavigation(fixture.app->tree(), fixture.node)).status()->resolution == PositionResolution::EXACT);
}

TEST_CASE("pending bookmark resolution retries only a newer applied readiness revision") {
  auto provider = std::make_shared<NavigationProvider>();
  auto source = ui::logSource(provider);
  Fixture fixture(source);

  fixture.settle();
  take(setScrollOffset(fixture.app->tree(), fixture.node, ScrollAxis::VERTICAL, {7, 0}));
  fixture.settle();
  take(fixture.reference.setModel(ui::logSource(std::make_shared<ShortPages>())));
  fixture.settle();
  provider->pending = true;
  take(fixture.reference.setModel(source));
  fixture.settle();
  auto navigation = take(logNavigation(fixture.app->tree(), fixture.node));

  CHECK(take(navigation.status()).resolution == PositionResolution::PENDING);
  CHECK(provider->resolutions == 1);
  fixture.settle(20);
  CHECK(provider->resolutions == 1);

  {
    std::lock_guard lock(provider->mutex);
    auto endpoint = provider->readiness.lock();

    REQUIRE(endpoint);
    auto wrong = endpoint->notifyReady({999, 999}, 2);

    REQUIRE_FALSE(wrong);
    CHECK(wrong.error() == SourceError::STALE_SOURCE);
  }

  fixture.settle(4);
  CHECK(provider->resolutions == 1);
  provider->pending = false;
  take(provider->notify(2));
  CHECK(provider->resolutions == 1);
  fixture.settle();

  CHECK(provider->resolutions == 2);
  CHECK(take(navigation.status()).resolution == PositionResolution::EXACT);
  REQUIRE(fixture.frame());
  CHECK(fixture.frame()->rows.front()->bytes() == "generic 7");
  take(fixture.reference.setModel(ui::logSource(std::make_shared<ShortPages>())));
  fixture.settle();

  auto retired = provider->notify(3);

  REQUIRE_FALSE(retired);
  CHECK(retired.error() == SourceError::CLOSED);
}

TEST_CASE("a provider-proven deleted item restores its following neighbor with a typed diagnostic") {
  auto provider = std::make_shared<NavigationProvider>();
  auto source = ui::logSource(provider);
  Fixture fixture(source);

  fixture.settle();
  take(setScrollOffset(fixture.app->tree(), fixture.node, ScrollAxis::VERTICAL, {7, 0}));
  fixture.settle();
  take(fixture.reference.setModel(ui::logSource(std::make_shared<ShortPages>())));
  fixture.settle();
  provider->deleteSeven = true;
  take(fixture.reference.setModel(source));
  fixture.settle();
  const auto navigation = take(logNavigation(fixture.app->tree(), fixture.node));
  const auto status = take(navigation.status());

  CHECK(status.resolution == PositionResolution::NEAREST);
  CHECK(status.reason == LogRestoreReason::DELETED_SEGMENT);
  REQUIRE(fixture.frame());
  CHECK(fixture.frame()->rows.front()->bytes() == "generic 8");
  CHECK_FALSE(fixture.frame()->following);
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}

TEST_CASE("a retained pending-resolution stop callback can supersede its readiness retry") {
  auto provider = std::make_shared<NavigationProvider>();
  auto source = ui::logSource(provider);
  auto other = ui::logSource(std::make_shared<ShortPages>());
  Fixture fixture(source);
  TemporaryLog target;

  target.write("callback target\n");
  auto replacement = ui::logFile(target.utf8());
  std::optional<std::expected<void, ManagedError>> callbackResult;

  fixture.settle();
  take(setScrollOffset(fixture.app->tree(), fixture.node, ScrollAxis::VERTICAL, {7, 0}));
  fixture.settle();
  take(fixture.reference.setModel(other));
  fixture.settle();
  provider->pending = true;
  provider->stopped = [&] {
    callbackResult = fixture.reference.setModel(replacement);
  };
  take(fixture.reference.setModel(source));
  fixture.pump();
  auto retained = std::exchange(fixture.executor.job, {});

  REQUIRE(retained);
  std::expected<void, AsyncError> published;
  std::thread worker([&] {
    retained->run(retained->stopToken());
    published = retained->publish();
  });

  worker.join();
  take(std::move(published));
  fixture.pump();
  CHECK(provider->resolutions == 1);
  provider->pending = false;
  take(provider->notify(2));
  fixture.pump();

  REQUIRE(callbackResult);
  CHECK(*callbackResult);
  retained.reset();
  ++fixture.executor.epoch;
  fixture.settle();

  REQUIRE(fixture.frame());
  CHECK(fixture.frame()->rows.front()->bytes() == "callback target");
  CHECK(provider->resolutions == 1);
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}

TEST_CASE("implicit checkpoint sessions release their metadata after view removal") {
  auto source = ui::logSource(std::make_shared<NavigationProvider>());
  Fixture fixture(source);
  std::weak_ptr<SourceBudget> metadata;
  std::weak_ptr<detail::LogNavigationSession> session;

  for (unsigned iteration = 0; iteration < 12; ++iteration) {
    fixture.settle();
    take(fixture.app->tree().read(fixture.node, [&](const LogView &widget) {
      const auto handle = detail::LogViewAccess::state(widget).external->navigation;
      auto owner = take(ui::detail::ExternalLogAccess::navigation(handle));

      metadata = owner->budget;
      session = owner;
    }));
    take(fixture.scene->remove(fixture.node));
    fixture.settle(2);

    CHECK(session.expired());
    CHECK(metadata.expired());
    fixture.reference = ui::Ref<LogView>{};
    fixture.node = take(ui::mount(*fixture.scene, ui::logView(fixture.reference, source, {.followTail = false})));
  }
}

TEST_CASE("an explicit checkpoint handle restores reading after its view is removed") {
  auto source = ui::logSource(std::make_shared<NavigationProvider>());
  Fixture fixture(source);

  fixture.settle();
  take(setScrollOffset(fixture.app->tree(), fixture.node, ScrollAxis::VERTICAL, {7, 0}));
  fixture.settle();
  auto navigation = take(logNavigation(fixture.app->tree(), fixture.node));

  take(fixture.scene->remove(fixture.node));
  fixture.settle(2);
  CHECK(navigation.status());
  fixture.reference = ui::Ref<LogView>{};
  fixture.node = take(
    ui::mount(*fixture.scene, ui::logView(fixture.reference, source, {.followTail = false, .navigation = navigation})));
  fixture.settle();

  REQUIRE(fixture.frame());
  CHECK(fixture.frame()->rows.front()->bytes() == "generic 7");
}

TEST_CASE("paused and following modes are remembered independently for each file resource") {
  TemporaryLog a;
  TemporaryLog b;
  std::string records;

  for (unsigned index = 0; index < 100; ++index) {
    records += "line " + std::to_string(index) + "\n";
  }

  a.write(records);
  b.write(records);
  auto sourceA = ui::logFile(a.utf8());
  auto sourceB = ui::logFile(b.utf8());
  Fixture fixture(sourceA, true);

  fixture.settle();
  take(seekLogByte(fixture.app->tree(), fixture.node, records.find("line 60\n")));
  fixture.settle();
  take(fixture.reference.setModel(sourceB));
  fixture.settle();
  a.write("new A\n", true);
  b.write("new B\n", true);
  take(fixture.reference.setModel(sourceA));
  fixture.settle();

  REQUIRE(fixture.frame());
  CHECK(fixture.frame()->rows.front()->bytes() == "line 60");
  CHECK_FALSE(fixture.frame()->following);
  take(fixture.reference.setModel(sourceB));
  fixture.settle();

  REQUIRE(fixture.frame());
  CHECK(fixture.frame()->rows.back()->bytes() == "new B");
  CHECK(fixture.frame()->following);
}

TEST_CASE("a paused wrapped Unicode record preserves its grapheme-aligned in-record position") {
  TemporaryLog a;
  TemporaryLog b;
  std::string record;

  for (unsigned index = 0; index < 80; ++index) {
    record += "e\xCC\x81\xE7\x95\x8C";
  }

  a.write("heading\n" + record + "\nlast\n");
  b.write("another source\n");
  auto sourceA = ui::logFile(a.utf8());
  Fixture fixture(sourceA);

  fixture.size = {14, 8};
  fixture.settle();
  take(setScrollOffset(fixture.app->tree(), fixture.node, ScrollAxis::VERTICAL, {5, 0}));
  fixture.settle();
  std::size_t byte{};
  std::string visible;

  take(fixture.app->tree().read(fixture.node, [&](const LogView &widget) {
    const auto &state = *detail::LogViewAccess::state(widget).external;

    REQUIRE(state.frame);
    REQUIRE(state.offset < state.frame->anchors.size());
    byte = state.frame->anchors[state.offset].firstByte;
    visible = state.frame->rows[state.offset]->bytes();
  }));

  REQUIRE(byte > 0);
  auto text = take(GraphemeText::from(record));

  CHECK(std::ranges::any_of(text.graphemes(), [&](const Grapheme &cluster) {
    return cluster.byte_begin == byte;
  }));
  take(fixture.reference.setModel(ui::logFile(b.utf8())));
  fixture.settle();
  take(fixture.reference.setModel(sourceA));
  fixture.settle();
  take(fixture.app->tree().read(fixture.node, [&](const LogView &widget) {
    const auto &state = *detail::LogViewAccess::state(widget).external;

    REQUIRE(state.frame);
    REQUIRE(state.offset < state.frame->anchors.size());
    CHECK(state.frame->anchors[state.offset].firstByte == byte);
    CHECK(state.frame->rows[state.offset]->bytes() == visible);
    CHECK_FALSE(state.followTail);
  }));
}

TEST_CASE("checkpoint capacity rejects a new resource before replacing the current view") {
  auto first = ui::logSource(std::make_shared<NavigationProvider>());
  Fixture fixture(first);
  auto navigation = take(fixture.scene->logNavigation({.maxBytes = 8192}));

  take(fixture.scene->remove(fixture.node));
  fixture.settle(2);
  fixture.reference = ui::Ref<LogView>{};
  fixture.node = take(
    ui::mount(*fixture.scene, ui::logView(fixture.reference, first, {.followTail = false, .navigation = navigation})));
  fixture.settle();
  std::vector<ui::LogSource> sources(0);
  bool rejected{};

  sources.push_back(first);

  for (unsigned index = 0; index < 40; ++index) {
    sources.push_back(ui::logSource(std::make_shared<NavigationProvider>()));
    const auto before = fixture.frame();
    const auto replaced = fixture.reference.setModel(sources.back());

    if (!replaced) {
      CHECK(replaced.error().source == SourceError::WOULD_BLOCK);
      CHECK(fixture.frame() == before);
      CHECK(take(navigation.status()).reason == LogRestoreReason::CHECKPOINT_CAPACITY);
      rejected = true;

      break;
    }

    fixture.settle();
  }

  REQUIRE(rejected);
  CHECK(sources.size() > 3);
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
  take(fixture.reference.setModel(first));
  fixture.settle();
  CHECK(take(navigation.status()).resolution == PositionResolution::EXACT);
}

TEST_CASE("a late restore completion cannot replace a subsequently admitted source") {
  auto provider = std::make_shared<NavigationProvider>();
  auto source = ui::logSource(provider);
  auto other = ui::logSource(std::make_shared<ShortPages>());
  Fixture fixture(source);

  fixture.settle();
  take(setScrollOffset(fixture.app->tree(), fixture.node, ScrollAxis::VERTICAL, {7, 0}));
  fixture.settle();
  take(fixture.reference.setModel(other));
  fixture.settle();
  take(fixture.reference.setModel(source));
  fixture.pump();
  auto old = std::exchange(fixture.executor.job, {});

  REQUIRE(old);
  take(fixture.reference.setModel(other));
  std::expected<void, AsyncError> published;
  std::thread worker([&] {
    // Simulate work whose provider continues after cooperative cancellation.
    old->run({});
    published = old->publish();
  });

  worker.join();
  CHECK((published || published.error() == AsyncError::CLOSED || published.error() == AsyncError::CANCELLED));
  old.reset();
  fixture.settle();

  REQUIRE(fixture.frame());
  CHECK(fixture.frame()->rows.front()->bytes() == "record 0");
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}

TEST_CASE("lost file history preserves a paused checkpoint and reports a recoverable outcome") {
  for (const bool rotate : {false, true}) {
    CAPTURE(rotate);
    TemporaryLog a;
    TemporaryLog b;

    a.write("first\nsecond\nthird\n");
    b.write("another file\n");
    auto source = ui::logFile(a.utf8());
    Fixture fixture(source);

    fixture.settle();
    take(seekLogByte(fixture.app->tree(), fixture.node, 6));
    fixture.settle();
    const auto navigation = take(logNavigation(fixture.app->tree(), fixture.node));
    const auto resource = source.bookmarks()->resource();
    const auto saved = take(navigation.checkpoint(resource));

    REQUIRE(saved);
    REQUIRE(saved->bookmark);
    take(fixture.reference.setModel(ui::logFile(b.utf8())));
    fixture.settle();
    const auto previousPath = a.path.string() + ".old";

    if (rotate) {
      std::filesystem::rename(a.path, previousPath);
    }

    a.write("x\n");
    take(fixture.reference.setModel(source));
    fixture.settle();
    const auto status = take(navigation.status());

    CHECK(status.resolution == (rotate ? PositionResolution::INCOMPATIBLE : PositionResolution::UNAVAILABLE));
    REQUIRE(fixture.frame());
    CHECK(fixture.frame()->rows.empty());
    CHECK_FALSE(fixture.frame()->following);
    const auto retained = take(navigation.checkpoint(resource));

    REQUIRE(retained);
    REQUIRE(retained->bookmark);
    CHECK(retained->bookmark->position().row == saved->bookmark->position().row);
    CHECK_FALSE(retained->following);
    CHECK(take(fixture.scene->diagnostics()).errors.empty());
    std::error_code ignored;

    std::filesystem::remove(previousPath, ignored);
  }
}

TEST_CASE("reset forgets only the saved entry and does not schedule active-view work") {
  auto source = ui::logSource(std::make_shared<NavigationProvider>());
  Fixture fixture(source);

  fixture.settle();
  take(setScrollOffset(fixture.app->tree(), fixture.node, ScrollAxis::VERTICAL, {7, 0}));
  fixture.settle();
  const auto navigation = take(logNavigation(fixture.app->tree(), fixture.node));
  const auto resource = source.bookmarks()->resource();
  const auto before = fixture.frame();
  const auto submissions = fixture.executor.submissions;

  take(fixture.app->tree().consumeFrameRequest());
  take(navigation.reset(resource));
  CHECK_FALSE(take(navigation.checkpoint(resource)));
  CHECK(fixture.frame() == before);
  CHECK(fixture.executor.submissions == submissions);
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);

  fixture.settle(2);
  take(fixture.reference.setModel(ui::logSource(std::make_shared<ShortPages>())));
  fixture.settle();
  REQUIRE(take(navigation.checkpoint(resource)));
  take(navigation.reset(resource));
  CHECK_FALSE(take(navigation.checkpoint(resource)));
  take(fixture.reference.setModel(source));
  fixture.settle();

  REQUIRE(fixture.frame());
  CHECK(fixture.frame()->rows.front()->bytes() == "generic 0");
}

TEST_CASE("rotation between file resolution and first load cannot install another file's row") {
  TemporaryLog a;
  TemporaryLog b;

  a.write("first\nsecond\nthird\n");
  b.write("other\n");
  auto source = ui::logFile(a.utf8());
  Fixture fixture(source);

  fixture.settle();
  take(seekLogByte(fixture.app->tree(), fixture.node, 6));
  fixture.settle();
  const auto navigation = take(logNavigation(fixture.app->tree(), fixture.node));

  take(fixture.reference.setModel(ui::logFile(b.utf8())));
  fixture.settle();
  take(fixture.reference.setModel(source));
  fixture.pump();
  REQUIRE(fixture.executor.job);
  fixture.executor.runOne();
  fixture.pump();
  REQUIRE(fixture.executor.job);
  const auto previousPath = a.path.string() + ".old";

  std::filesystem::rename(a.path, previousPath);
  a.write("wrong\nsecond\nfile\n");
  fixture.settle();

  REQUIRE(fixture.frame());
  CHECK(fixture.frame()->rows.empty());
  CHECK(take(navigation.status()).resolution == PositionResolution::INCOMPATIBLE);
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
  std::error_code ignored;

  std::filesystem::remove(previousPath, ignored);
}

TEST_CASE("a newer applied revision between resolve and first page requires fresh resolution") {
  auto provider = std::make_shared<NavigationProvider>();
  auto source = ui::logSource(provider);
  auto other = ui::logSource(std::make_shared<ShortPages>());
  Fixture fixture(source);

  fixture.settle();
  take(setScrollOffset(fixture.app->tree(), fixture.node, ScrollAxis::VERTICAL, {7, 0}));
  fixture.settle();
  take(fixture.reference.setModel(other));
  fixture.settle();
  take(fixture.reference.setModel(source));
  fixture.pump();
  fixture.executor.runOne();
  fixture.pump();
  REQUIRE(fixture.executor.job);
  take(provider->notify(2));
  fixture.pump();
  fixture.settle();

  CHECK(provider->resolutions == 2);
  REQUIRE(fixture.frame());
  CHECK(fixture.frame()->rows.front()->bytes() == "generic 7");
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}

TEST_CASE("a source stop callback can supersede replacement or remove its receiving view") {
  class StopProvider final : public WindowProvider<LogLine> {
  public:
    ShortPages rows;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{};
    bool released{};
    std::function<void()> stopped;

    SourceCapabilities capabilities() const noexcept override {
      return rows.capabilities();
    }

    std::expected<SourceReadReply<LogLine>, SourceError>
    load(const SourceReadCommand &command, PageBuilder<LogLine> &out, std::stop_token stop) override {
      std::stop_callback callback(stop, [&] {
        stopped();
      });
      std::unique_lock lock(mutex);

      entered = true;
      changed.notify_all();
      changed.wait(lock, [&] {
        return released;
      });
      lock.unlock();

      return rows.load(command, out, {});
    }

    void release() {
      {
        std::lock_guard lock(mutex);

        released = true;
      }

      changed.notify_all();
    }
  };

  for (const bool remove : {false, true}) {
    CAPTURE(remove);
    auto provider = std::make_shared<StopProvider>();
    auto source = ui::logSource(provider);
    Fixture fixture(source);
    TemporaryLog target;

    target.write("callback target\n");
    auto superseding = ui::logFile(target.utf8());
    std::optional<std::expected<void, ManagedError>> callbackResult;

    provider->stopped = [&] {
      callbackResult = remove ? fixture.scene->remove(fixture.node) : fixture.reference.setModel(superseding);
    };
    fixture.pump();
    auto work = std::exchange(fixture.executor.job, {});

    REQUIRE(work);
    std::jthread worker([&] {
      work->run(work->stopToken());
    });

    struct Release {
      std::shared_ptr<StopProvider> provider;

      ~Release() {
        provider->release();
      }
    } release{provider};

    bool entered{};

    {
      std::unique_lock lock(provider->mutex);

      entered = provider->changed.wait_for(lock, std::chrono::seconds(2), [&] {
        return provider->entered;
      });
    }

    REQUIRE(entered);
    const auto replaced = fixture.reference.setModel(ui::logSource(std::make_shared<ShortPages>()));

    REQUIRE(callbackResult);
    CHECK(*callbackResult);
    REQUIRE_FALSE(replaced);
    CHECK(replaced.error().source == (remove ? SourceError::CLOSED : SourceError::CANCELLED));
    provider->release();
    worker.join();
    (void)work->publish();
    work.reset();
    ++fixture.executor.epoch;
    fixture.settle();

    if (!remove) {
      REQUIRE(fixture.frame());
      CHECK(fixture.frame()->rows.front()->bytes() == "callback target");
    }

    CHECK(take(fixture.scene->diagnostics()).errors.empty());
  }
}

TEST_CASE("swapping a blocked load discards late success and error without UI source reads") {
  class BarrierProvider final : public WindowProvider<LogLine> {
  public:
    ShortPages provider;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{};
    bool released{};

    SourceCapabilities capabilities() const noexcept override {
      return provider.capabilities();
    }

    std::expected<SourceReadReply<LogLine>, SourceError> load(const SourceReadCommand &command,
                                                              PageBuilder<LogLine> &out, std::stop_token) override {
      {
        std::unique_lock lock(mutex);

        entered = true;
        changed.notify_all();

        if (!changed.wait_for(lock, std::chrono::seconds(2), [&] {
              return released;
            })) {
          return std::unexpected(SourceError::IO_ERROR);
        }
      }

      // Deliberately ignore cancellation to exercise a provider already inside its I/O call.
      return provider.load(command, out, {});
    }
  };

  for (const auto fail : {false, true}) {
    CAPTURE(fail);

    auto old = std::make_shared<BarrierProvider>();
    auto source = ui::logSource(old);
    Fixture fixture(source);

    old->provider.fail = fail;
    fixture.pump();
    fixture.pump();

    auto work = std::exchange(fixture.executor.job, {});

    REQUIRE(work);

    std::jthread worker([&] {
      work->run(work->stopToken());
    });
    bool entered{};

    {
      std::unique_lock lock(old->mutex);

      entered = old->changed.wait_for(lock, std::chrono::seconds(2), [&] {
        return old->entered;
      });
    }

    REQUIRE(entered);

    auto rows = std::make_shared<RichRows>();
    std::shared_ptr<const ModelSnapshot<std::string>> snapshot = rows;

    take(fixture.reference.setModel(ui::logSource(snapshot)));
    CHECK_FALSE(fixture.frame());

    {
      std::lock_guard lock(old->mutex);

      old->released = true;
    }

    old->changed.notify_all();
    worker.join();
    (void)work->publish();
    work.reset();
    ++fixture.executor.epoch;
    fixture.settle(6);

    REQUIRE(fixture.frame());
    CHECK(fixture.frame()->rows.front()->bytes() == "rich record 0");
    CHECK_FALSE(rows->uiRead.load());
    CHECK_FALSE(old->provider.uiRead.load());
    CHECK(source.budget()->usage().work == 0);
    CHECK(source.budget()->usage().pages == 0);
    CHECK(source.budget()->usage().consumers == 0);
    CHECK(take(fixture.scene->diagnostics()).errors.empty());
  }
}

TEST_CASE("hidden model replacement clears old cursors and resumes the new finite source") {
  TemporaryLog file;

  file.write("old file\n");

  Fixture fixture(ui::logFile(file.utf8()));

  fixture.settle(6);
  take(fixture.app->tree().setVisible(fixture.node, false));
  fixture.pump();

  auto rows = std::make_shared<RichRows>();
  std::shared_ptr<const ModelSnapshot<std::string>> snapshot = rows;

  take(fixture.reference.setModel(ui::logSource(snapshot)));
  CHECK_FALSE(fixture.frame());
  take(seekLogRow(fixture.app->tree(), fixture.node, 100));
  fixture.settle(3);
  CHECK_FALSE(fixture.frame());
  take(fixture.app->tree().setVisible(fixture.node, true));
  fixture.settle(6);

  REQUIRE(fixture.frame());
  CHECK(fixture.frame()->rows.front()->bytes() == "rich record 100");
  CHECK_FALSE(seekLogByte(fixture.app->tree(), fixture.node, 0));
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}

TEST_CASE("repeated swaps retain one activity and release old source credits") {
  auto a = ui::logSource(std::make_shared<ShortPages>());
  auto b = ui::logSource(std::make_shared<ShortPages>());
  Fixture fixture(a);
  std::shared_ptr<ui::detail::SceneActivity> activity;

  take(fixture.app->tree().read(fixture.node, [&](const LogView &widget) {
    activity = detail::LogViewAccess::state(widget).external->activity;
  }));

  for (unsigned index = 0; index < 300; ++index) {
    const auto &source = index % 2 ? a : b;
    const auto &previous = index % 2 ? b : a;

    take(fixture.reference.setModel(source));
    take(fixture.app->tree().read(fixture.node, [&](const LogView &widget) {
      CHECK(detail::LogViewAccess::state(widget).external->activity == activity);
    }));
    CHECK(previous.budget()->usage().consumers == 0);
    CHECK(previous.budget()->usage().pages == 0);
    CHECK(previous.budget()->usage().work == 0);
  }

  fixture.settle();
  REQUIRE(fixture.frame());
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}

TEST_CASE("an admitted open failure belongs to the new binding and permits replacement from an input callback") {
  TemporaryLog file;
  TemporaryLog missing;

  file.write("old binding\n");

  Fixture fixture(ui::logFile(file.utf8()));
  auto rows = std::make_shared<RichRows>();
  std::shared_ptr<const ModelSnapshot<std::string>> snapshot = rows;
  unsigned callbacks{};

  fixture.settle(6);
  take(fixture.scene->onError([&](const ManagedDiagnostic &diagnostic) {
    ++callbacks;
    CHECK(diagnostic.error.logFile.has_value());
    const auto blocked = fixture.reference.setModel(ui::logSource(snapshot));

    REQUIRE_FALSE(blocked);
    CHECK(std::get<TreeError>(blocked.error().cause) == TreeError::WOULD_BLOCK);
  }));
  take(fixture.scene->onKey(KeyEvent{InputKey::F2}, [&] {
    take(fixture.reference.setModel(ui::logSource(snapshot)));
  }));
  take(fixture.reference.setModel(ui::logFile(missing.utf8())));
  CHECK_FALSE(fixture.frame());
  fixture.settle(8);
  CHECK(callbacks == 1);
  CHECK_FALSE(fixture.frame());
  fixture.input(KeyEvent{InputKey::F2});

  REQUIRE(fixture.frame());
  CHECK(fixture.frame()->rows.front()->bytes() == "rich record 0");
  CHECK(callbacks == 1);
}

TEST_CASE("removing a replacement from an input callback cancels the failed binding") {
  TemporaryLog missing;
  Fixture fixture(ui::logSource(std::make_shared<ShortPages>()));
  auto source = ui::logFile(missing.utf8());
  unsigned callbacks{};

  take(fixture.scene->onError([&](const ManagedDiagnostic &) {
    ++callbacks;
    const auto blocked = fixture.scene->remove(fixture.node);

    REQUIRE_FALSE(blocked);
    CHECK(std::get<TreeError>(blocked.error().cause) == TreeError::WOULD_BLOCK);
  }));
  take(fixture.scene->onKey(KeyEvent{InputKey::F2}, [&] {
    take(fixture.scene->remove(fixture.node));
  }));
  take(fixture.reference.setModel(source));
  fixture.settle(6);

  CHECK(callbacks == 1);
  fixture.dispatchInput(KeyEvent{InputKey::F2});
  CHECK(source.budget()->usage().consumers == 0);
  CHECK(source.budget()->usage().work == 0);
  CHECK(source.budget()->usage().pages == 0);
}

TEST_CASE("a rolling log changes to an external source without replacing its node") {
  Fixture fixture(ui::logSource(std::make_shared<ShortPages>()));
  ui::Ref<LogView> rolling;

  take(fixture.scene->remove(fixture.node));

  const auto node = take(ui::mount(*fixture.scene, ui::logView(rolling, "old rolling data", {.followTail = false})));
  std::shared_ptr<const ModelSnapshot<std::string>> snapshot = std::make_shared<RichRows>();

  fixture.node = node;
  take(fixture.app->tree().consumeFrameRequest());
  take(rolling.setModel(ui::logSource(snapshot)));
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::LAYOUT);
  CHECK(rolling.node() == node);
  take(fixture.app->tree().read(node, [](const LogView &widget) {
    const auto &state = detail::LogViewAccess::state(widget);

    REQUIRE(state.external);
    CHECK(state.data->records.empty());
    CHECK(state.data->rows.empty());
  }));
  fixture.settle(8);

  take(fixture.app->tree().read(node, [](const LogView &widget) {
    const auto frame = detail::LogViewAccess::state(widget).external->frame;

    REQUIRE(frame);
    REQUIRE_FALSE(frame->rows.empty());
    CHECK(frame->rows.front()->bytes() == "rich record 0");
  }));
}

TEST_CASE("loaded-window replacement releases old frames and keeps steady source credit usage") {
  auto sources =
    std::array{ui::logSource(std::make_shared<ShortPages>()), ui::logSource(std::make_shared<ShortPages>())};
  Fixture fixture(sources[0]);
  std::array<std::optional<std::size_t>, 2> loadedBytes;

  fixture.settle();
  loadedBytes[0] = sources[0].budget()->usage().bytes;

  for (std::size_t index = 0; index < 20; ++index) {
    const auto selected = (index + 1) % sources.size();
    const auto previous = 1 - selected;
    std::weak_ptr<const detail::ExternalLogFrame> old = fixture.frame();

    take(fixture.reference.setModel(sources[selected]));
    fixture.settle();

    REQUIRE(fixture.frame());
    CHECK(old.expired());
    CHECK(sources[previous].budget()->usage().consumers == 0);
    CHECK(sources[previous].budget()->usage().pages == 0);
    CHECK(sources[previous].budget()->usage().work == 0);

    const auto bytes = sources[selected].budget()->usage().bytes;

    if (!loadedBytes[selected]) {
      loadedBytes[selected] = bytes;
    }

    CHECK(bytes == *loadedBytes[selected]);
  }

  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}

TEST_CASE("model admission resets an unprepared navigation intent to the configured follow-tail default") {
  TemporaryLog first;
  TemporaryLog second;
  std::string records;

  for (unsigned index = 0; index < 100; ++index) {
    records += "line " + std::to_string(index) + "\n";
  }

  first.write(records);
  second.write(records);

  Fixture fixture(ui::logFile(first.utf8()), true);

  fixture.settle(8);
  take(setScrollOffset(fixture.app->tree(), fixture.node, ScrollAxis::VERTICAL, {0, 0}));
  take(fixture.reference.setModel(ui::logFile(second.utf8())));
  fixture.settle(8);

  REQUIRE(fixture.frame());
  CHECK(fixture.frame()->rows.back()->bytes() == "line 99");
  take(fixture.app->tree().read(fixture.node, [](const LogView &widget) {
    CHECK(detail::LogViewAccess::state(widget).external->followTail);
  }));
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}
