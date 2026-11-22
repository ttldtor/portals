// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../src/widgets/document_view_state.hpp"
#include <atomic>
#include <doctest.h>
#include <functional>
#include <portals/render.hpp>
#include <portals/style_pss.hpp>
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>
#include <source_location>
#include <thread>

namespace {
using namespace portals;

template <class T, class E>
T take(std::expected<T, E> value, std::source_location origin = std::source_location::current()) {
  INFO("checked call ", origin.line());
  if constexpr (std::same_as<E, ManagedError>) {
    if (!value) {
      INFO("phase ", static_cast<int>(value.error().phase), " source ",
           value.error().source ? static_cast<int>(*value.error().source) : -1, " cause ",
           std::get_if<TreeError>(&value.error().cause) ? static_cast<int>(std::get<TreeError>(value.error().cause))
                                                        : -1);
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

  std::expected<BackgroundTicket, BackgroundError> submit(std::unique_ptr<BackgroundJob> &input) override {
    if (job) {
      return std::unexpected(BackgroundError::WOULD_BLOCK);
    }
    ++submissions;
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
    std::expected<void, AsyncError> result;
    std::thread worker([&] {
      work->run(work->stopToken());
      result = work->publish();
    });
    worker.join();
    CHECK((result || result.error() == AsyncError::CANCELLED || result.error() == AsyncError::CLOSED));
    ++epoch;
  }
};

class Provider final : public ByteDocumentProvider {
public:
  std::string text = std::string(0, '\0');
  std::uint64_t revision{1};
  std::atomic<std::size_t> reads{};
  std::atomic<std::size_t> bytesRead{};
  bool seekable{true};
  bool enforceSequential{};
  std::uint64_t sequentialOffset{};
  bool utf16{};
  std::vector<std::pair<std::uint64_t, std::string>> patches;
  std::atomic<std::size_t> metadataCalls{};
  std::atomic<bool> uiOperation{};
  bool unknown{};
  bool discoverable{};
  std::atomic<std::size_t> discoveries{};
  bool finalUnknown{};
  bool emptyMore{};
  bool failRead{};
  bool providerWouldBlock{};
  std::shared_ptr<SourceBudget> accountedRoot;
  SourceCredit pinnedCredit;
  SourceCredit optionalCache;
  std::size_t cacheTrims{};
  std::optional<DocumentError> budgetFailure;

  std::size_t pinnedBudgetBytes(const SourceBudget &root) const noexcept override {
    return accountedRoot.get() == &root ? pinnedCredit.usage().bytes : 0;
  }

  bool tryReleaseOptionalStorage() noexcept override {
    if (!optionalCache.usage().bytes) {
      return false;
    }

    optionalCache = {};
    ++cacheTrims;

    return true;
  }

  std::optional<std::uint64_t> syntheticExtent;
  const std::thread::id ui = std::this_thread::get_id();

  ByteDocumentCapabilities capabilities() const noexcept override {
    return {seekable, false};
  }

  std::expected<ByteDocumentMetadata, DocumentError> metadata(std::stop_token) override {
    ++metadataCalls;
    uiOperation = uiOperation || ui == std::this_thread::get_id();
    return ByteDocumentMetadata{revision, unknown           ? std::nullopt
                                          : syntheticExtent ? syntheticExtent
                                                            : std::optional<std::uint64_t>(text.size())};
  }

  std::expected<ByteDocumentMetadata, DocumentError> discoverExtent(std::stop_token stop) override {
    ++discoveries;
    auto result = metadata(stop);

    if (result && discoverable) {
      result->byteExtent = syntheticExtent;
    }

    return result;
  }

  std::expected<ByteReadResult, DocumentError> read(const ByteReadCommand &command, std::span<std::byte> destination,
                                                    std::stop_token) override {
    ++reads;
    if (enforceSequential && command.offset != sequentialOffset) {
      return std::unexpected(DocumentError{SourceError::UNSUPPORTED, {}, {}});
    }
    if (providerWouldBlock) {
      return std::unexpected(DocumentError{SourceError::WOULD_BLOCK, {}, {}});
    }
    if (budgetFailure) {
      return std::unexpected(*budgetFailure);
    }

    if (failRead) {
      return std::unexpected(DocumentError{SourceError::IO_ERROR, std::make_error_code(std::errc::io_error), {}});
    }
    uiOperation = uiOperation || ui == std::this_thread::get_id();
    if (emptyMore) {
      return ByteReadResult{};
    }
    if (command.dataRevision != revision) {
      return std::unexpected(DocumentError{SourceError::REVISION_CHANGED, {}, {}});
    }
    if (syntheticExtent) {
      const auto offset = std::min(command.offset, *syntheticExtent);
      const auto count =
        static_cast<std::size_t>(std::min<std::uint64_t>(destination.size(), *syntheticExtent - offset));
      for (std::size_t byte = 0; byte < count; ++byte) {
        const auto position = offset + byte;
        const auto value = utf16 ? (position % 2 ? 0 : (position % 4 ? '\n' : 'x')) : (position % 2 ? '\n' : 'x');
        destination[byte] = static_cast<std::byte>(value);
        for (const auto &[start, bytes] : patches) {
          if (position >= start && position - start < bytes.size()) {
            destination[byte] = static_cast<std::byte>(bytes[static_cast<std::size_t>(position - start)]);
          }
        }
      }
      bytesRead += count;
      sequentialOffset = command.offset + count;
      return ByteReadResult{count, offset + count == *syntheticExtent};
    }
    const auto offset = std::min<std::uint64_t>(command.offset, text.size());
    const auto count = std::min(destination.size(), text.size() - static_cast<std::size_t>(offset));
    std::memcpy(destination.data(), text.data() + offset, count);
    bytesRead += count;
    sequentialOffset = command.offset + count;
    return ByteReadResult{count, (!unknown || finalUnknown) && offset + count == text.size()};
  }
};

ui::DocumentSource source(std::string text) {
  return ui::document(std::make_shared<MemoryDocumentProvider>(std::make_shared<const std::string>(std::move(text))));
}

struct Fixture {
  Executor executor;
  SchedulerTime now{};
  std::unique_ptr<Application> app =
    take(Application::create({.backgroundExecutor = &executor}, {[](void *context) noexcept {
                                                                   return *static_cast<SchedulerTime *>(context);
                                                                 },
                                                                 &now}));
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));
  ui::Ref<DocumentView> reference;
  Node<DocumentView> node;
  CellSize size{24, 8};

  explicit Fixture(ui::DocumentSource descriptor, DocumentViewConfig config = {}, bool requestFocus = true) {
    node = take(ui::mount(*scene, ui::viewer(reference, std::move(descriptor), config)));
    if (requestFocus) {
      take(scene->focus(node));
    }
    take(app->router().setPointerCaptureCapabilities({true, true, true}));
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

  void settle(unsigned turns = 12) {
    for (unsigned turn = 0; turn < turns; ++turn) {
      pump();
      executor.runOne();
    }
    pump();
  }

  std::shared_ptr<const detail::DocumentPreparedFrame> frame() {
    std::shared_ptr<const detail::DocumentPreparedFrame> result;
    take(app->tree().read(node, [&](const DocumentView &widget) {
      result = detail::DocumentViewAccess::state(widget).frame;
    }));
    return result;
  }

  bool input(InputKey key) {
    return take(scene->input(*app, KeyEvent{.key = key}));
  }
};

class PartSink final : public PaintContext {
public:
  struct Text {
    std::string bytes;
    CellPoint origin;
    CellRect clip;
    CellStyle style;
    std::size_t first{};
    std::size_t count{};
  };

  std::vector<Text> texts;

  std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
    for (const auto &command : commands) {
      if (const auto *text = std::get_if<TextCommand>(&command)) {
        texts.push_back({std::string(text->text.bytes()), text->origin, text->clip, text->style, text->firstGrapheme,
                         text->graphemeCount});
      }
    }
    return {};
  }

  CellStyle part(std::string_view bytes) const {
    const auto found = std::find_if(texts.begin(), texts.end(), [&](const Text &text) {
      return text.bytes == bytes;
    });
    REQUIRE(found != texts.end());
    return found->style;
  }
};

PartSink paint(Fixture &fixture) {
  PartSink sink;
  take(fixture.app->tree().frame([&] {
    take(fixture.app->tree().paint(fixture.node, sink));
  }));
  return sink;
}

std::uint64_t navigationSerial(Fixture &fixture) {
  std::uint64_t serial{};
  take(fixture.app->tree().read(fixture.node, [&](const DocumentView &widget) {
    serial = detail::DocumentViewAccess::state(widget).serial;
  }));
  return serial;
}

void selectDocumentRange(Fixture &fixture, std::uint64_t begin, std::uint64_t end, bool reveal = false) {
  const auto frame = fixture.frame();

  REQUIRE(frame);
  take(detail::setDocumentSearchSelection(*fixture.scene, fixture.node,
                                          detail::DocumentSearchSelection{frame->status.revision, begin, end}, reveal));
}
} // namespace

TEST_CASE("cold adapter construction failure is retained until mounting") {
  const auto descriptor = ui::document(std::unexpected(DocumentError{SourceError::UNSUPPORTED, {}, {}}));

  CHECK_FALSE(descriptor.byteSource());
  CHECK_FALSE(descriptor.budget());

  Executor executor;
  auto app = take(Application::create({.backgroundExecutor = &executor}));
  auto scene = take(ManagedScene::create(*app));
  const auto mounted = ui::mount(*scene, ui::viewer(descriptor));

  REQUIRE_FALSE(mounted);
  CHECK(mounted.error().source == SourceError::UNSUPPORTED);
}

TEST_CASE("cold factory mounts fluent native framing without UI metadata reads") {
  auto provider = std::make_shared<Provider>();
  provider->text = "first\nsecond\nthird";
  auto descriptor = ui::document(provider);
  CHECK(provider->metadataCalls.load() == 0);
  CHECK_FALSE(descriptor.byteSource());
  Executor executor;
  auto app = take(Application::create({.backgroundExecutor = &executor}));
  auto scene = take(ManagedScene::create(*app));
  ui::Ref<DocumentView> ref;
  const auto node = take(ui::mount(
    *scene,
    ui::viewer(ref, descriptor).lineNumbers().wrap(false).encoding(DocumentEncoding::UTF8).framed("rounded", "File")));
  CHECK(node == ref.node());
  CHECK(descriptor.byteSource());
  CHECK(descriptor.byteSource()->budget() == descriptor.budget());
  CHECK(provider->metadataCalls.load() == 0);
  CHECK(provider->reads.load() == 0);
  take(app->tree().read(node, [&](const DocumentView &widget) {
    CHECK(detail::DocumentViewAccess::state(widget).config.lineNumbers);
    CHECK_FALSE(detail::DocumentViewAccess::state(widget).config.wrap);
    const auto *frame = detail::scrollFrameOptions(detail::ScrollWidgetAccess::state(widget));
    REQUIRE(frame);
    CHECK(frame->pattern == FramePattern::ROUNDED);
    CHECK(frame->title == "File");
  }));
}

TEST_CASE("standard managed preparation produces immutable visible rows and then becomes idle") {
  auto provider = std::make_shared<Provider>();
  provider->text = "one\ntwo\nthree";
  Fixture f(ui::document(provider));
  CHECK(provider->reads.load() == 0);
  f.settle();
  const auto frame = f.frame();
  REQUIRE(frame);
  REQUIRE(frame->rows.size() == 3);
  CHECK(frame->rows[0].bytes() == "one");
  CHECK(frame->status.totalLines == 3);
  CHECK_FALSE(provider->uiOperation.load());
  const auto reads = provider->reads.load();
  const auto submitted = f.executor.submissions;
  f.settle();
  CHECK(provider->reads.load() == reads);
  CHECK(f.executor.submissions == submitted);
}

TEST_CASE("incomplete text End preparation continues after dispatch without another layout") {
  auto provider = std::make_shared<Provider>();
  provider->text = std::string(10000, 'x');
  Fixture f(ui::document(provider), {.workBytes = 16});
  REQUIRE(f.reference.end());
  f.pump();
  REQUIRE(f.executor.job);
  const auto submitted = f.executor.submissions;

  for (unsigned turn = 0; turn < 3; ++turn) {
    const auto beforeRender = take(f.app->tree().inspect(f.node));
    const auto bounds = take(CellRect::from({}, f.size));
    // Native buildFrame arranges again after captureLayout/prepareViews.
    take(f.app->tree().frame([&] {
      take(layoutTree(f.app->tree(), f.app->root(), bounds, bounds));
    }));
    const auto rendered = take(f.app->tree().inspect(f.node));

    REQUIRE(rendered.geometryEpoch != beforeRender.geometryEpoch);
    REQUIRE(rendered.geometry.surface == beforeRender.geometry.surface);
    REQUIRE(rendered.geometry.clip == beforeRender.geometry.clip);
    f.executor.runOne();
    take(f.app->scheduler().dispatch());
    take(f.scene->afterDispatch(*f.app));
    REQUIRE(f.executor.job);
  }

  CHECK(f.executor.submissions == submitted + 3);
  CHECK_FALSE(provider->uiOperation.load());
  CHECK_FALSE(f.frame());
}

TEST_CASE("text End continuation defers stale geometry and superseded intent to layout") {
  auto provider = std::make_shared<Provider>();
  provider->text = std::string(10000, 'x');
  Fixture f(ui::document(provider), {.workBytes = 16});
  REQUIRE(f.reference.end());
  f.pump();
  REQUIRE(f.executor.job);
  f.executor.runOne();

  SUBCASE("hidden view") {
    take(f.app->tree().setVisible(f.node, false));
  }
  SUBCASE("terminal resize invalidates the old arrangement") {
    f.size = {32, 10};
    take(f.app->tree().invalidate(f.app->root(), Invalidation::LAYOUT));
  }
  SUBCASE("completed resized arrangement before view preparation") {
    f.size = {32, 10};
    const auto bounds = take(CellRect::from({}, f.size));
    take(f.app->tree().frame([&] {
      take(layoutTree(f.app->tree(), f.app->root(), bounds, bounds));
    }));
  }
  SUBCASE("changed cumulative clip at the same outer size") {
    const auto bounds = take(CellRect::from({}, f.size));
    const auto clip = take(CellRect::from({}, {12, 8}));
    take(f.app->tree().frame([&] {
      take(layoutTree(f.app->tree(), f.app->root(), bounds, clip));
    }));
  }
  SUBCASE("new absolute intent") {
    REQUIRE(f.reference.home());
  }
  SUBCASE("new source") {
    take(f.reference.setModel(source("replacement")));
  }

  take(f.app->scheduler().dispatch());
  take(f.scene->afterDispatch(*f.app));
  CHECK_FALSE(f.executor.job);
  CHECK_FALSE(f.frame());
  CHECK_FALSE(provider->uiOperation.load());
}

TEST_CASE("text End final frame still commits through the layout path") {
  auto provider = std::make_shared<Provider>();
  provider->text = std::string(70, 'x');
  Fixture f(ui::document(provider), {.workBytes = 16});
  REQUIRE(f.reference.end());
  f.pump();
  unsigned turns{};

  while (f.executor.job && turns < 32) {
    ++turns;
    f.executor.runOne();
    take(f.app->scheduler().dispatch());
    take(f.scene->afterDispatch(*f.app));
  }

  REQUIRE(turns > 1);
  REQUIRE(turns < 32);
  CHECK_FALSE(f.frame());
  f.pump();
  REQUIRE(f.frame());
  CHECK(f.frame()->finalEndVisible);
  CHECK_FALSE(provider->uiOperation.load());
}

TEST_CASE("hide cancels demand without releasing running-job credits and show resumes") {
  auto budget = take(SourceBudget::create());
  auto provider = std::make_shared<Provider>();
  provider->text = std::string(10000, 'x');
  Fixture f(ui::document(provider, budget));
  f.pump();
  f.pump();
  REQUIRE(f.executor.job);
  CHECK(budget->usage().work == 1);
  take(f.app->tree().setVisible(f.node, false));
  f.pump();
  CHECK(budget->usage().work == 1);
  f.executor.runOne();
  CHECK(budget->usage().work == 0);
  CHECK_FALSE(f.frame());
  const auto submitted = f.executor.submissions;
  f.settle(3);
  CHECK(f.executor.submissions == submitted);
  take(f.app->tree().setVisible(f.node, true));
  f.settle();
  REQUIRE(f.frame());
}

TEST_CASE("source replacement rejects late completion and preserves bounded source byte bookmarks") {
  const auto first = source("first\nline two\nline three");
  const auto second = source("second source");
  Fixture f(first);
  f.pump();
  f.pump();
  REQUIRE(f.executor.job);
  take(f.reference.setModel(second));
  f.pump();
  f.executor.runOne();
  f.settle();
  REQUIRE(f.frame());
  CHECK(f.frame()->rows[0].bytes() == "second source");
  take(f.reference.setModel(first));
  f.settle();
  take(f.reference.seekByte(6));
  f.settle();
  CHECK(take(f.reference.status()).byteOffset == 6);
  take(f.reference.setModel(second));
  f.settle();
  take(f.reference.setModel(first));
  CHECK(take(f.reference.status()).byteOffset == 6);
  f.settle();
  CHECK(take(f.reference.status()).byteOffset == 6);
  const auto count = f.executor.submissions;
  take(f.reference.setModel(first));
  f.settle(2);
  CHECK(f.executor.submissions == count);
}

TEST_CASE("resize supersedes prepared old-width frames and preserves original byte anchor") {
  Fixture f(source(std::string(300, 'a')));
  f.pump();
  f.pump();
  REQUIRE(f.executor.job);
  f.executor.runOne();
  f.size = {12, 6};
  f.pump();
  f.settle();
  REQUIRE(f.frame());
  CHECK(f.frame()->viewport.width == 10);
  CHECK(take(f.reference.status()).byteOffset == 0);
}

TEST_CASE("navigation is read-only and F10 remains available to the scene") {
  Fixture f(source("one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine"));
  f.settle();
  CHECK_FALSE(f.input(InputKey::F10));
  CHECK(f.input(InputKey::DOWN));
  f.settle();
  CHECK(take(f.reference.status()).byteOffset == 4);
  CHECK(f.input(InputKey::HOME));
  f.settle();
  CHECK(take(f.reference.status()).byteOffset == 0);
  CHECK(f.input(InputKey::END));
  f.settle();
  REQUIRE(f.frame());
  CHECK(f.frame()->rows.back().bytes() == "nine");
}

TEST_CASE("unknown extent waits without polling and explicit refresh observes arriving data") {
  auto provider = std::make_shared<Provider>();
  provider->unknown = true;
  provider->emptyMore = true;
  Fixture f(ui::document(provider));
  f.settle();
  CHECK(take(f.reference.status()).waiting);
  CHECK_FALSE(take(f.reference.status()).finalEnd);
  const auto reads = provider->reads.load();
  f.settle();
  CHECK(provider->reads.load() == reads);
  provider->text = "arrived\n";
  provider->emptyMore = false;
  provider->unknown = false;
  ++provider->revision;
  take(f.reference.refresh());
  f.settle();
  REQUIRE(f.frame());
  CHECK(f.frame()->rows[0].bytes() == "arrived");
  CHECK(f.frame()->status.revision.dataRevision == provider->revision);
}

TEST_CASE("remove closes retained worker delivery without a UI join or stale publication") {
  auto descriptor = source("pending");
  auto budget = descriptor.budget();
  Fixture f(descriptor);
  f.pump();
  f.pump();
  REQUIRE(f.executor.job);
  CHECK(budget->usage().work == 1);
  take(f.scene->remove(f.node));
  CHECK(budget->usage().work == 1);
  f.executor.runOne();
  f.pump();
  CHECK(budget->usage().work == 0);
  CHECK_FALSE(f.reference.status());
}

TEST_CASE("oversized actual viewport reports a typed limit once instead of silent clipping or retry") {
  DocumentViewConfig config;
  config.maxFrameCells = 8;
  Fixture f(source("visible"), config);
  f.settle();
  const auto status = take(f.reference.status());
  REQUIRE(status.error);
  CHECK(status.error->code == SourceError::PAGE_TOO_LARGE);
  CHECK_FALSE(status.preparing);
  const auto count = f.executor.submissions;
  f.settle();
  CHECK(f.executor.submissions == count);
}

TEST_CASE("the first completed layout submits demand without another key or preparation cycle") {
  Fixture f(source("first screen"));
  CHECK_FALSE(f.executor.job);
  f.pump();
  REQUIRE(f.executor.job);
  f.executor.runOne();
  f.pump();
  REQUIRE(f.frame());
  CHECK(f.frame()->rows.front().bytes() == "first screen");
}

TEST_CASE("standard framed string means a pattern for DocumentView too") {
  Executor executor;
  auto app = take(Application::create({.backgroundExecutor = &executor}));
  auto scene = take(ManagedScene::create(*app));
  const auto node = take(ui::mount(*scene, ui::viewer(source("text")).framed("rounded")));
  take(app->tree().read(node, [&](const DocumentView &widget) {
    const auto *frame = detail::scrollFrameOptions(detail::ScrollWidgetAccess::state(widget));
    REQUIRE(frame);
    CHECK(frame->pattern == FramePattern::ROUNDED);
    CHECK(frame->title.empty());
  }));
  CHECK_FALSE(ui::mount(*scene, ui::viewer(source("text")).framed("unknown-pattern")));
}

TEST_CASE("byte-position scrollbar projects the full source and drag preserves offsets above4GiB") {
  auto provider = std::make_shared<Provider>();
  provider->syntheticExtent = (std::uint64_t{5} << 30) + 123;
  Fixture f(ui::document(provider));
  f.settle(3);
  REQUIRE(f.frame());
  CHECK(f.frame()->status.byteExtent == provider->syntheticExtent);
  ScrollTrack track;
  take(f.app->tree().read(f.node, [&](const DocumentView &widget) {
    const auto &scroll = detail::ScrollWidgetAccess::state(widget);
    REQUIRE(scroll.geometry.verticalBar);
    track = scroll.geometry.vertical;
    const auto values = take(scroll.controller->snapshot());
    CHECK(values.vertical.metrics.extent == *provider->syntheticExtent);
    CHECK(values.vertical.metrics.extent > (std::uint64_t{1} << 32));
  }));
  const auto send = [&](MouseAction action, std::int32_t row) {
    InputEvent event = MouseInputEvent{track.origin.x, row, MouseButton::LEFT, action};
    if (!take(f.scene->input(*f.app, event))) {
      take(f.app->router().dispatch(event));
    }
  };
  send(MouseAction::PRESS, track.origin.y);
  CHECK(take(f.app->router().pointerCapture()) == f.node);
  send(MouseAction::MOVE, track.origin.y + track.length + 10);
  send(MouseAction::RELEASE, track.origin.y + track.length + 10);
  f.now += 250;
  f.pump();
  CHECK(take(f.reference.status()).byteOffset == *provider->syntheticExtent);
  CHECK(take(f.reference.status()).byteOffset > (std::uint64_t{1} << 32));
}

TEST_CASE("unknown extent has no invented global scrollbar range") {
  auto provider = std::make_shared<Provider>();
  provider->unknown = true;
  provider->text = std::string(100, 'x');
  Fixture f(ui::document(provider));
  f.settle(3);
  REQUIRE(f.frame());
  CHECK_FALSE(f.frame()->status.byteExtent);
  take(f.app->tree().read(f.node, [&](const DocumentView &widget) {
    CHECK_FALSE(detail::ScrollWidgetAccess::state(widget).geometry.verticalBar);
  }));
}

TEST_CASE("cancelled running work retains credit and the latest resize resumes after capacity changes") {
  auto budget = take(SourceBudget::create({.maxWork = 2}));
  auto descriptor = ui::document(std::make_shared<const std::string>("resumed"), budget);
  Fixture f(descriptor);
  f.pump();
  REQUIRE(f.executor.job);
  auto pressure = take(budget->reserve({4096, 0, 1, 0}));
  f.size = {18, 7};
  f.pump();
  CHECK(take(f.reference.status()).backpressured);
  CHECK(budget->usage().work == 2);
  f.executor.runOne();
  pressure = {};
  f.settle();
  REQUIRE(f.frame());
  CHECK(f.frame()->viewport.width == 16);
  CHECK_FALSE(take(f.reference.status()).backpressured);
  CHECK_FALSE(take(f.reference.status()).error);
}

TEST_CASE("focused document surface uses the palette and honors explicit focused user overrides") {
  class Sink final : public PaintContext {
  public:
    std::optional<CellStyle> surface;

    std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
      for (const auto &command : commands) {
        if (const auto *fill = std::get_if<FillCommand>(&command); fill && fill->area.contains({1, 1})) {
          surface = fill->style;
        }
      }
      return {};
    }
  };

  Fixture f(source("styled"));
  take(f.scene->theme(modernTheme()));
  f.settle(3);
  Sink builtin;
  take(f.app->tree().frame([&] {
    take(f.app->tree().paint(f.node, builtin));
  }));
  REQUIRE(builtin.surface);
  CHECK(builtin.surface->background == RgbColor{0x18, 0x1c, 0x24});
  CHECK(builtin.surface->foreground == RgbColor{0xdc, 0xe1, 0xe8});
  auto custom = modernTheme();
  StyleRule focused;
  focused.selector.type = "DocumentView";
  focused.selector.state = StyleState::FOCUSED;
  focused.declarations.background = RgbColor{12, 34, 56};
  custom.rules.push_back(std::move(focused));
  take(f.scene->theme(std::move(custom)));
  f.settle(2);
  Sink overridden;
  take(f.app->tree().frame([&] {
    take(f.app->tree().paint(f.node, overridden));
  }));
  REQUIRE(overridden.surface);
  CHECK(overridden.surface->background == RgbColor{12, 34, 56});
}

TEST_CASE("collected Ref model failure can retire both its observer proxy and the target") {
  Fixture f(source("old"));
  f.settle(2);
  unsigned callbacks{};
  take(f.scene->onError([&](const ManagedDiagnostic &) {
    ++callbacks;
    take(f.app->tree().remove(f.node));
    f.reference = {};
  }));
  CHECK_FALSE(f.reference.setModel(ui::DocumentSource{}));
  CHECK(callbacks == 0);
  CHECK_FALSE(f.reference.model(ui::DocumentSource{}));
  CHECK(callbacks == 1);
  CHECK_FALSE(f.app->tree().read(f.node, [](const DocumentView &) {
  }));
}

TEST_CASE("worker error delivery permits low-level target removal without later activity access") {
  auto provider = std::make_shared<Provider>();
  provider->text = "unread";
  provider->failRead = true;
  Fixture f(ui::document(provider));
  unsigned callbacks{};
  take(f.scene->onError([&](const ManagedDiagnostic &diagnostic) {
    ++callbacks;
    CHECK(diagnostic.error.source == SourceError::IO_ERROR);
    take(f.app->tree().remove(f.node));
  }));
  f.settle(3);
  CHECK(callbacks == 1);
  CHECK_FALSE(f.app->tree().read(f.node, [](const DocumentView &) {
  }));
}

TEST_CASE("alternating sources update one bookmark per source beyond the history ring capacity") {
  const auto first = source(std::string(1000, 'a'));
  const auto second = source(std::string(1000, 'b'));
  Fixture f(first);

  f.settle(2);

  for (std::uint64_t visit = 1; visit <= 24; ++visit) {
    take(f.reference.seekByte(visit));
    f.settle(2);
    take(f.reference.setModel(second));
    f.settle(2);
    take(f.reference.seekByte(100 + visit));
    f.settle(2);
    take(f.reference.setModel(first));

    CHECK(take(f.reference.status()).byteOffset == visit);

    f.settle(2);
    take(f.reference.setModel(second));

    CHECK(take(f.reference.status()).byteOffset == 100 + visit);

    f.settle(2);
    take(f.reference.setModel(first));
    f.settle(2);
  }
}

TEST_CASE("foreign capacity release after a blocked completion resumes without an executor event") {
  auto budget = take(SourceBudget::create({.maxWork = 2}));
  Fixture f(ui::document(std::make_shared<const std::string>("capacity resumed"), budget));

  f.pump();
  REQUIRE(f.executor.job);
  auto foreign = take(budget->reserve({0, 0, 1, 0}));
  f.executor.runOne();
  f.pump();
  REQUIRE(take(f.reference.status()).backpressured);
  CHECK_FALSE(f.executor.job);
  const auto epoch = f.executor.epoch;
  foreign = {};
  CHECK(f.executor.epoch == epoch);
  f.settle(3);
  REQUIRE(f.frame());
  CHECK(f.frame()->rows.front().bytes() == "capacity resumed");
  CHECK_FALSE(take(f.reference.status()).backpressured);
}

TEST_CASE("provider WOULD_BLOCK is explicit waiting rather than a guessed budget release") {
  auto provider = std::make_shared<Provider>();
  provider->text = "provider resumed";
  provider->providerWouldBlock = true;
  Fixture f(ui::document(provider));

  f.settle(3);
  const auto status = take(f.reference.status());
  CHECK(status.waiting);
  CHECK_FALSE(status.backpressured);
  CHECK_FALSE(status.preparing);
  const auto submissions = f.executor.submissions;
  f.settle(3);
  CHECK(f.executor.submissions == submissions);
  provider->providerWouldBlock = false;
  take(f.reference.refresh());
  f.settle(3);
  REQUIRE(f.frame());
  CHECK(f.frame()->rows.front().bytes() == "provider resumed");
}

TEST_CASE("cold distant seek displays its actual marker with distance-independent byte work") {
  for (const auto anchor : {std::uint64_t{8} << 20, std::uint64_t{512} << 20, std::uint64_t{8} << 30}) {
    CAPTURE(anchor);
    auto provider = std::make_shared<Provider>();
    provider->syntheticExtent = anchor + 4096;
    provider->unknown = anchor == (std::uint64_t{512} << 20);
    provider->patches.emplace_back(anchor, "TARGET-" + std::to_string(anchor) + "\n");
    Fixture f(ui::document(provider), {.lineNumbers = true});
    take(f.reference.seekByte(anchor));
    f.settle(3);
    const auto frame = f.frame();
    REQUIRE(frame);
    REQUIRE_FALSE(frame->rows.empty());
    CHECK(frame->status.byteOffset == anchor);
    CHECK(frame->status.visibleByteOffset == anchor);
    CHECK(frame->rows.front().bytes().starts_with("TARGET-"));
    REQUIRE(frame->gutters);
    CHECK(frame->gutters->front().bytes() == "      ?  ");
    CHECK_FALSE(frame->status.logicalLine);
    CHECK_FALSE(frame->status.totalLines);
    CHECK_FALSE(frame->status.preparing);
    CHECK_FALSE(provider->uiOperation.load());
    // Includes origin, local proof, grapheme precontext and target viewport reads.
    CHECK(provider->bytesRead.load() <= 128 * 1024);
    CHECK(f.executor.submissions <= 3);
  }
}

TEST_CASE("cold local projection keeps a normal combining cluster and original byte affinity") {
  constexpr std::uint64_t START = std::uint64_t{8} << 30;
  auto provider = std::make_shared<Provider>();
  provider->syntheticExtent = START + 4096;
  provider->patches.emplace_back(START, "e\xCC\x81Z\n");
  Fixture f(ui::document(provider));
  take(f.reference.seekByte(START + 2));
  f.settle(3);
  const auto frame = f.frame();
  REQUIRE(frame);
  REQUIRE_FALSE(frame->rows.empty());
  CHECK(frame->rows.front().bytes() == "e\xCC\x81Z");
  CHECK(frame->status.byteOffset == START + 2);
  CHECK(frame->status.visibleByteOffset == START);
  CHECK_FALSE(frame->status.fragmentedCluster);
  CHECK_FALSE(frame->status.logicalLine);
  CHECK(provider->bytesRead.load() <= 128 * 1024);
}

TEST_CASE("cold local UTF16 proof retains BOM choice and complete surrogate pair") {
  constexpr std::uint64_t START = std::uint64_t{8} << 30;
  auto provider = std::make_shared<Provider>();
  provider->syntheticExtent = START + 4096;
  provider->utf16 = true;
  provider->patches.emplace_back(0, std::string("\xFF\xFE", 2));
  provider->patches.emplace_back(START, std::string("\x3D\xD8\x00\xDEZ\x00\n\x00", 8));
  DocumentViewConfig config;
  config.decoder.explicitEncoding = DocumentEncoding::UNRESOLVED;
  config.decoder.hint = DocumentEncoding::WINDOWS1251;
  Fixture f(ui::document(provider), config);
  take(f.reference.seekByte(START + 3));
  f.settle(3);
  const auto frame = f.frame();
  REQUIRE(frame);
  REQUIRE_FALSE(frame->rows.empty());
  CHECK(frame->rows.front().bytes() == "\xF0\x9F\x98\x80Z");
  CHECK(frame->status.byteOffset == START + 3);
  CHECK(frame->status.visibleByteOffset == START);
  CHECK_FALSE(frame->status.logicalLine);
  CHECK(provider->bytesRead.load() <= 128 * 1024);
}

TEST_CASE("cold seek origin is invalidated by revision and late source replacement") {
  constexpr std::uint64_t START = std::uint64_t{512} << 20;
  auto first = std::make_shared<Provider>();
  first->syntheticExtent = START + 4096;
  first->patches.emplace_back(START, "OLD\n");
  Fixture f(ui::document(first));
  take(f.reference.seekByte(START));
  f.settle(3);
  REQUIRE(f.frame());
  REQUIRE_FALSE(f.frame()->rows.empty());
  CHECK(f.frame()->rows.front().bytes() == "OLD");
  ++first->revision;
  first->patches.back().second = "NEW\n";
  take(f.reference.refresh());
  f.settle(3);
  REQUIRE(f.frame());
  CHECK(f.frame()->rows.front().bytes() == "NEW");
  CHECK(f.frame()->status.revision.dataRevision == first->revision);
  take(f.reference.seekByte(START + 2));
  f.pump();
  REQUIRE(f.executor.job);
  take(f.reference.setModel(source("replacement")));
  f.executor.runOne();
  f.settle(4);
  REQUIRE(f.frame());
  REQUIRE_FALSE(f.frame()->rows.empty());
  CHECK(f.frame()->rows.front().bytes() == "replacement");
  CHECK(f.frame()->status.byteOffset == 0);
}

TEST_CASE("forward-only document keeps typed distant-seek rejection without random reads") {
  auto provider = std::make_shared<Provider>();
  provider->seekable = false;
  provider->text = "sequential\nsource";
  Fixture f(ui::document(provider));
  const auto rejected = f.reference.seekByte(std::uint64_t{8} << 30);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().source == SourceError::UNSUPPORTED);
  CHECK(provider->reads.load() == 0);
  f.settle(3);
  REQUIRE(f.frame());
  REQUIRE_FALSE(f.frame()->rows.empty());
  CHECK(f.frame()->rows.front().bytes() == "sequential");
  CHECK_FALSE(provider->uiOperation.load());
}

TEST_CASE("local proof probes honor tiny work turns and short source pages") {
  constexpr std::uint64_t START = std::uint64_t{8} << 30;
  SUBCASE("one-byte turns retain bounded qualification progress") {
    auto provider = std::make_shared<Provider>();
    provider->syntheticExtent = START + 4096;
    DocumentViewConfig config;
    config.workBytes = 1;
    Fixture f(ui::document(provider), config);
    take(f.reference.seekByte(START));
    for (unsigned turn = 0; turn < 20; ++turn) {
      f.pump();
      f.executor.runOne();
      CHECK(provider->bytesRead.load() == turn + 1);
    }
    f.pump();
    CHECK(take(f.reference.status()).preparing);
    CHECK_FALSE(take(f.reference.status()).error);
    CHECK_FALSE(provider->uiOperation.load());
  }
  SUBCASE("short pages complete the actual target frame") {
    auto provider = std::make_shared<Provider>();
    provider->syntheticExtent = START + 4096;
    provider->patches.emplace_back(START, "SHORT-PAGES\n");
    DocumentViewConfig config;
    config.workBytes = 512;
    Fixture f(ui::document(provider, {}, {.maxPageBytes = 3}), config);
    take(f.reference.seekByte(START));
    f.settle(16);
    REQUIRE(f.frame());
    REQUIRE_FALSE(f.frame()->rows.empty());
    CHECK(f.frame()->rows.front().bytes() == "SHORT-PAGES");
    CHECK(f.frame()->status.byteOffset == START);
    CHECK(f.frame()->status.visibleByteOffset == START);
    CHECK(provider->bytesRead.load() <= 8 * 1024);
    CHECK_FALSE(provider->uiOperation.load());
  }
}

TEST_CASE("a leading RI run beyond local precontext exposes unknown segmentation by continuation") {
  constexpr std::uint64_t START = std::uint64_t{8} << 30;
  constexpr std::string_view REGIONAL = "\xF0\x9F\x87\xA6";
  auto provider = std::make_shared<Provider>();
  provider->syntheticExtent = START + 4096;
  std::string run;
  for (unsigned count = 0; count < 2048; ++count) {
    run.append(REGIONAL);
  }
  run.append("\n");
  provider->patches.emplace_back(START - 8192, std::move(run));
  Fixture f(ui::document(provider));
  // This anchor has more than 4KiB of RI context before it. No global pair
  // parity is asserted: a continuation explicitly identifies the projection.
  const auto anchor = START - 4096 + 4;
  take(f.reference.seekByte(anchor));
  f.settle(3);
  REQUIRE(f.frame());
  REQUIRE_FALSE(f.frame()->rows.empty());
  CHECK(f.frame()->status.byteOffset == anchor);
  CHECK(f.frame()->status.fragmentedCluster);
  CHECK(f.frame()->rows.front().bytes().starts_with("\xE2\x97\x8C"));
  CHECK(f.frame()->rows.front().bytes().find(REGIONAL) != std::string_view::npos);
  CHECK_FALSE(f.frame()->status.logicalLine);
  take(f.reference.seekByte(anchor + 32));
  f.settle(3);
  REQUIRE(f.frame());
  REQUIRE_FALSE(f.frame()->rows.empty());
  CHECK(f.frame()->status.byteOffset == anchor + 32);
  CHECK(f.frame()->status.fragmentedCluster);
  CHECK(f.frame()->rows.front().bytes().starts_with("\xE2\x97\x8C"));
  CHECK(provider->bytesRead.load() <= 128 * 1024);
}

TEST_CASE("cold known EOF and beyond retain the actual non-LF tail row") {
  constexpr std::uint64_t EXTENT = std::uint64_t{8} << 30;
  constexpr std::string_view MARKER = "TAIL-MARKER!";
  for (const auto anchor : {EXTENT, EXTENT + 1}) {
    CAPTURE(anchor);
    auto provider = std::make_shared<Provider>();
    provider->syntheticExtent = EXTENT;
    provider->patches.emplace_back(EXTENT - MARKER.size(), std::string(MARKER));
    Fixture f(ui::document(provider));
    take(f.reference.seekByte(anchor));
    f.settle(3);
    REQUIRE(f.frame());
    REQUIRE_FALSE(f.frame()->rows.empty());
    CHECK(f.frame()->rows.back().bytes() == MARKER);
    CHECK(f.frame()->status.byteOffset == anchor);
    CHECK(f.frame()->status.visibleByteOffset == EXTENT - MARKER.size());
    CHECK(f.frame()->status.anchorBeyondExtent == (anchor > EXTENT));
    CHECK(f.frame()->status.finalEnd);
    CHECK_FALSE(f.frame()->status.logicalLine);
    CHECK_FALSE(f.frame()->status.totalLines);
    CHECK(provider->bytesRead.load() <= 128 * 1024);
    CHECK_FALSE(provider->uiOperation.load());
  }
}

TEST_CASE("cold known final LF shows content without inventing an empty display row") {
  constexpr std::uint64_t EXTENT = std::uint64_t{8} << 30;
  constexpr std::string_view TAIL = "TAIL-MARKER!\n";
  for (const auto anchor : {EXTENT, EXTENT + 1}) {
    CAPTURE(anchor);
    auto provider = std::make_shared<Provider>();
    provider->syntheticExtent = EXTENT;
    provider->patches.emplace_back(EXTENT - TAIL.size(), std::string(TAIL));
    Fixture f(ui::document(provider));
    take(f.reference.seekByte(anchor));
    f.settle(3);
    REQUIRE(f.frame());
    REQUIRE(f.frame()->rows.size() == 1);
    CHECK(f.frame()->rows.front().bytes() == "xTAIL-MARKER!");
    CHECK(f.frame()->status.byteOffset == anchor);
    CHECK(f.frame()->status.visibleByteOffset < EXTENT);
    CHECK(f.frame()->status.anchorBeyondExtent == (anchor > EXTENT));
    CHECK(f.frame()->status.finalEnd);
    CHECK(f.frame()->finalEndVisible);
    CHECK(f.frame()->footer->bytes().find("100.0%") != std::string_view::npos);
    CHECK(provider->bytesRead.load() <= 128 * 1024);
  }
}

TEST_CASE("known empty source keeps an empty final line without a fabricated decoder origin") {
  for (const std::uint64_t anchor : {0, 1}) {
    auto provider = std::make_shared<Provider>();
    Fixture f(ui::document(provider));
    take(f.reference.seekByte(anchor));
    f.settle(3);
    REQUIRE(f.frame());
    REQUIRE(f.frame()->rows.size() == 1);
    CHECK(f.frame()->rows.front().bytes().empty());
    CHECK(f.frame()->status.byteOffset == anchor);
    CHECK(f.frame()->status.visibleByteOffset == 0);
    CHECK(f.frame()->status.anchorBeyondExtent == (anchor > 0));
    CHECK(f.frame()->status.finalEnd);
    CHECK(f.frame()->finalEndVisible);
    CHECK(f.frame()->footer->bytes().find("100.0%") != std::string_view::npos);
    CHECK(f.frame()->status.logicalLine == 1);
    CHECK(f.frame()->status.totalLines == 1);
  }
}

TEST_CASE("document body gutter and position paint actual role states without source work") {
  auto provider = std::make_shared<Provider>();
  provider->text = "body\nsecond";
  Fixture f(ui::document(provider), {.lineNumbers = true});
  take(f.scene->theme(take(parsePss("DocumentView{foreground:#101112;role-body-foreground:#212223;"
                                    "role-gutter-foreground:#313233;role-position-foreground:#414243;}"
                                    "DocumentView:focused{role-gutter-foreground:#515253;}"
                                    "DocumentView:disabled{role-body-foreground:#616263;}"))));
  f.settle(3);
  const auto frame = f.frame();
  REQUIRE(frame);
  REQUIRE(frame->footer);
  const auto reads = provider->reads.load();
  const auto submissions = f.executor.submissions;
  const auto serial = navigationSerial(f);
  auto sink = paint(f);
  CHECK(sink.part("body").foreground == RgbColor{0x21, 0x22, 0x23});
  CHECK(sink.part("      1  ").foreground == RgbColor{0x51, 0x52, 0x53});
  CHECK(sink.part(frame->footer->bytes()).foreground == RgbColor{0x41, 0x42, 0x43});

  const auto next = take(parsePss("DocumentView{foreground:#101112;role-body-foreground:#717273;"
                                  "role-gutter-foreground:#818283;role-position-foreground:#919293;}"));
  take(f.app->tree().consumeFrameRequest());
  take(f.scene->theme(next));
  CHECK(take(f.app->tree().consumeFrameRequest()) == Invalidation::PAINT);
  sink = paint(f);
  CHECK(sink.part("body").foreground == RgbColor{0x71, 0x72, 0x73});
  CHECK(sink.part("      1  ").foreground == RgbColor{0x81, 0x82, 0x83});
  CHECK(sink.part(frame->footer->bytes()).foreground == RgbColor{0x91, 0x92, 0x93});
  take(f.scene->theme(next));
  CHECK(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  CHECK(f.frame() == frame);
  CHECK(navigationSerial(f) == serial);
  CHECK(provider->reads.load() == reads);
  CHECK(f.executor.submissions == submissions);
  CHECK_FALSE(provider->uiOperation.load());

  take(f.scene->theme(take(parsePss("DocumentView{role-body-foreground:#212223;}"
                                    "DocumentView:disabled{role-body-foreground:#616263;}"))));
  take(f.app->tree().setEnabled(f.node, false));
  f.pump();
  CHECK(paint(f).part("body").foreground == RgbColor{0x61, 0x62, 0x63});
  CHECK(f.frame() == frame);
  CHECK(navigationSerial(f) == serial);
  CHECK(provider->reads.load() == reads);
  CHECK(f.executor.submissions == submissions);
}

TEST_CASE("typed live theme inheritance and masked body roles repaint without navigation") {
  namespace sc = portals::style::ct;
  constexpr auto LIVE = sc::sheet(sc::parameters(sc::parameter<"color">(sc::rgb(12, 23, 34))),
                                  sc::rule(sc::selector(), sc::foreground(sc::token<"color">())));
  Fixture f(source("body"), {.lineNumbers = true});
  take(f.scene->enableStyleLayers());
  auto live = take(sc::live(*f.scene, LIVE));
  const auto override =
    take(f.scene->addLayer(StyleTier::USER, "parts", take(parsePss("DocumentView{role-gutter-foreground:#AABBCC;}"))));
  f.settle(3);
  const auto frame = f.frame();
  REQUIRE(frame);
  REQUIRE(frame->footer);
  const auto submissions = f.executor.submissions;
  const auto serial = navigationSerial(f);
  CHECK(paint(f).part("body").foreground == RgbColor{12, 23, 34});
  CHECK(take(live.set<"color">(RgbColor{45, 56, 67})));
  auto sink = paint(f);
  CHECK(sink.part("body").foreground == RgbColor{45, 56, 67});
  CHECK(sink.part("      1  ").foreground == RgbColor{0xAA, 0xBB, 0xCC});
  CHECK(sink.part(frame->footer->bytes()).foreground == RgbColor{45, 56, 67});
  take(f.scene->replaceLayer(override,
                             take(parsePss("DocumentView{foreground:#DEADBE;role-body-foreground:#010203;"
                                           "role-gutter-foreground:#AABBCC;role-position-foreground:#040506;}"))));
  CHECK(take(live.set<"color">(RgbColor{78, 89, 90})));
  CHECK(paint(f).part("body").foreground == RgbColor{1, 2, 3});
  take(f.app->tree().consumeFrameRequest());
  take(f.scene->replaceLayer(override,
                             take(parsePss("DocumentView{foreground:#DEADBE;role-body-foreground:#070809;"
                                           "role-gutter-foreground:#AABBCC;role-position-foreground:#040506;}"))));
  CHECK(take(f.app->tree().consumeFrameRequest()) == Invalidation::PAINT);
  CHECK(paint(f).part("body").foreground == RgbColor{7, 8, 9});
  CHECK(f.frame() == frame);
  CHECK(navigationSerial(f) == serial);
  CHECK(f.executor.submissions == submissions);
}

TEST_CASE("narrow document gutters clip prepared parts and preserve byte anchors") {
  Fixture f(source("body\nsecond"), {.lineNumbers = true});
  f.size = {3, 4};
  f.settle(3);
  const auto frame = f.frame();
  REQUIRE(frame);
  REQUIRE(frame->gutters);
  REQUIRE_FALSE(frame->anchors.empty());
  CHECK(frame->anchors.front() == 0);
  CHECK(take(f.reference.status()).byteOffset == 0);
  const auto sink = paint(f);
  const auto gutter = std::find_if(sink.texts.begin(), sink.texts.end(), [](const PartSink::Text &text) {
    return text.bytes == "      1  ";
  });
  REQUIRE(gutter != sink.texts.end());
  CHECK(gutter->clip.size().width == 1);
  CHECK(std::none_of(sink.texts.begin(), sink.texts.end(), [](const PartSink::Text &text) {
    return text.bytes == "b" || text.bytes == "body";
  }));
}

TEST_CASE("unflushed role admission reports once and permits target retirement in the collector") {
  Fixture f(source("unread"));
  unsigned callbacks{};
  take(f.scene->onError([&](const ManagedDiagnostic &diagnostic) {
    ++callbacks;
    CHECK(diagnostic.error.phase == ManagedPhase::STYLE);
    CHECK(std::get<TreeError>(diagnostic.error.cause) == TreeError::WOULD_BLOCK);
    take(f.app->tree().remove(f.node));
  }));
  const auto bounds = take(CellRect::from({}, f.size));
  std::optional<LayoutSnapshot> layout;
  take(f.app->tree().frame([&] {
    take(layoutTree(f.app->tree(), f.app->root(), bounds, bounds));
    layout.emplace(take(LayoutSnapshot::capture(f.app->tree(), f.app->root(), LayoutSnapshotProfile::GEOMETRY)));
  }));
  take(f.scene->prepareViews(*f.app, *layout));
  CHECK(callbacks == 1);
  CHECK(f.executor.submissions == 0);
  CHECK_FALSE(f.app->tree().read(f.node, [](const DocumentView &) {
  }));
}

TEST_CASE("document paint rejects sink removal and preserves every prepared role part") {
  Fixture f(source("body\nsecond"), {.lineNumbers = true});
  take(f.scene->theme(take(parsePss("DocumentView{role-body-foreground:#010203;role-gutter-foreground:#040506;"
                                    "role-position-foreground:#070809;}"))));
  f.settle(3);
  const auto footer = [&] {
    const auto frame = f.frame();
    REQUIRE(frame);
    REQUIRE(frame->footer);
    return std::string(frame->footer->bytes());
  }();

  class MutationSink final : public PaintContext {
  public:
    Fixture &fixture;
    PartSink captured;
    std::optional<TreeError> removalError;

    explicit MutationSink(Fixture &owner) : fixture(owner) {
    }

    std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
      take(captured.submit(commands));
      for (const auto &command : commands) {
        if (const auto *text = std::get_if<TextCommand>(&command);
            text && text->text.bytes() == "body" && !removalError) {
          const auto removed = fixture.app->tree().remove(fixture.node);
          REQUIRE_FALSE(removed);
          removalError = removed.error();
        }
      }
      return {};
    }
  } sink(f);

  take(f.app->tree().frame([&] {
    take(f.app->tree().paint(f.node, sink));
  }));
  CHECK(sink.removalError == TreeError::WOULD_BLOCK);
  CHECK(sink.captured.part("body").foreground == RgbColor{1, 2, 3});
  CHECK(sink.captured.part("      1  ").foreground == RgbColor{4, 5, 6});
  CHECK(sink.captured.part(footer).foreground == RgbColor{7, 8, 9});
  CHECK(f.app->tree().read(f.node, [](const DocumentView &) {
  }));
}

TEST_CASE("PRTLS-359 two wheel events before completion preserve six accepted rows") {
  std::string rows;
  for (unsigned row = 0; row < 40; ++row) {
    rows += "row-" + std::to_string(row) + "\n";
  }
  auto provider = std::make_shared<Provider>();
  provider->text = rows;
  Fixture f(ui::document(provider));
  take(f.app->tree().edit(f.node, [](DocumentView &widget) {
    detail::ScrollWidgetAccess::state(widget).area.wheelStep = 3;
  }));
  f.settle(3);
  REQUIRE(f.frame());
  REQUIRE(f.frame()->viewport.height == 6);
  REQUIRE(f.frame()->rows.front().bytes() == "row-0");
  REQUIRE_FALSE(f.executor.job);
  const auto before = f.executor.submissions;
  const InputEvent wheel = MouseInputEvent{
    .column = 2, .row = 2, .button = MouseButton::WHEEL_DOWN, .action = MouseAction::SCROLL, .wheelDelta = 1.0};
  const auto inspected = take(f.app->tree().inspect(f.node));
  REQUIRE(inspected.geometry.clip.contains({2, 2}));
  unsigned accepted{};
  if (take(f.scene->input(*f.app, wheel))) {
    ++accepted;
  }
  f.pump();
  REQUIRE(f.executor.job);
  if (take(f.scene->input(*f.app, wheel))) {
    ++accepted;
  }
  REQUIRE(accepted == 2);
  f.pump();
  REQUIRE(f.executor.job);
  f.executor.runOne();
  f.settle(5);
  REQUIRE(f.frame());
  const auto actual = std::string(f.frame()->rows.front().bytes());
  const auto status = take(f.reference.status());
  const auto sink = paint(f);
  const auto displayed = std::any_of(sink.texts.begin(), sink.texts.end(), [](const PartSink::Text &text) {
    return text.bytes == "row-6" && text.origin == CellPoint{1, 1};
  });
  std::printf("PRTLS-359 accepted=%u wheelDelta=1 wheelStep=3 viewportHeight=6 expectedMarker=row-6 actualMarker=%s "
              "byteOffset=%llu submissions=%zu uiProviderIO=%d\n",
              accepted, actual.c_str(), static_cast<unsigned long long>(status.byteOffset),
              f.executor.submissions - before, static_cast<int>(provider->uiOperation.load()));
  CHECK(actual == "row-6");
  CHECK(status.byteOffset == 36);
  CHECK(displayed);
  CHECK_FALSE(provider->uiOperation.load());
}

TEST_CASE("PRTLS-359 Viewer default wheel moves one screen row") {
  Fixture f(source("row-0\nrow-1\nrow-2\nrow-3\nrow-4\nrow-5\nrow-6\nrow-7\nrow-8\nrow-9"));
  f.settle(3);
  double wheelStep{};
  take(f.app->tree().read(f.node, [&](const DocumentView &widget) {
    wheelStep = detail::ScrollWidgetAccess::state(widget).area.wheelStep;
  }));
  const InputEvent wheel = MouseInputEvent{
    .column = 2, .row = 2, .button = MouseButton::WHEEL_DOWN, .action = MouseAction::SCROLL, .wheelDelta = 1.0};
  REQUIRE(take(f.scene->input(*f.app, wheel)));
  f.settle(3);
  REQUIRE(f.frame());
  const auto actual = std::string(f.frame()->rows.front().bytes());
  std::printf("PRTLS-359 ViewerDefault expectedStep=1 actualStep=%.1f expectedMarker=row-1 actualMarker=%s\n",
              wheelStep, actual.c_str());
  CHECK(ScrollAreaOptions{}.wheelStep == 3);
  CHECK(wheelStep == 1);
  CHECK(actual == "row-1");
}

namespace {
std::string scrollRows(unsigned count, std::string_view prefix = "row-") {
  std::string rows;
  for (unsigned row = 0; row < count; ++row) {
    if (row) {
      rows += "\n";
    }
    rows += std::string(prefix) + std::to_string(row);
  }
  return rows;
}

void wheel(Fixture &fixture, bool up = false, double magnitude = 1) {
  const InputEvent event = MouseInputEvent{.column = 2,
                                           .row = 1,
                                           .button = up ? MouseButton::WHEEL_UP : MouseButton::WHEEL_DOWN,
                                           .action = MouseAction::SCROLL,
                                           .wheelDelta = magnitude};
  REQUIRE(take(fixture.scene->input(*fixture.app, event)));
}

void checkTop(Fixture &fixture, std::string_view marker) {
  const auto frame = fixture.frame();
  REQUIRE(frame);
  REQUIRE_FALSE(frame->rows.empty());
  CHECK(frame->rows.front().bytes() == marker);
  CHECK_FALSE(take(fixture.reference.status()).error);
  const auto sink = paint(fixture);
  CHECK(std::any_of(sink.texts.begin(), sink.texts.end(), [&](const PartSink::Text &text) {
    return text.origin == CellPoint{1, 1} && text.bytes == marker;
  }));
}
} // namespace

TEST_CASE("pending wheel reversals and negative distance beyond one viewport are retained") {
  const auto rows = scrollRows(50);
  Fixture f(source(rows));
  f.settle(3);
  take(f.reference.seekByte(rows.find("row-20\n")));
  f.settle(3);
  checkTop(f, "row-20");

  for (unsigned event = 0; event < 4; ++event) {
    wheel(f, true, 3);
  }
  f.settle(8);
  checkTop(f, "row-8");

  const auto submissions = f.executor.submissions;
  wheel(f, true, 3);
  wheel(f, false, 3);
  f.settle(2);
  checkTop(f, "row-8");
  CHECK(f.executor.submissions == submissions);

  wheel(f, false, 3);
  f.pump();
  const auto job = f.executor.job;
  REQUIRE(job);
  wheel(f, true, 3);
  f.pump();
  CHECK_FALSE(job->stopToken().stop_requested());
  f.executor.runOne();
  f.settle(5);
  checkTop(f, "row-8");
}

TEST_CASE("continuing relative input cannot starve the current viewer batch") {
  auto provider = std::make_shared<Provider>();
  provider->text = scrollRows(80);
  Fixture f(ui::document(provider));
  f.settle(3);
  wheel(f);
  f.pump();
  unsigned accepted = 1;
  std::uint64_t previousAnchor{};

  for (unsigned turn = 0; turn < 3; ++turn) {
    const auto job = f.executor.job;
    REQUIRE(job);
    for (unsigned event = 0; event < 3; ++event) {
      wheel(f);
      ++accepted;
      f.pump();
      CHECK_FALSE(job->stopToken().stop_requested());
    }
    f.executor.runOne();
    f.pump();
    const auto anchor = take(f.reference.status()).byteOffset;
    CHECK(anchor > previousAnchor);
    previousAnchor = anchor;
  }

  f.settle(6);
  CHECK(accepted == 10);
  checkTop(f, "row-10");
  const auto submissions = f.executor.submissions;
  const auto reads = provider->reads.load();
  f.settle(3);
  CHECK(f.executor.submissions == submissions);
  CHECK(provider->reads.load() == reads);
  CHECK(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  CHECK_FALSE(provider->uiOperation.load());
}

TEST_CASE("absolute viewer commands replace old queued rows and rebase subsequent input") {
  const auto rows = scrollRows(40);
  Fixture f(source(rows));
  f.settle(3);
  take(f.reference.seekByte(rows.find("row-20\n")));
  f.settle(3);
  wheel(f, false, 5);
  f.pump();
  REQUIRE(f.executor.job);
  wheel(f, false, 3);

  std::string expected;
  SUBCASE("Home then relative input") {
    REQUIRE(f.input(InputKey::HOME));
    wheel(f);
    expected = "row-1";
  }
  SUBCASE("absolute byte seek then relative input") {
    take(f.reference.seekByte(rows.find("row-5\n")));
    wheel(f);
    expected = "row-6";
  }
  SUBCASE("End then relative input") {
    REQUIRE(f.input(InputKey::END));
    wheel(f, true);
    expected = "row-33";
  }
  SUBCASE("source replacement then relative input") {
    take(f.reference.setModel(source(scrollRows(20, "new-"))));
    wheel(f);
    expected = "new-1";
  }

  f.pump();
  f.executor.runOne();
  f.settle(8);
  checkTop(f, expected);
}

TEST_CASE("long viewer overshoot reaches the real partial EOF row without blank error or busy loop") {
  const auto rows = scrollRows(230);
  Fixture f(source(rows));
  unsigned height{};
  SUBCASE("ordinary six-row viewport") {
    height = 6;
  }
  SUBCASE("one-row viewport") {
    height = 1;
    f.size.height = 3;
  }
  f.settle(3);
  REQUIRE(f.frame());
  REQUIRE(f.frame()->viewport.height == static_cast<std::int32_t>(height));
  REQUIRE_FALSE(f.frame()->status.finalEnd);
  wheel(f, false, 1000);
  f.settle(260);
  checkTop(f, "row-" + std::to_string(230 - height));
  CHECK(take(f.reference.status()).byteOffset == rows.find("row-" + std::to_string(230 - height)));
  CHECK(take(f.reference.status()).finalEnd);
  CHECK_FALSE(take(f.reference.status()).preparing);

  wheel(f, false, 1000);
  f.settle(2);
  checkTop(f, "row-" + std::to_string(230 - height));
  const auto submissions = f.executor.submissions;
  wheel(f, true, 1000);
  f.settle(260);
  checkTop(f, "row-0");
  wheel(f, true, 1000);
  f.settle(2);
  checkTop(f, "row-0");
  CHECK(f.executor.submissions > submissions);
  const auto idle = f.executor.submissions;
  f.settle(2);
  CHECK(f.executor.submissions == idle);
  CHECK(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE);
}

TEST_CASE("resize restores an interrupted viewer batch without duplicating its accepted rows") {
  Fixture f(source(scrollRows(50)));
  f.settle(3);
  wheel(f, false, 3);
  f.pump();
  const auto job = f.executor.job;
  REQUIRE(job);
  wheel(f, false, 4);
  f.size = {18, 6};
  f.pump();
  CHECK(job->stopToken().stop_requested());
  f.executor.runOne();
  f.settle(10);
  checkTop(f, "row-7");
  REQUIRE(f.frame());
  CHECK(f.frame()->viewport == CellSize{16, 4});
}

TEST_CASE("explicit relative input retries a stopped provider without idle polling") {
  bool ioError{};
  SUBCASE("provider WOULD_BLOCK") {
  }
  SUBCASE("provider IO_ERROR") {
    ioError = true;
  }

  auto provider = std::make_shared<Provider>();
  provider->text = scrollRows(20);
  provider->providerWouldBlock = !ioError;
  provider->failRead = ioError;
  Fixture f(ui::document(provider));
  f.settle(3);
  if (ioError) {
    REQUIRE(take(f.reference.status()).error);
  } else {
    REQUIRE(take(f.reference.status()).waiting);
  }
  const auto reads = provider->reads.load();
  f.settle(2);
  CHECK(provider->reads.load() == reads);

  provider->providerWouldBlock = false;
  provider->failRead = false;
  wheel(f);
  f.settle(5);
  checkTop(f, "row-1");
  CHECK_FALSE(take(f.reference.status()).waiting);
  CHECK_FALSE(provider->uiOperation.load());
}

TEST_CASE("overflowed configured key distance leaves accepted viewer intent unchanged") {
  Fixture f(source(scrollRows(30)));
  f.settle(3);
  const auto submissions = f.executor.submissions;

  SUBCASE("pending counter overflow") {
    take(f.app->tree().edit(f.node, [](DocumentView &widget) {
      detail::ScrollWidgetAccess::state(widget).area.keyStep = std::numeric_limits<std::int64_t>::max();
    }));
    REQUIRE(f.input(InputKey::DOWN));
    const auto overflow = f.scene->input(*f.app, KeyEvent{.key = InputKey::DOWN});
    REQUIRE_FALSE(overflow);
    CHECK(overflow.error() == TreeError::INVALID_ARGUMENT);
    REQUIRE(f.input(InputKey::UP));
    f.settle(2);
    checkTop(f, "row-0");
    CHECK(f.executor.submissions == submissions);
  }
  SUBCASE("in-flight plus pending overflow") {
    REQUIRE(f.input(InputKey::DOWN));
    f.pump();
    const auto job = f.executor.job;
    REQUIRE(job);
    take(f.app->tree().edit(f.node, [](DocumentView &widget) {
      detail::ScrollWidgetAccess::state(widget).area.keyStep = std::numeric_limits<std::int64_t>::max();
    }));
    const auto overflow = f.scene->input(*f.app, KeyEvent{.key = InputKey::DOWN});
    REQUIRE_FALSE(overflow);
    CHECK(overflow.error() == TreeError::INVALID_ARGUMENT);
    CHECK_FALSE(job->stopToken().stop_requested());
    f.executor.runOne();
    f.settle(3);
    checkTop(f, "row-1");
    CHECK(f.executor.submissions == submissions + 1);
  }
}

TEST_CASE("waiting relative viewer retry does not resume a scan with another row delta") {
  auto provider = std::make_shared<Provider>();
  provider->text = scrollRows(17);
  Fixture f(ui::document(provider));
  f.settle(3);
  checkTop(f, "row-0");
  REQUIRE(f.frame()->viewport.height == 6);
  REQUIRE_FALSE(f.frame()->status.finalEnd);

  provider->providerWouldBlock = true;
  wheel(f, false, 3);
  f.pump();
  REQUIRE(f.executor.job);
  f.executor.runOne();
  f.pump();
  REQUIRE(take(f.reference.status()).waiting);
  std::int64_t waitingDelta{};
  take(f.app->tree().read(f.node, [&](const DocumentView &widget) {
    waitingDelta = detail::DocumentViewAccess::state(widget).rowDelta;
  }));
  REQUIRE(waitingDelta == 3);

  provider->providerWouldBlock = false;
  wheel(f);
  f.settle(6);
  REQUIRE(f.frame());
  REQUIRE_FALSE(f.frame()->rows.empty());
  const auto actual = std::string(f.frame()->rows.front().bytes());
  const auto status = take(f.reference.status());
  std::printf(
    "PRTLS-359 F1 waitingDelta=%lld retryDelta=1 expectedTop=row-4 actualTop=%s byteOffset=%llu finalEnd=%d\n",
    static_cast<long long>(waitingDelta), actual.c_str(), static_cast<unsigned long long>(status.byteOffset),
    static_cast<int>(status.finalEnd));
  checkTop(f, "row-4");
  CHECK(status.byteOffset == provider->text.find("row-4\n"));
  CHECK_FALSE(status.waiting);
  CHECK_FALSE(provider->uiOperation.load());
}

TEST_CASE("proven viewer boundaries cannot accrue impossible wheel debt") {
  Fixture f(source(scrollRows(40)));
  f.settle(3);
  std::string expected;
  bool upFirst{};
  SUBCASE("committed BOF clips UP before a later DOWN") {
    checkTop(f, "row-0");
    upFirst = true;
    expected = "row-3";
  }
  SUBCASE("committed final row clips DOWN before a later UP") {
    wheel(f, false, 1000);
    f.settle(12);
    checkTop(f, "row-34");
    REQUIRE(f.frame()->status.finalEnd);
    expected = "row-31";
  }

  wheel(f, upFirst, 3);
  wheel(f, !upFirst, 3);
  f.settle(5);
  REQUIRE(f.frame());
  const auto actual = std::string(f.frame()->rows.front().bytes());
  std::printf("PRTLS-359 knownBoundary upFirst=%d expectedTop=%s actualTop=%s\n", static_cast<int>(upFirst),
              expected.c_str(), actual.c_str());
  checkTop(f, expected);
}

TEST_CASE("visible final EOF projects both text position and scrollbar end without changing byte anchor") {
  Fixture f(source(scrollRows(40)));
  f.settle(3);
  REQUIRE(f.input(InputKey::END));
  f.settle(4);
  const auto status = take(f.reference.status());
  REQUIRE(f.frame());
  REQUIRE(f.frame()->footer);
  REQUIRE(status.finalEnd);
  REQUIRE(status.byteExtent);
  CHECK(status.byteOffset < *status.byteExtent);
  CHECK(f.frame()->footer->bytes().find("100.0%") != std::string_view::npos);
  CHECK(f.frame()->footer->bytes().find("byte ") == std::string_view::npos);
  take(f.app->tree().read(f.node, [&](const DocumentView &widget) {
    const auto &scroll = detail::ScrollWidgetAccess::state(widget);
    const auto position = take(scroll.controller->snapshot());
    CHECK(position.vertical.offset.raster == scrollRange(position.vertical.metrics).maximum);
  }));
  CHECK(take(f.reference.status()).byteOffset == status.byteOffset);
}

TEST_CASE("byte approximation never rounds to100 before the final rows are visible") {
  auto provider = std::make_shared<Provider>();
  provider->syntheticExtent = (std::uint64_t{1} << 63) + 101;
  Fixture f(ui::document(provider));
  f.settle(3);
  take(f.reference.seekByte(*provider->syntheticExtent - 100));
  f.settle(4);
  REQUIRE(f.frame());
  REQUIRE(f.frame()->footer);
  REQUIRE_FALSE(f.frame()->status.finalEnd);
  CHECK(f.frame()->footer->bytes().find("100.0%") == std::string_view::npos);
  CHECK(f.frame()->footer->bytes().find("99.9%") != std::string_view::npos);
  CHECK(f.frame()->footer->bytes().find("line ") == std::string_view::npos);
  CHECK(take(f.reference.status()).byteOffset == *provider->syntheticExtent - 100);
}

TEST_CASE("line-number no-op and rapid presentation reflow retain accepted relative navigation") {
  Fixture f(source(scrollRows(40)));
  f.settle(3);
  SUBCASE("same configuration is an exact no-op") {
    const auto serial = navigationSerial(f);
    const auto frame = f.frame();
    const auto submitted = f.executor.submissions;
    take(f.app->tree().consumeFrameRequest());
    take(setDocumentLineNumbers(*f.scene, f.node, false));
    take(f.reference.setLineNumbers(false));
    CHECK(f.reference.lineNumbers(false));
    CHECK(navigationSerial(f) == serial);
    CHECK(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE);
    f.settle(2);
    CHECK(f.frame() == frame);
    CHECK(f.executor.submissions == submitted);
  }
  SUBCASE("toggle and toggle back while a batch is held") {
    wheel(f, false, 3);
    f.pump();
    REQUIRE(f.executor.job);
    wheel(f, false, 4);
    CHECK(f.reference.toggleLineNumbers());
    CHECK(f.reference.toggleLineNumbers());
    take(setDocumentWrap(*f.scene, f.node, false));
    f.pump();
    f.executor.runOne();
    f.settle(8);
    checkTop(f, "row-7");
  }
}

TEST_CASE("Unicode final cluster and LF in a one-row viewport require real displayed EOF proof") {
  std::string text = "abcdefghij"
                     "e\xCC\x81\n";
  DocumentViewConfig config;
  std::uint64_t expectedAnchor = 10;
  SUBCASE("UTF-8 combining cluster") {
  }
  SUBCASE("UTF-16LE BOM and combining cluster") {
    text.assign("\xFF\xFE", 2);
    for (const char16_t unit : std::u16string(u"abcdefghije\u0301\n")) {
      text.push_back(static_cast<char>(unit & 0xff));
      text.push_back(static_cast<char>(unit >> 8));
    }
    config.decoder.explicitEncoding = DocumentEncoding::UTF16LE;
    expectedAnchor = 22;
  }

  Fixture f(source(std::move(text)), config);
  f.size = {12, 3};
  f.settle(3);
  REQUIRE(f.frame());
  REQUIRE(f.frame()->viewport == CellSize{10, 1});
  CHECK(f.frame()->rows.front().bytes() == "abcdefghij");
  CHECK_FALSE(f.frame()->finalEndVisible);
  CHECK(f.frame()->footer->bytes().find("100.0%") == std::string_view::npos);

  wheel(f);
  f.settle(3);
  REQUIRE(f.frame());
  CHECK(f.frame()->rows.front().bytes() == "e\xCC\x81");
  CHECK(f.frame()->finalEndVisible);
  CHECK(f.frame()->status.finalEnd);
  CHECK(f.frame()->footer->bytes().find("100.0%") != std::string_view::npos);
  const auto anchor = take(f.reference.status()).byteOffset;
  REQUIRE(f.input(InputKey::HOME));
  f.pump();
  CHECK(take(f.reference.status()).byteOffset == 0);
  f.settle(3);
  CHECK_FALSE(f.frame()->finalEndVisible);
  CHECK(f.frame()->footer->bytes().find("100.0%") == std::string_view::npos);
  CHECK(anchor == expectedAnchor);
}

TEST_CASE("unknown extent final read proof differs from nonfinal waiting") {
  auto provider = std::make_shared<Provider>();
  provider->unknown = true;
  provider->text = "short";
  SUBCASE("explicit final read proves EOF without an invented denominator") {
    provider->finalUnknown = true;
    Fixture f(ui::document(provider));
    f.settle(3);
    REQUIRE(f.frame());
    CHECK_FALSE(f.frame()->status.byteExtent);
    CHECK(f.frame()->finalEndVisible);
    CHECK(f.frame()->footer->bytes().find("100.0%") != std::string_view::npos);
    CHECK_FALSE(provider->uiOperation.load());
  }
  SUBCASE("a stopped provider hides a retained old EOF caption") {
    provider->finalUnknown = true;
    Fixture f(ui::document(provider));
    f.settle(3);
    REQUIRE(f.frame());
    REQUIRE(f.frame()->footer->bytes().find("100.0%") != std::string_view::npos);
    provider->providerWouldBlock = true;
    take(f.reference.refresh());
    f.pump();
    f.executor.runOne();
    f.pump();
    REQUIRE(take(f.reference.status()).waiting);
    REQUIRE(f.frame()->footer->bytes().find("100.0%") != std::string_view::npos);
    const auto sink = paint(f);
    CHECK(std::none_of(sink.texts.begin(), sink.texts.end(), [](const PartSink::Text &text) {
      return text.bytes.find("100.0%") != std::string::npos;
    }));
    CHECK_FALSE(take(f.reference.status()).byteExtent);
  }
  SUBCASE("empty nonfinal read does not prove EOF") {
    provider->emptyMore = true;
    Fixture f(ui::document(provider));
    f.settle(3);
    REQUIRE(f.frame());
    CHECK(f.frame()->status.waiting);
    CHECK_FALSE(f.frame()->finalEndVisible);
    CHECK(f.frame()->footer->bytes().find("100.0%") == std::string_view::npos);
    CHECK(f.frame()->footer->bytes().find("progress unknown") != std::string_view::npos);
  }
}

TEST_CASE("compact line-number commands preserve aliases and reject foreign or retired owners") {
  Fixture f(source("first\nsecond"));
  f.settle(3);
  auto alias = f.reference;
  CHECK(alias.toggleLineNumbers());
  f.settle(3);
  REQUIRE(f.frame()->gutters);
  CHECK((*f.frame()->gutters)[0].bytes() == "      1  ");
  CHECK(take(f.reference.status()).byteOffset == 0);
  CHECK(f.reference.lineNumbers(false));
  f.settle(3);
  CHECK_FALSE(f.frame()->gutters);

  const auto serial = navigationSerial(f);
  std::optional<ManagedError> foreign;
  std::thread worker([ref = alias, &foreign] {
    auto result = ref.setLineNumbers(true);
    if (!result) {
      foreign = result.error();
    }
  });
  worker.join();
  REQUIRE(foreign);
  CHECK(std::get<TreeError>(foreign->cause) == TreeError::WRONG_THREAD);
  CHECK(navigationSerial(f) == serial);

  unsigned callbacks{};
  take(f.scene->onError([&](const ManagedDiagnostic &) {
    ++callbacks;
    f.reference = {};
  }));
  take(f.app->tree().remove(f.node));
  CHECK_FALSE(alias.setLineNumbers(true));
  CHECK(callbacks == 0);
  CHECK_FALSE(f.reference.toggleLineNumbers());
  CHECK(callbacks == 1);
}

TEST_CASE("hot gutter reflow preserves the requested original byte anchor across wrapping") {
  Fixture f(source(std::string(100, 'x')));
  f.settle(3);
  take(f.reference.seekByte(17));
  f.settle(3);
  REQUIRE(take(f.reference.status()).byteOffset == 17);
  CHECK(f.reference.lineNumbers(true));
  f.settle(3);
  CHECK(take(f.reference.status()).byteOffset == 17);
  REQUIRE(f.frame()->gutters);
  take(setDocumentWrap(*f.scene, f.node, false));
  f.settle(3);
  CHECK(take(f.reference.status()).byteOffset == 17);
  CHECK(f.reference.toggleLineNumbers());
  f.settle(3);
  CHECK_FALSE(f.frame()->gutters);
  CHECK(take(f.reference.status()).byteOffset == 17);
}

TEST_CASE("a tall reading viewport stops at its last full page for wheel and burst input") {
  Fixture f(source(scrollRows(120)));
  f.size = {40, 50};
  f.settle(4);
  REQUIRE(f.frame()->viewport.height == 48);
  wheel(f, false, 71);
  f.settle(12);
  checkTop(f, "row-71");
  CHECK_FALSE(f.frame()->finalEndVisible);
  wheel(f);
  f.settle(4);
  checkTop(f, "row-72");
  REQUIRE(f.frame()->rows.size() == 48);
  CHECK(f.frame()->rows.back().bytes() == "row-119");
  CHECK(f.frame()->finalEndVisible);
  const auto submissions = f.executor.submissions;
  wheel(f);
  wheel(f, false, 1000);
  f.settle(4);
  checkTop(f, "row-72");
  CHECK(f.executor.submissions == submissions);
  wheel(f, true);
  f.settle(4);
  checkTop(f, "row-71");
}

TEST_CASE("the last wheel step does not switch scrollbar coordinate systems") {
  Fixture f(source(scrollRows(120)));
  f.size = {40, 50};
  f.settle(4);
  wheel(f, false, 71);
  f.settle(12);
  REQUIRE(f.frame());
  REQUIRE_FALSE(f.frame()->finalEndVisible);
  take(f.app->tree().read(f.node, [&](const DocumentView &widget) {
    const auto &scroll = detail::ScrollWidgetAccess::state(widget);
    const auto value = take(scroll.controller->snapshot());
    const auto maximum = scrollRange(value.vertical.metrics).maximum;
    REQUIRE(maximum > 0);
    CHECK(value.vertical.offset.raster < maximum);
    CHECK(value.vertical.offset.raster * 100 > maximum * 95);
  }));
  wheel(f);
  f.settle(4);
  REQUIRE(f.frame()->finalEndVisible);
  take(f.app->tree().read(f.node, [&](const DocumentView &widget) {
    const auto &scroll = detail::ScrollWidgetAccess::state(widget);
    const auto value = take(scroll.controller->snapshot());
    CHECK(value.vertical.offset.raster == scrollRange(value.vertical.metrics).maximum);
  }));
}

namespace {
ScrollControllerSnapshot documentBar(Fixture &fixture) {
  ScrollControllerSnapshot snapshot;

  take(fixture.app->tree().read(fixture.node, [&](const DocumentView &widget) {
    snapshot = take(detail::ScrollWidgetAccess::state(widget).controller->snapshot());
  }));

  return snapshot;
}

ScrollTrack documentTrack(Fixture &fixture) {
  ScrollTrack track;

  take(fixture.app->tree().read(fixture.node, [&](const DocumentView &widget) {
    const auto &scroll = detail::ScrollWidgetAccess::state(widget);
    REQUIRE(scroll.geometry.verticalBar);
    track = scroll.geometry.vertical;
  }));

  return track;
}

void dragDocument(Fixture &fixture, ScrollTrack track, MouseAction action, std::int32_t row) {
  const InputEvent event = MouseInputEvent{track.origin.x, row, MouseButton::LEFT, action};

  if (!take(fixture.scene->input(*fixture.app, event))) {
    take(fixture.app->router().dispatch(event));
  }
}
} // namespace

TEST_CASE("scrollbar maximum retains the last full reading page while held and after release") {
  Fixture f(source(scrollRows(120)));
  f.size = {40, 50};
  f.settle(4);
  REQUIRE(f.frame()->viewport.height == 48);

  const auto track = documentTrack(f);
  const auto end = track.origin.y + track.length + 10;

  dragDocument(f, track, MouseAction::PRESS, track.origin.y);
  REQUIRE(take(f.app->router().pointerCapture()) == f.node);
  dragDocument(f, track, MouseAction::MOVE, end);
  f.settle(6);
  REQUIRE(f.frame());
  REQUIRE(f.frame()->rows.size() == 48);
  CHECK(f.frame()->rows.front().bytes() == "row-72");
  CHECK(f.frame()->rows.back().bytes() == "row-119");
  CHECK(f.frame()->finalEndVisible);
  CHECK(take(f.app->router().pointerCapture()) == f.node);

  dragDocument(f, track, MouseAction::RELEASE, end);
  f.settle(3);
  const auto submissions = f.executor.submissions;

  wheel(f);
  wheel(f, false, 1000);
  REQUIRE(f.input(InputKey::DOWN));
  f.settle(3);
  checkTop(f, "row-72");
  CHECK(f.executor.submissions == submissions);
}

TEST_CASE("held scrollbar preserves its byte projection across variable row lengths") {
  auto provider = std::make_shared<Provider>();
  provider->text = scrollRows(60);

  for (unsigned row = 60; row < 120; ++row) {
    provider->text += "row-" + std::to_string(row) + std::string(240, 'x') + "\n";
  }

  Fixture f(ui::document(provider), {.wrap = false});
  f.size = {40, 20};
  f.settle(4);

  const auto track = documentTrack(f);
  const auto middle = track.origin.y + track.length / 2;

  dragDocument(f, track, MouseAction::PRESS, track.origin.y);
  REQUIRE(take(f.app->router().pointerCapture()) == f.node);
  dragDocument(f, track, MouseAction::MOVE, middle);
  const auto intended = documentBar(f).vertical.offset;

  f.pump();
  REQUIRE(f.executor.job);
  const auto firstAnchor = take(f.reference.status()).byteOffset;
  const auto busySerial = navigationSerial(f);
  const auto busySubmissions = f.executor.submissions;

  dragDocument(f, track, MouseAction::MOVE, middle);
  f.pump();
  CHECK(navigationSerial(f) == busySerial);
  CHECK(f.executor.submissions == busySubmissions);
  CHECK(documentBar(f).vertical.offset.raster == intended.raster);

  f.settle(6);
  REQUIRE(f.frame());
  CHECK(documentBar(f).vertical.offset.raster == intended.raster);
  CHECK(documentBar(f).vertical.offset.fraction == intended.fraction);
  CHECK(take(f.app->router().pointerCapture()) == f.node);

  const auto serial = navigationSerial(f);
  const auto submissions = f.executor.submissions;
  const auto reads = provider->reads.load();
  const auto revision = documentBar(f).revision;

  dragDocument(f, track, MouseAction::MOVE, middle);
  f.settle(3);
  CHECK(navigationSerial(f) == serial);
  CHECK(f.executor.submissions == submissions);
  CHECK(provider->reads.load() == reads);
  CHECK(documentBar(f).revision == revision);

  dragDocument(f, track, MouseAction::MOVE, middle + 1);
  f.pump();
  CHECK(take(f.reference.status()).byteOffset > firstAnchor);
  f.settle(6);
  CHECK(take(f.app->router().pointerCapture()) == f.node);

  SUBCASE("a fresh capture without an idle frame uses the newly visible page") {
    const auto range = detail::documentScrollByteRange(*f.frame());

    dragDocument(f, track, MouseAction::RELEASE, middle + 1);

    const auto current = documentBar(f).vertical;
    const auto thumb = take(scrollFractionalThumb(current.metrics, current.offset, track.length));
    const auto grab = track.origin.y + static_cast<std::int32_t>(thumb.startEighths / 8);

    dragDocument(f, track, MouseAction::PRESS, grab);
    REQUIRE(take(f.app->router().pointerCapture()) == f.node);
    dragDocument(f, track, MouseAction::MOVE, grab + 2);

    const auto position = documentBar(f).vertical;
    const auto expected =
      detail::scrollMultiplyDivide(position.offset.raster, range, scrollRange(position.metrics).maximum).quotient;

    REQUIRE(position.offset.raster > current.offset.raster);
    CHECK(take(f.app->router().pointerCapture()) == f.node);
    f.pump();
    CHECK(take(f.reference.status()).byteOffset == expected);
    f.settle(6);
    dragDocument(f, track, MouseAction::RELEASE, grab + 2);
  }

  SUBCASE("resizing cancels the capture and an old move cannot navigate") {
    f.size = {41, 21};
    f.settle(4);
    CHECK(take(f.app->router().pointerCapture()) == Node<Widget>{});

    const auto before = navigationSerial(f);
    const auto jobs = f.executor.submissions;

    dragDocument(f, track, MouseAction::MOVE, middle + 2);
    f.settle(3);
    CHECK(navigationSerial(f) == before);
    CHECK(f.executor.submissions == jobs);
  }

  SUBCASE("a same-extent replacement rejects moves from the old capture") {
    auto replacement = scrollRows(120, "new-");
    replacement.resize(provider->text.size(), 'z');
    take(f.reference.setModel(source(std::move(replacement))));
    dragDocument(f, track, MouseAction::MOVE, middle + 2);
    f.pump();
    dragDocument(f, track, MouseAction::MOVE, middle + 3);
    dragDocument(f, track, MouseAction::RELEASE, middle + 3);
    f.settle(4);
    checkTop(f, "new-0");

    const auto before = navigationSerial(f);
    const auto jobs = f.executor.submissions;

    dragDocument(f, track, MouseAction::MOVE, middle + 2);
    f.settle(3);
    CHECK(take(f.app->router().pointerCapture()) == Node<Widget>{});
    CHECK(navigationSerial(f) == before);
    CHECK(f.executor.submissions == jobs);
    checkTop(f, "new-0");
  }

  f.settle(3);
  CHECK_FALSE(provider->uiOperation.load());
}

TEST_CASE("repeated scrollbar End retains a full tall page after long unwrapped rows") {
  auto provider = std::make_shared<Provider>();

  for (unsigned row = 0; row < 120; ++row) {
    provider->text += "row-" + std::to_string(row) + ":" + std::string(2048, 'x') + "\n";
  }

  Fixture f(ui::document(provider), {.wrap = false});
  f.size = {40, 50};
  f.settle(4);
  REQUIRE(f.frame());
  REQUIRE(f.frame()->viewport.height == 48);

  const auto track = documentTrack(f);
  const auto end = track.origin.y + track.length + 10;
  const auto away = track.origin.y + track.length - 6;

  dragDocument(f, track, MouseAction::PRESS, track.origin.y);
  REQUIRE(take(f.app->router().pointerCapture()) == f.node);
  dragDocument(f, track, MouseAction::MOVE, end);
  f.settle(6);
  REQUIRE(f.frame()->rows.size() == 48);
  REQUIRE(f.frame()->rows.front().bytes().starts_with("row-72:"));
  REQUIRE(f.frame()->rows.back().bytes().starts_with("row-119:"));

  dragDocument(f, track, MouseAction::MOVE, away);
  f.settle(6);
  REQUIRE_FALSE(f.frame()->finalEndVisible);
  REQUIRE(take(f.app->router().pointerCapture()) == f.node);

  SUBCASE("the same mouse gesture returns to EOF") {
    dragDocument(f, track, MouseAction::MOVE, end);
  }

  SUBCASE("End returns to EOF after releasing the mouse") {
    dragDocument(f, track, MouseAction::RELEASE, away);
    REQUIRE(f.input(InputKey::END));
  }

  f.settle(6);
  REQUIRE(f.frame());
  CHECK(f.frame()->rows.size() == 48);
  CHECK(f.frame()->rows.front().bytes().starts_with("row-72:"));
  CHECK(f.frame()->rows.back().bytes().starts_with("row-119:"));
  CHECK(f.frame()->finalEndVisible);
  CHECK_FALSE(provider->uiOperation.load());
  dragDocument(f, track, MouseAction::RELEASE, end);
}

TEST_CASE("End and scrollbar maximum prepare only a bounded suffix on the managed worker") {
  constexpr std::uint64_t SOURCE_BYTES = 64 * 1024 * 1024;
  auto provider = std::make_shared<Provider>();
  provider->syntheticExtent = SOURCE_BYTES;
  Fixture f(ui::document(provider));
  f.size = {40, 50};
  f.settle(4);
  REQUIRE(f.frame());
  REQUIRE(f.frame()->viewport.height == 48);
  provider->bytesRead = 0;
  std::size_t pageBytes{};
  take(f.app->tree().read(f.node, [&](const DocumentView &widget) {
    pageBytes = detail::DocumentViewAccess::state(widget).source->maxPageBytes();
  }));

  SUBCASE("End key") {
    REQUIRE(f.input(InputKey::END));
  }

  SUBCASE("scrollbar maximum") {
    const auto track = documentTrack(f);
    const auto end = track.origin.y + track.length + 10;
    dragDocument(f, track, MouseAction::PRESS, track.origin.y);
    REQUIRE(take(f.app->router().pointerCapture()) == f.node);
    dragDocument(f, track, MouseAction::MOVE, end);
    dragDocument(f, track, MouseAction::RELEASE, end);
  }

  f.settle(6);
  REQUIRE(f.frame());
  REQUIRE(f.frame()->rows.size() == 48);
  CHECK(f.frame()->rows.front().bytes() == "x");
  CHECK(f.frame()->rows.back().bytes() == "x");
  CHECK(f.frame()->status.visibleByteOffset == SOURCE_BYTES - 96);
  CHECK(f.frame()->finalEndVisible);
  CHECK(provider->bytesRead.load() <= 2 * pageBytes + 96 + DocumentDecoder::LOCAL_WINDOW_BYTES);
  CHECK_FALSE(provider->uiOperation.load());
}

TEST_CASE("page input uses the current arranged body after resize before preparation") {
  Fixture f(source(scrollRows(60)));
  f.settle(4);
  REQUIRE(f.frame()->viewport.height == 6);
  f.size = {24, 12};
  const auto bounds = take(CellRect::from({}, f.size));

  take(f.app->tree().frame([&] {
    take(layoutTree(f.app->tree(), f.app->root(), bounds, bounds));
  }));
  take(f.app->tree().read(f.node, [&](const DocumentView &widget) {
    REQUIRE(detail::ScrollWidgetAccess::state(widget).geometry.viewport.height == 10);
  }));
  REQUIRE(f.input(InputKey::PAGE_DOWN));
  f.settle(6);
  checkTop(f, "row-9");
}

TEST_CASE("zero-body page input is consumed without changing Ref page rejection") {
  auto provider = std::make_shared<Provider>();
  provider->text = scrollRows(60);
  Fixture f(ui::document(provider));
  f.settle(4);
  REQUIRE(f.frame()->viewport.height == 6);
  const auto frame = f.frame();
  const auto serial = navigationSerial(f);
  const auto reads = provider->reads.load();
  const auto jobs = f.executor.submissions;
  f.size = {24, 2};
  const auto bounds = take(CellRect::from({}, f.size));

  take(f.app->tree().frame([&] {
    take(layoutTree(f.app->tree(), f.app->root(), bounds, bounds));
  }));
  take(f.app->tree().read(f.node, [&](const DocumentView &widget) {
    REQUIRE(detail::ScrollWidgetAccess::state(widget).geometry.viewport.height == 0);
  }));
  take(f.app->tree().consumeFrameRequest());

  const auto input = f.scene->input(*f.app, KeyEvent{.key = InputKey::PAGE_DOWN});

  REQUIRE(input);
  CHECK(*input);
  CHECK(navigationSerial(f) == serial);
  CHECK(f.frame() == frame);
  CHECK(f.executor.submissions == jobs);
  CHECK(provider->reads.load() == reads);
  CHECK(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE);

  const auto checked = f.reference.scrollPages(1);
  REQUIRE_FALSE(checked);
  CHECK(std::get<TreeError>(checked.error().cause) == TreeError::WOULD_BLOCK);
  unsigned callbacks{};
  take(f.scene->onError([&](const ManagedDiagnostic &diagnostic) {
    ++callbacks;
    CHECK(std::get<TreeError>(diagnostic.error.cause) == TreeError::WOULD_BLOCK);
  }));
  CHECK_FALSE(f.reference.page(1));
  CHECK(callbacks == 1);
  CHECK(navigationSerial(f) == serial);
  CHECK(f.frame() == frame);
  CHECK(f.executor.submissions == jobs);
  CHECK(provider->reads.load() == reads);
}

TEST_CASE("input and Ref navigation share lossless pending rows while a worker is busy") {
  Fixture f(source(scrollRows(100)), {.fastScroll = DocumentFastScroll{}});
  f.settle(3);
  REQUIRE(take(f.scene->input(*f.app, KeyEvent{.key = InputKey::DOWN, .modifiers = {.control = true}})));
  f.pump();
  REQUIRE(f.executor.job);
  REQUIRE(f.reference.scroll(2));
  REQUIRE(f.reference.page(1));
  REQUIRE(take(f.scene->input(*f.app, MouseInputEvent{.column = 2,
                                                      .row = 2,
                                                      .button = MouseButton::WHEEL_DOWN,
                                                      .action = MouseAction::SCROLL,
                                                      .modifiers = {.alt = true},
                                                      .wheelDelta = 1.0})));
  take(f.reference.scrollRows(-3));
  f.settle(14);
  checkTop(f, "row-20");
  REQUIRE(f.input(InputKey::PAGE_DOWN));
  f.settle(5);
  checkTop(f, "row-25");
  take(f.reference.goHome());
  f.settle(4);
  checkTop(f, "row-0");
  REQUIRE(f.reference.end());
  f.settle(5);
  checkTop(f, "row-94");
  REQUIRE(f.reference.scroll(-1));
  f.settle(4);
  checkTop(f, "row-93");
}

TEST_CASE("page admission requires arranged geometry and zero and overflow preserve navigation state") {
  Fixture f(source(scrollRows(50)));
  const auto before = navigationSerial(f);
  const auto blocked = f.reference.scrollPages(1);
  REQUIRE_FALSE(blocked);
  CHECK(std::get<TreeError>(blocked.error().cause) == TreeError::WOULD_BLOCK);
  take(f.reference.scrollRows(0));
  take(f.reference.scrollPages(0));
  CHECK(navigationSerial(f) == before);
  CHECK(f.executor.submissions == 0);
  f.settle(3);
  const auto serial = navigationSerial(f);
  const auto jobs = f.executor.submissions;
  take(f.app->tree().consumeFrameRequest());
  take(f.reference.scrollRows(0));
  take(f.reference.scrollPages(0));
  CHECK(navigationSerial(f) == serial);
  CHECK(f.executor.submissions == jobs);
  CHECK(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  for (const auto count : {std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()}) {
    const auto overflow = f.reference.scrollPages(count);
    REQUIRE_FALSE(overflow);
    CHECK(std::get<TreeError>(overflow.error().cause) == TreeError::INVALID_ARGUMENT);
    CHECK(navigationSerial(f) == serial);
  }
  take(f.reference.scrollRows(std::numeric_limits<std::int64_t>::min()));
  REQUIRE(f.reference.scroll(1));
  f.settle(4);
  checkTop(f, "row-1");
}

TEST_CASE("programmatic navigation preserves seek convention for disabled read-only input") {
  Fixture f(source(scrollRows(30)));
  f.settle(3);
  take(f.app->tree().edit(f.node, [](DocumentView &widget) {
    auto &scroll = detail::ScrollWidgetAccess::state(widget);
    scroll.enabled = false;
    scroll.area.readOnly = true;
  }));
  CHECK_FALSE(f.input(InputKey::DOWN));
  REQUIRE(f.reference.scroll(3));
  f.settle(4);
  checkTop(f, "row-3");
  REQUIRE(f.reference.home());
  f.settle(3);
  checkTop(f, "row-0");
}

TEST_CASE("navigation Ref checks immutable affinity and collected failure may retire its target") {
  Fixture f(source(scrollRows(20)));
  auto alias = f.reference;
  std::optional<ManagedError> foreign;
  std::thread worker([ref = alias, &foreign] {
    const auto result = ref.scrollRows(0);
    if (!result) {
      foreign = result.error();
    }
  });
  worker.join();
  REQUIRE(foreign);
  CHECK(std::get<TreeError>(foreign->cause) == TreeError::WRONG_THREAD);
  unsigned callbacks{};
  take(f.scene->onError([&](const ManagedDiagnostic &) {
    if (++callbacks == 1) {
      take(f.app->tree().remove(f.node));
      f.reference = {};
    }
  }));
  CHECK_FALSE(alias.page(1));
  CHECK(callbacks == 1);
  CHECK_FALSE(alias.goHome());
  CHECK(callbacks == 1);
  CHECK_FALSE(alias.scroll(0));
  CHECK(callbacks == 2);
}

TEST_CASE("Viewer fluent navigation policy is sparse checked and uses absolute fast rows") {
  Executor executor;
  auto app = take(Application::create({.backgroundExecutor = &executor}));
  auto scene = take(ManagedScene::create(*app));
  const auto node = take(ui::mount(
    *scene, ui::viewer(source(scrollRows(20))).fastScroll(5, key::SHIFT, key::CTRL).scrollStep(2).pageOverlap(0)));
  take(app->tree().read(node, [](const DocumentView &widget) {
    const auto &state = detail::DocumentViewAccess::state(widget);
    REQUIRE(state.config.fastScroll);
    CHECK(state.config.fastScroll->step == 5);
    CHECK(state.config.fastScroll->keyboard == key::SHIFT);
    CHECK(state.config.fastScroll->wheel == key::CTRL);
    const auto &area = detail::ScrollWidgetAccess::state(widget).area;
    CHECK(area.keyStep == 2);
    CHECK(area.wheelStep == 2);
    CHECK(area.pageOverlap == 0);
  }));
  CHECK_FALSE(ui::mount(*scene, ui::viewer(source("x")).fastScroll(0)));
  CHECK_FALSE(ui::mount(*scene, ui::viewer(source("x")).fastScroll(std::uint64_t{1} << 63)));
  CHECK_FALSE(ui::mount(*scene, ui::viewer(source("x")).fastScroll(8, static_cast<key::Modifier>(99))));
  CHECK_FALSE(ui::mount(*scene, ui::viewer(source("x")).fastScroll(8, key::CTRL, static_cast<key::Modifier>(99))));
  CHECK_FALSE(ui::mount(*scene, ui::viewer(source("x")).scrollStep(std::uint64_t{1} << 63)));
}

TEST_CASE("fast input is opt-in exact and absolute while paging keeps its own overlap") {
  SUBCASE("unmodified defaults do not claim modified input") {
    Fixture f(source(scrollRows(60)));
    f.settle(3);
    CHECK_FALSE(take(f.scene->input(*f.app, KeyEvent{.key = InputKey::DOWN, .modifiers = {.control = true}})));
    CHECK_FALSE(take(f.scene->input(*f.app, MouseInputEvent{.column = 2,
                                                            .row = 2,
                                                            .button = MouseButton::WHEEL_DOWN,
                                                            .action = MouseAction::SCROLL,
                                                            .modifiers = {.alt = true},
                                                            .wheelDelta = 1.0})));
    REQUIRE(f.input(InputKey::DOWN));
    f.settle(4);
    checkTop(f, "row-1");
  }
  SUBCASE("configured modifier tags use five rows rather than multiplying the ordinary two") {
    Fixture f(source(scrollRows(60)), {.fastScroll = DocumentFastScroll{5, key::SHIFT, key::CTRL}});
    f.settle(3);
    take(f.app->tree().edit(f.node, [](DocumentView &widget) {
      auto &area = detail::ScrollWidgetAccess::state(widget).area;
      area.keyStep = 2;
      area.wheelStep = 2;
      area.pageOverlap = 0;
    }));
    REQUIRE(f.input(InputKey::DOWN));
    REQUIRE(take(f.scene->input(*f.app, KeyEvent{.key = InputKey::DOWN, .modifiers = {.shift = true}})));
    REQUIRE(take(f.scene->input(*f.app, MouseInputEvent{.column = 2,
                                                        .row = 2,
                                                        .button = MouseButton::WHEEL_DOWN,
                                                        .action = MouseAction::SCROLL,
                                                        .modifiers = {.control = true},
                                                        .wheelDelta = 1.0})));
    CHECK_FALSE(
      take(f.scene->input(*f.app, KeyEvent{.key = InputKey::DOWN, .modifiers = {.shift = true, .control = true}})));
    CHECK_FALSE(take(f.scene->input(*f.app, KeyEvent{.key = InputKey::PAGE_DOWN, .modifiers = {.shift = true}})));
    f.settle(10);
    checkTop(f, "row-12");
    REQUIRE(f.input(InputKey::PAGE_DOWN));
    f.settle(5);
    checkTop(f, "row-18");
  }
}

TEST_CASE("accepted pages keep admission rows across resize and Ref row overflow is atomic") {
  Fixture f(source(scrollRows(60)));
  f.settle(3);
  take(f.reference.scrollPages(1));
  f.size = {24, 12};
  f.settle(6);
  checkTop(f, "row-5");
  take(f.reference.scrollPages(2));
  f.settle(7);
  checkTop(f, "row-23");
  take(f.reference.goHome());
  f.settle(4);
  const auto jobs = f.executor.submissions;
  constexpr auto MAX = std::numeric_limits<std::int64_t>::max();
  take(f.reference.scrollRows(MAX));
  const auto rejected = f.reference.scrollRows(1);
  REQUIRE_FALSE(rejected);
  CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::INVALID_ARGUMENT);
  take(f.reference.scrollRows(-MAX));
  f.settle(4);
  checkTop(f, "row-0");
  CHECK(f.executor.submissions == jobs);
}

TEST_CASE("HEX mode changes preserve raw UTF anchors and retained TEXT settings") {
  const auto text = std::string("\xEF\xBB\xBF") + "a\xC3\xA9\nsecond\nthird";
  Fixture f(source(text), {.wrap = false, .lineNumbers = true});
  f.size = {100, 6};
  f.settle();
  take(f.reference.seekByte(5));
  f.settle();
  const auto before = take(f.reference.status());
  REQUIRE(before.byteOffset == 5);
  take(f.reference.setMode(DocumentViewMode::HEX));
  CHECK(take(f.reference.status()).byteOffset == 5);
  f.settle();
  const auto hex = f.frame();
  REQUIRE(hex);
  CHECK(hex->status.byteOffset == 5);
  CHECK(hex->status.visibleByteOffset == 0);
  CHECK_FALSE(hex->status.logicalLine);
  CHECK_FALSE(hex->status.totalLines);
  CHECK(hex->rows.front().bytes().starts_with("0000000000000000  EF BB BF 61 C3 A9 0A"));
  take(f.app->tree().read(f.node, [](const DocumentView &widget) {
    const auto &state = detail::DocumentViewAccess::state(widget);
    CHECK(state.config.lineNumbers);
    CHECK_FALSE(state.config.wrap);
    REQUIRE(state.roles.snapshot());
    CHECK(state.roles.snapshot()->values.size() == 7);
    CHECK(detail::ScrollWidgetAccess::state(widget).area.horizontal == ScrollBarPolicy::AUTO);
  }));
  take(f.reference.setMode(DocumentViewMode::TEXT));
  f.settle();
  CHECK(take(f.reference.status()).byteOffset == 5);
  CHECK(take(f.reference.status()).mode == DocumentViewMode::TEXT);
  take(f.app->tree().read(f.node, [](const DocumentView &widget) {
    const auto &state = detail::DocumentViewAccess::state(widget);
    REQUIRE(state.roles.snapshot());
    CHECK(state.roles.snapshot()->values.size() == 3);
    CHECK(detail::ScrollWidgetAccess::state(widget).area.horizontal == ScrollBarPolicy::NEVER);
  }));
}

TEST_CASE("mode change supersedes unresolved old row commands while no-op retains them") {
  Fixture f(source(scrollRows(200)), {.workBytes = 32});
  f.size = {100, 6};
  f.settle(30);
  take(f.reference.scrollRows(7));
  f.pump();
  REQUIRE(f.executor.job);
  take(f.reference.scrollRows(5));
  const auto anchor = take(f.reference.status()).byteOffset;
  take(f.reference.setMode(DocumentViewMode::HEX));
  take(f.reference.setMode(DocumentViewMode::TEXT));
  f.settle(40);
  CHECK(take(f.reference.status()).byteOffset == anchor);
  CHECK(take(f.reference.status()).mode == DocumentViewMode::TEXT);
  CHECK_FALSE(take(f.reference.status()).error);
  take(f.reference.scrollRows(2));
  take(f.reference.setMode(DocumentViewMode::TEXT));
  f.settle(30);
  REQUIRE(f.frame());
  CHECK(take(f.reference.status()).byteOffset > anchor);
}

TEST_CASE("HEX row option and resize reflow preserve raw anchors and latest generation") {
  Fixture f(source(std::string(1000, 'A')));
  f.size = {100, 6};
  f.settle();
  take(f.reference.seekByte(73));
  take(f.reference.setMode(DocumentViewMode::HEX));
  f.pump();
  REQUIRE(f.executor.job);
  take(f.reference.setHexOptions({.bytesPerRow = 8, .groupSize = 4, .uppercase = false}));
  f.size = {60, 5};
  f.settle(30);
  const auto frame = f.frame();
  REQUIRE(frame);
  CHECK(frame->status.byteOffset == 73);
  CHECK(frame->status.visibleByteOffset == 72);
  CHECK(frame->rows.front().bytes().starts_with("0000000000000048"));
  take(f.reference.setHexOptions({.bytesPerRow = 32, .autoFit = true}));
  f.size = {50, 5};
  f.settle(30);
  REQUIRE(f.frame());
  CHECK(f.frame()->status.byteOffset == 73);
  CHECK(f.frame()->hex->columns <= static_cast<std::size_t>(f.frame()->viewport.width));
}

TEST_CASE("HEX navigation shares lossless queue maps rows and pages to bytes and ends at full page") {
  Fixture f(source(std::string(4099, 'B')), {.mode = DocumentViewMode::HEX});
  f.size = {100, 6};
  f.settle();
  const auto height = f.frame()->viewport.height;
  take(f.reference.scrollRows(3));
  f.pump();
  take(f.reference.scrollRows(2));
  take(f.reference.scrollPages(1));
  f.settle(40);
  CHECK(take(f.reference.status()).byteOffset == static_cast<std::uint64_t>(5 + height - 1) * 16);
  take(f.reference.goEnd());
  f.settle(30);
  const auto frame = f.frame();
  REQUIRE(frame);
  REQUIRE(frame->anchors.size() == static_cast<std::size_t>(height));
  CHECK(frame->visibleByteEnd == 4099);
  CHECK(frame->finalEndVisible);
  const auto atEnd = take(f.reference.status()).byteOffset;
  take(f.reference.scrollRows(100));
  f.settle();
  CHECK(take(f.reference.status()).byteOffset == atEnd);
  take(f.reference.goHome());
  f.settle();
  CHECK(take(f.reference.status()).byteOffset == 0);
}

TEST_CASE("HEX checked failures preserve mode frame and pending navigation with one collected diagnostic") {
  Fixture f(source(scrollRows(100)));
  f.settle();
  const auto frame = f.frame();
  const auto serial = navigationSerial(f);
  CHECK_FALSE(f.reference.setMode(static_cast<DocumentViewMode>(99)));
  CHECK_FALSE(f.reference.setHexOptions({.bytesPerRow = 0}));
  CHECK(f.frame() == frame);
  CHECK(navigationSerial(f) == serial);
  unsigned diagnostics{};
  take(f.scene->onError([&](const ManagedDiagnostic &) {
    ++diagnostics;
  }));
  CHECK_FALSE(f.reference.mode(static_cast<DocumentViewMode>(99)));
  CHECK(diagnostics == 1);
  CHECK_FALSE(f.reference.hexOptions({.groupSize = MAX_HEX_ROW_BYTES + 1}));
  CHECK(diagnostics == 2);
  take(f.reference.scrollRows(2));
  CHECK_FALSE(f.reference.setMode(static_cast<DocumentViewMode>(99)));
  f.settle();
  CHECK(take(f.reference.status()).byteOffset > 0);
  CHECK(take(f.reference.status()).mode == DocumentViewMode::TEXT);
}

TEST_CASE("forward-only mode and unsupported HEX navigation fail before changing state or reading") {
  auto provider = std::make_shared<Provider>();
  provider->seekable = false;
  provider->enforceSequential = true;
  provider->text = std::string(500, 'F');
  Fixture f(ui::document(provider), {.mode = DocumentViewMode::HEX});
  f.size = {100, 6};
  f.settle();
  const auto frame = f.frame();
  const auto reads = provider->reads.load();
  const auto rejected = f.reference.setMode(DocumentViewMode::TEXT);
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().source == SourceError::UNSUPPORTED);
  const auto end = f.reference.goEnd();
  REQUIRE_FALSE(end);
  CHECK(end.error().source == SourceError::UNSUPPORTED);
  const auto backwards = f.reference.scrollRows(-1);
  REQUIRE_FALSE(backwards);
  CHECK(backwards.error().source == SourceError::UNSUPPORTED);
  CHECK_FALSE(f.reference.setHexOptions({.bytesPerRow = 8}));
  CHECK(f.frame() == frame);
  CHECK(provider->reads.load() == reads);
  CHECK(take(f.reference.status()).mode == DocumentViewMode::HEX);
  take(f.reference.setHexOptions({.uppercase = false, .showAscii = false}));
  f.settle();
  CHECK_FALSE(take(f.reference.status()).error);
  CHECK(provider->reads.load() == reads);
}

TEST_CASE("HEX provider waiting has no idle retries and refresh resumes raw reads") {
  auto provider = std::make_shared<Provider>();
  provider->text = "raw bytes";
  provider->providerWouldBlock = true;
  Fixture f(ui::document(provider), {.mode = DocumentViewMode::HEX});
  f.settle();
  CHECK(take(f.reference.status()).waiting);
  CHECK_FALSE(take(f.reference.status()).finalEnd);
  const auto reads = provider->reads.load();
  f.settle();
  CHECK(provider->reads.load() == reads);
  provider->providerWouldBlock = false;
  take(f.reference.refresh());
  f.settle();
  REQUIRE(f.frame());
  CHECK(f.frame()->status.mode == DocumentViewMode::HEX);
  CHECK(f.frame()->visibleByteEnd == provider->text.size());
}

TEST_CASE("HEX semantic roles paint safe ASCII including space as printable") {
  Fixture f(source(std::string("\0 A\x1B\xFF", 5)), {.mode = DocumentViewMode::HEX});
  f.size = {100, 6};
  take(f.scene->enableStyleLayers());
  take(f.scene->addLayer(
    StyleTier::USER, "hex",
    take(parsePss("DocumentView{role-hex-zero-foreground:#010203;role-hex-printable-foreground:#040506;"
                  "role-hex-control-foreground:#070809;role-hex-high-byte-foreground:#0A0B0C;}"))));
  f.settle();
  const auto sink = paint(f);
  CHECK(sink.part("00").foreground == RgbColor{1, 2, 3});
  CHECK(sink.part("20").foreground == RgbColor{4, 5, 6});
  CHECK(sink.part("1B").foreground == RgbColor{7, 8, 9});
  CHECK(sink.part("FF").foreground == RgbColor{10, 11, 12});
}

TEST_CASE("HEX fixed forward-only rows and resize replay only the retained byte window") {
  auto provider = std::make_shared<Provider>();
  provider->seekable = false;
  provider->enforceSequential = true;
  provider->text = std::string(1000, 'F');
  Fixture f(ui::document(provider), {.mode = DocumentViewMode::HEX});
  f.size = {100, 6};
  f.settle();
  const auto initial = provider->bytesRead.load();
  take(f.reference.scrollRows(1));
  f.settle();
  REQUIRE(f.frame());
  CHECK_FALSE(take(f.reference.status()).error);
  CHECK(take(f.reference.status()).byteOffset == 16);
  CHECK(provider->bytesRead.load() == initial + 16);
  const auto moved = provider->bytesRead.load();
  f.size = {70, 6};
  f.settle();
  CHECK_FALSE(take(f.reference.status()).error);
  CHECK(take(f.reference.status()).byteOffset == 16);
  CHECK(provider->bytesRead.load() == moved);
}

TEST_CASE("HEX horizontal arrow moves a raw column without moving the byte anchor") {
  Fixture f(source(std::string(400, 'H')), {.mode = DocumentViewMode::HEX});
  f.settle();
  REQUIRE(f.frame());
  const std::string first(f.frame()->rows.front().bytes());
  CHECK(f.input(InputKey::RIGHT));
  f.settle();
  CHECK(take(f.reference.status()).byteOffset == 0);
  CHECK(f.frame()->rows.front().bytes() != first);
}

TEST_CASE("forward-only HEX repeated shrink scroll and grow carry charged unread rows") {
  auto provider = std::make_shared<Provider>();
  provider->seekable = false;
  provider->enforceSequential = true;
  provider->text = std::string(1000, 'C');
  auto budget = take(SourceBudget::create());
  Fixture f(ui::document(provider, budget), {.mode = DocumentViewMode::HEX});
  f.size = {100, 8};
  f.settle();
  const auto readAhead = provider->bytesRead.load();
  auto rawCharge = [&] {
    take(f.app->tree().read(f.node, [&](const DocumentView &widget) {
      const auto &state = detail::DocumentViewAccess::state(widget);
      CHECK_FALSE(state.status.error);
    }));
    return budget->usage().bytes;
  };
  CHECK(rawCharge() > 0);
  f.size = {100, 5};
  f.settle();
  CHECK(provider->bytesRead.load() == readAhead);
  take(f.reference.scrollRows(1));
  f.settle();
  CHECK(take(f.reference.status()).byteOffset == 16);
  f.size = {100, 4};
  f.settle();
  take(f.reference.scrollRows(1));
  f.settle();
  CHECK(take(f.reference.status()).byteOffset == 32);
  CHECK(provider->bytesRead.load() == readAhead);
  f.size = {100, 8};
  f.settle();
  REQUIRE(f.frame());
  CHECK_FALSE(take(f.reference.status()).error);
  CHECK(f.frame()->status.visibleByteOffset == 32);
  CHECK(provider->bytesRead.load() == readAhead + 32);
  const auto held = budget->usage().bytes;
  take(f.scene->remove(f.node));
  f.executor.runOne();
  CHECK(budget->usage().bytes < held);
}

TEST_CASE("HEX switch preserves an anchor inside a UTF16 surrogate and recovers strict text failure") {
  DocumentViewConfig config;
  config.decoder.explicitEncoding = DocumentEncoding::UTF16LE;
  Fixture f(source(std::string("\xFF\xFE\x41\0\x3D\xD8\0\xDE\x42\0", 10)), config);
  f.size = {100, 6};
  f.settle();
  take(f.reference.seekByte(5));
  f.settle();
  take(f.reference.setMode(DocumentViewMode::HEX));
  f.settle();
  REQUIRE(f.frame());
  CHECK(f.frame()->status.byteOffset == 5);
  CHECK(f.frame()->rows.front().bytes().starts_with("0000000000000000  FF FE 41 00 3D D8 00 DE"));
  take(f.reference.setMode(DocumentViewMode::TEXT));
  f.settle();
  CHECK(take(f.reference.status()).byteOffset == 5);
  DocumentViewConfig strict;
  strict.decoder.policy = DocumentDecodePolicy::STRICT_MODE;
  Fixture malformed(source(std::string("\xFF\x1B", 2)), strict);
  malformed.size = {100, 6};
  malformed.settle();
  REQUIRE(take(malformed.reference.status()).error);
  take(malformed.reference.setMode(DocumentViewMode::HEX));
  malformed.settle();
  REQUIRE(malformed.frame());
  CHECK_FALSE(take(malformed.reference.status()).error);
  CHECK(malformed.frame()->rows.front().bytes().starts_with("0000000000000000  FF 1B"));
}

TEST_CASE("HEX preparation capacity release wakes the existing activity once") {
  auto budget = take(SourceBudget::create({.maxWork = 2}));
  auto provider = std::make_shared<Provider>();
  provider->text = std::string(512, 'P');
  Fixture f(ui::document(provider, budget), {.mode = DocumentViewMode::HEX});
  f.settle();
  const auto oldFrame = f.frame();
  const auto reads = provider->reads.load();
  auto pressure = take(budget->reserve({0, 0, 1, 0}));
  take(f.reference.refresh());
  f.pump();
  CHECK(take(f.reference.status()).backpressured);
  CHECK(f.frame() == oldFrame);
  CHECK(provider->reads.load() == reads);
  pressure = {};
  f.settle();
  REQUIRE(f.frame());
  CHECK_FALSE(take(f.reference.status()).backpressured);
  CHECK_FALSE(take(f.reference.status()).error);
  CHECK(f.frame()->status.mode == DocumentViewMode::HEX);
  CHECK(provider->reads.load() > reads);
}

TEST_CASE("forward-only HEX queued generation retains replay on resize or case change") {
  for (const bool resize : {false, true}) {
    auto provider = std::make_shared<Provider>();
    provider->seekable = false;
    provider->enforceSequential = true;
    provider->text = std::string(1000, 'F');
    Fixture f(ui::document(provider), {.mode = DocumentViewMode::HEX});
    f.size = {100, 6};
    f.settle();
    take(f.reference.scrollRows(1));
    f.pump();
    REQUIRE(f.executor.job);
    if (resize) {
      f.size = {70, 6};
    } else {
      take(f.reference.setHexOptions({.uppercase = false}));
    }
    f.settle(30);
    CHECK_FALSE(take(f.reference.status()).error);
    CHECK(take(f.reference.status()).byteOffset == 16);
    REQUIRE(f.frame());
    CHECK(f.frame()->status.visibleByteOffset == 16);
  }
}

TEST_CASE("inactive HEX options preserve TEXT work navigation and apply on a later mode change") {
  auto provider = std::make_shared<Provider>();
  provider->text = scrollRows(100);
  Fixture f(ui::document(provider));
  f.settle();
  const auto frame = f.frame();
  const auto serial = navigationSerial(f);
  const auto reads = provider->reads.load();
  const auto submissions = f.executor.submissions;
  take(f.app->tree().consumeFrameRequest());
  take(f.reference.setHexOptions({.bytesPerRow = 8, .uppercase = false}));
  CHECK(navigationSerial(f) == serial);
  CHECK(f.frame() == frame);
  CHECK(provider->reads.load() == reads);
  CHECK(f.executor.submissions == submissions);
  CHECK(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  f.settle();
  CHECK(provider->reads.load() == reads);
  CHECK(f.executor.submissions == submissions);
  take(f.reference.scrollRows(2));
  f.pump();
  REQUIRE(f.executor.job);
  const auto pendingSerial = navigationSerial(f);
  take(f.reference.setHexOptions({.bytesPerRow = 4, .groupSize = 2, .uppercase = false}));
  CHECK(navigationSerial(f) == pendingSerial);
  f.settle();
  CHECK(take(f.reference.status()).byteOffset == std::string("row-0\nrow-1\n").size());
  take(f.reference.setMode(DocumentViewMode::HEX));
  f.settle();
  REQUIRE(f.frame());
  CHECK(f.frame()->hex->columns == *hexRowWidth({4, 2, false, true}));
  take(f.app->tree().read(f.node, [](const DocumentView &widget) {
    const auto &hex = detail::DocumentViewAccess::state(widget).config.hex;
    CHECK(hex.bytesPerRow == 4);
    CHECK(hex.groupSize == 2);
    CHECK_FALSE(hex.uppercase);
  }));
}

TEST_CASE("forward-only HEX completed worker before UI pump preserves one row command on reflow") {
  for (const bool resize : {false, true}) {
    auto provider = std::make_shared<Provider>();
    provider->seekable = false;
    provider->enforceSequential = true;
    provider->text = std::string(1000, 'F');
    Fixture f(ui::document(provider), {.mode = DocumentViewMode::HEX});
    f.size = {100, 6};
    f.settle();
    take(f.reference.scrollRows(1));
    f.pump();
    REQUIRE(f.executor.job);
    f.executor.runOne();
    if (resize) {
      f.size = {70, 6};
    } else {
      take(f.reference.setHexOptions({.uppercase = false}));
    }
    f.settle(30);
    CHECK_FALSE(take(f.reference.status()).error);
    CHECK(take(f.reference.status()).byteOffset == 16);
  }
}

TEST_CASE("forward HEX replay overlap survives temporary capacity pressure and stays bounded") {
  auto budget = take(SourceBudget::create());
  auto provider = std::make_shared<Provider>();
  provider->seekable = false;
  provider->enforceSequential = true;
  provider->text = std::string(10000, 'R');
  Fixture f(ui::document(provider, budget), {.mode = DocumentViewMode::HEX});
  f.size = {100, 6};
  f.settle();
  const auto baseline = budget->usage().bytes;
  const auto height = static_cast<std::size_t>(f.frame()->viewport.height);
  const auto reads = provider->reads.load();
  auto pressure = take(budget->reserve({budget->options().maxBytes - budget->usage().bytes - 1, 0, 0, 0}));
  take(f.reference.scrollRows(1));
  f.pump();
  CHECK(take(f.reference.status()).backpressured);
  CHECK(provider->reads.load() == reads);
  f.size = {70, 6};
  take(f.reference.setHexOptions({.uppercase = false}));
  f.pump();
  CHECK(take(f.reference.status()).backpressured);
  pressure = {};
  f.settle(30);
  CHECK_FALSE(take(f.reference.status()).error);
  CHECK(take(f.reference.status()).byteOffset == 16);
  for (std::uint64_t row = 2; row <= 24; ++row) {
    take(f.reference.scrollRows(1));
    f.pump();
    if (row % 2) {
      f.executor.runOne();
    }
    f.size = {static_cast<std::int32_t>(row % 2 ? 70 : 100), 6};
    f.settle(30);
    CHECK_FALSE(take(f.reference.status()).error);
    CHECK(take(f.reference.status()).byteOffset == row * 16);
  }
  f.size = {100, 6};
  f.settle();
  const auto overlap =
    (2 * height + 1) * (16 + detail::DOCUMENT_HEX_RAW_ROW_BYTES + 2 * sizeof(std::shared_ptr<const void>));
  CHECK(budget->usage().bytes <= baseline + overlap);
}

TEST_CASE("native sized HEX actual wheel and End input remain within default source budget") {
  constexpr std::uint64_t EXTENT = 8 * 1024 * 1024;
  auto provider = std::make_shared<Provider>();
  provider->syntheticExtent = EXTENT;
  Fixture f(ui::document(provider), {.mode = DocumentViewMode::HEX}, false);
  f.size = {192, 52};
  f.settle();
  REQUIRE(f.frame());
  REQUIRE(f.frame()->viewport == CellSize{190, 50});
  REQUIRE(take(f.reference.status()).byteOffset == 0);
  const auto dispatch = [&](const InputEvent &event) {
    const auto handled = take(f.scene->input(*f.app, event));
    if (!handled) {
      take(f.app->router().dispatch(event));
    }
    return handled;
  };
  dispatch(MouseInputEvent{40, 10, MouseButton::LEFT, MouseAction::PRESS});
  dispatch(MouseInputEvent{40, 10, MouseButton::LEFT, MouseAction::RELEASE});
  f.pump();
  CHECK(take(f.app->router().focused()) == f.node);
  CHECK(dispatch(MouseInputEvent{
    .column = 40, .row = 10, .button = MouseButton::WHEEL_DOWN, .action = MouseAction::SCROLL, .wheelDelta = 1.0}));
  f.settle();
  const auto wheel = take(f.reference.status());
  CAPTURE(wheel.error ? static_cast<int>(wheel.error->code) : -1);
  CHECK_FALSE(wheel.error);
  CHECK(wheel.byteOffset == 16);
  CHECK(dispatch(KeyEvent{.key = InputKey::END}));
  f.settle();
  const auto end = take(f.reference.status());
  CAPTURE(end.error ? static_cast<int>(end.error->code) : -1);
  CHECK_FALSE(end.error);
  REQUIRE(f.frame());
  CHECK(end.byteOffset == EXTENT - 50 * 16);
  CHECK(f.frame()->visibleByteEnd == EXTENT);
  CHECK(f.frame()->finalEndVisible);
}

TEST_CASE("document error caption survives deferred rendering and reentrant sinks") {
  auto provider = std::make_shared<Provider>();
  provider->failRead = true;
  Fixture f(ui::document(provider));
  f.size = {80, 8};
  f.settle();
  REQUIRE(take(f.reference.status()).error);

  class DeferredSink final : public PaintContext {
  public:
    std::vector<RenderCommand> commands;
    std::function<void()> action;
    bool acted{};

    std::expected<void, TreeError> submit(std::span<const RenderCommand> incoming) override {
      for (const auto &command : incoming) {
        commands.push_back(command);
        if (const auto *text = std::get_if<TextCommand>(&command);
            text && text->text.bytes().starts_with("Unable to read document.") && !acted) {
          acted = true;
          action();
        }
      }
      return {};
    }
  } sink;

  SUBCASE("switch source") {
    sink.action = [&] {
      take(f.reference.setModel(source("replacement")));
    };
  }
  SUBCASE("remove target") {
    sink.action = [&] {
      const auto removed = f.app->tree().remove(f.node);
      CHECK_FALSE(removed);
      if (!removed) {
        CHECK(removed.error() == TreeError::WOULD_BLOCK);
      }
    };
  }

  take(f.app->tree().frame([&] {
    take(f.app->tree().paint(f.node, sink));
    REQUIRE(sink.acted);
    auto cells = take(renderFullFrame(f.size, sink.commands));
    std::string row;
    for (int column = 0; column < f.size.width; ++column) {
      row += take(cells.at({column, 1})).glyph;
    }
    CHECK(row.find("Unable to read document.") != std::string::npos);
  }));
}

TEST_CASE("document match maps original encoding across wide combining and escaped clusters") {
  auto provider = std::make_shared<Provider>();
  DocumentViewConfig config;
  std::uint64_t begin{};
  std::uint64_t end{};

  SUBCASE("UTF8") {
    provider->text = "A\xE7\x95\x8C"
                     "e\xCC\x81\tB";
    config.decoder.explicitEncoding = DocumentEncoding::UTF8;
    begin = 1;
    end = 8;
  }
  SUBCASE("UTF16LE") {
    provider->text = std::string("\xFF\xFE"
                                 "A\0\x4C\x75"
                                 "e\0\x01\x03\t\0B\0",
                                 14);
    config.decoder.explicitEncoding = DocumentEncoding::UTF16LE;
    begin = 4;
    end = 12;
  }

  Fixture f(ui::document(provider), config);
  f.size = {40, 8};
  f.settle();
  selectDocumentRange(f, begin, end);
  f.settle();

  const auto frame = f.frame();

  REQUIRE(frame);
  REQUIRE(frame->search);
  REQUIRE(frame->search->runs.size() == 1);
  CHECK(frame->rows.front().bytes() == "A\xE7\x95\x8C"
                                       "e\xCC\x81\\u0009B");
  const auto &run = frame->search->runs.front();

  CHECK(run.row == 0);
  CHECK(run.column == 1);
  CHECK(run.first == 1);
  CHECK(run.count == 8);
  CHECK(frame->search->selection.begin == begin);
  CHECK(frame->search->selection.end == end);
  CHECK(frame->search->credit.usage().bytes >= detail::DOCUMENT_SEARCH_FRAME_BYTES);
  CHECK_FALSE(provider->uiOperation.load());
}

TEST_CASE("document match preserves original control and standalone extender boundaries") {
  Fixture f(source("\t\xCC\x81X"));

  f.settle();

  std::size_t first{};
  std::size_t count{};

  SUBCASE("escaped control only") {
    selectDocumentRange(f, 0, 1);
    count = 6;
  }
  SUBCASE("standalone extender only") {
    selectDocumentRange(f, 1, 3);
    first = 6;
    count = 1;
  }

  f.settle();

  const auto frame = f.frame();

  REQUIRE(frame);
  REQUIRE(frame->search);
  REQUIRE(frame->search->runs.size() == 1);
  CHECK(frame->rows.front().bytes() == "\\u0009?X");
  CHECK(frame->search->runs.front().column == static_cast<std::int32_t>(first));
  CHECK(frame->search->runs.front().first == first);
  CHECK(frame->search->runs.front().count == count);
}

TEST_CASE("document match wraps with layout and clear only repaints") {
  auto provider = std::make_shared<Provider>();

  provider->text = "abcdefghij";
  Fixture f(ui::document(provider));

  f.size = {7, 7};
  f.settle();
  selectDocumentRange(f, 3, 8);
  f.settle();

  const auto frame = f.frame();

  REQUIRE(frame);
  REQUIRE(frame->search);
  REQUIRE(frame->search->runs.size() == 2);
  CHECK(frame->search->runs[0].row == 0);
  CHECK(frame->search->runs[0].column == 3);
  CHECK(frame->search->runs[0].count == 2);
  CHECK(frame->search->runs[1].row == 1);
  CHECK(frame->search->runs[1].column == 0);
  CHECK(frame->search->runs[1].count == 3);

  take(f.scene->theme(take(parsePss("DocumentView{role-match-foreground:#010203;role-match-background:#AABBCC;}"))));
  const auto sink = paint(f);

  CHECK(std::count_if(sink.texts.begin(), sink.texts.end(), [](const PartSink::Text &text) {
          return text.style.foreground == RgbColor{1, 2, 3} && text.style.background == RgbColor{0xAA, 0xBB, 0xCC};
        }) == 2);
  const auto reads = provider->reads.load();
  const auto submissions = f.executor.submissions;

  take(detail::setDocumentSearchSelection(*f.scene, f.node, std::nullopt, false));
  const auto cleared = paint(f);

  CHECK(std::none_of(cleared.texts.begin(), cleared.texts.end(), [](const PartSink::Text &text) {
    return text.style.background == RgbColor{0xAA, 0xBB, 0xCC};
  }));
  f.settle(2);
  CHECK(provider->reads.load() == reads);
  CHECK(f.executor.submissions == submissions);
  CHECK_FALSE(provider->uiOperation.load());
}

TEST_CASE("document HEX match covers both digits and ASCII without gutter separators") {
  Fixture f(source("ABCD"), {.mode = DocumentViewMode::HEX});

  f.size = {100, 6};
  f.settle();
  selectDocumentRange(f, 1, 3);
  f.settle();

  const auto frame = f.frame();

  REQUIRE(frame);
  REQUIRE(frame->search);
  std::string selected;

  for (const auto &run : frame->search->runs) {
    const auto graphemes = frame->rows[run.row].graphemes();
    const auto begin = graphemes[run.first].byte_begin;
    const auto end = graphemes[run.first + run.count - 1].byte_end;

    selected.append(frame->rows[run.row].bytes().substr(begin, end - begin));
  }

  CHECK(selected == "42B43C");
}

TEST_CASE("document match retains bounded ASCII tail navigation with an offscreen or final selection") {
  constexpr std::size_t SOURCE_BYTES = 128 * 1024;
  auto provider = std::make_shared<Provider>();

  provider->text.assign(SOURCE_BYTES, 'x');
  Fixture f(ui::document(provider), {.workBytes = 32 * 1024});

  f.size = {20, 8};
  f.settle();

  std::size_t selected{};

  SUBCASE("offscreen match") {
    selectDocumentRange(f, 0, 1);
  }
  SUBCASE("final match") {
    selectDocumentRange(f, SOURCE_BYTES - 3, SOURCE_BYTES);
    selected = 3;
  }

  f.settle();
  take(f.reference.goEnd());
  f.settle(20);

  const auto frame = f.frame();

  REQUIRE(frame);
  REQUIRE(frame->search);
  CHECK(frame->finalEndVisible);
  CHECK(frame->visibleByteEnd == SOURCE_BYTES);
  std::size_t highlighted{};

  for (const auto &run : frame->search->runs) {
    highlighted += run.count;
  }

  CHECK(highlighted == selected);
  CHECK(frame->search->runs.capacity() <=
        static_cast<std::size_t>(frame->viewport.width) * static_cast<std::size_t>(frame->viewport.height));
  CHECK_FALSE(provider->uiOperation.load());
}

TEST_CASE("document match rejects stale selection and snapshots paint before reentrant replacement") {
  Fixture f(source("selected text"));

  f.settle();
  selectDocumentRange(f, 0, 8);
  f.settle();
  take(f.scene->theme(take(parsePss("DocumentView{role-match-background:#ABCDEF;}"))));

  class ReplacingSink final : public PaintContext {
  public:
    Fixture &fixture;
    PartSink captured;
    bool replaced{};

    explicit ReplacingSink(Fixture &value) : fixture(value) {
    }

    std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
      take(captured.submit(commands));
      if (!replaced) {
        replaced = true;
        take(fixture.reference.setModel(source("replacement")));
      }

      return {};
    }
  } sink(f);

  take(f.app->tree().frame([&] {
    take(f.app->tree().paint(f.node, sink));
  }));
  CHECK(sink.replaced);
  CHECK(std::any_of(sink.captured.texts.begin(), sink.captured.texts.end(), [](const PartSink::Text &text) {
    return text.first == 0 && text.count == 8 && text.style.background == RgbColor{0xAB, 0xCD, 0xEF};
  }));
  const auto current = paint(f);

  CHECK(std::none_of(current.texts.begin(), current.texts.end(), [](const PartSink::Text &text) {
    return text.style.background == RgbColor{0xAB, 0xCD, 0xEF};
  }));
  f.settle();
  REQUIRE(f.frame());
  CHECK_FALSE(f.frame()->search);
}

TEST_CASE("document match clears on model decoder and observed provider revision changes") {
  auto provider = std::make_shared<Provider>();

  provider->text = "selected";
  Fixture f(ui::document(provider));

  f.settle();
  selectDocumentRange(f, 0, 8);
  f.settle();
  REQUIRE(f.frame()->search);

  SUBCASE("source namespace") {
    take(f.reference.setModel(source("replacement")));
  }
  SUBCASE("decoder interpretation") {
    take(setDocumentEncoding(*f.scene, f.node, DocumentEncoding::WINDOWS1252));
  }
  SUBCASE("observed revision of the same provider") {
    ++provider->revision;
    take(f.reference.refresh());
    f.settle();
    REQUIRE(f.frame());
    CHECK(f.frame()->status.revision.dataRevision == provider->revision);
  }

  take(f.app->tree().read(f.node, [](const DocumentView &widget) {
    const auto &state = detail::DocumentViewAccess::state(widget);

    REQUIRE(state.search);
    CHECK_FALSE(state.search->selection);
  }));
  f.settle();
  REQUIRE(f.frame());
  CHECK_FALSE(f.frame()->search);
  CHECK_FALSE(provider->uiOperation.load());
}

TEST_CASE("document match role is optional and visible in dark and light themes across mode changes") {
  Fixture f(source("selected"));

  SUBCASE("Nord") {
    take(f.scene->theme(nordTheme()));
  }
  SUBCASE("Catppuccin Latte") {
    take(f.scene->theme(catppuccinLatteTheme()));
  }

  f.settle();
  take(f.app->tree().read(f.node, [](const DocumentView &widget) {
    const auto &state = detail::DocumentViewAccess::state(widget);

    CHECK_FALSE(state.search);
    REQUIRE(state.roles.snapshot());
    CHECK_FALSE(state.roles.snapshot()->find("match"));
  }));
  selectDocumentRange(f, 0, 8);
  f.settle();

  const auto checkMatch = [&] {
    take(f.app->tree().read(f.node, [](const DocumentView &widget) {
      const auto &state = detail::DocumentViewAccess::state(widget);
      const auto match = state.roles.snapshot()->find("match", StyleState::FOCUSED);

      REQUIRE(match);
      CHECK(match->foreground != match->background);
      CHECK(match->bold);
    }));
  };

  checkMatch();
  take(setDocumentLanguage(*f.scene, f.node, DocumentLanguage::POSTGRESQL_LEXICAL));
  checkMatch();
  take(setDocumentMode(*f.scene, f.node, DocumentViewMode::HEX));
  checkMatch();
  f.settle();
  REQUIRE(f.frame());
  REQUIRE(f.frame()->search);
}

TEST_CASE("document error caption counts only against its owning source budget") {
  DocumentViewConfig config;
  CellSize viewport{};
  auto probeProvider = std::make_shared<Provider>();
  probeProvider->failRead = true;
  auto probeBudget = take(SourceBudget::create());
  auto probe = std::make_unique<Fixture>(ui::document(probeProvider, probeBudget));
  probe->settle();
  std::size_t envelope{};
  std::size_t page{};
  std::size_t metadataUsage{};
  std::size_t captionBytes{};
  take(probe->app->tree().read(probe->node, [&](const DocumentView &widget) {
    const auto &state = detail::DocumentViewAccess::state(widget);
    viewport = detail::ScrollWidgetAccess::state(widget).geometry.viewport;
    envelope = state.source->envelopeBytes();
    REQUIRE(state.errorCaption);
    captionBytes = state.errorCaption->credit.usage().bytes;
    metadataUsage = state.source->budget()->usage().bytes - captionBytes;
    page = state.source->maxPageBytes() + SourceArena::METADATA_BYTES;
  }));
  probe.reset();
  const auto work = take(detail::documentPreparationBytes(viewport, config));
  REQUIRE(work >= envelope + page);
  const auto minimum = 2 * work + metadataUsage;
  // One byte below the overlap needed while retaining the error caption.
  const auto limit = minimum + captionBytes - 1;
  auto firstBudget = take(SourceBudget::create({.maxBytes = limit}));
  auto secondBudget = take(SourceBudget::create({.maxBytes = limit}));
  auto provider = std::make_shared<Provider>();
  provider->failRead = true;
  auto descriptor = ui::document(provider, firstBudget);
  auto f = std::make_unique<Fixture>(descriptor);
  f->settle();
  const auto initialStatus = take(f->reference.status());
  INFO("limit=", limit, " work=", work, " envelope=", envelope, " preparing=", initialStatus.preparing,
       " blocked=", initialStatus.backpressured, " submissions=", f->executor.submissions,
       " reads=", provider->reads.load(), " usage=", firstBudget->usage().bytes);
  const auto initialError = initialStatus.error;
  REQUIRE(initialError);
  CHECK(initialError->code == SourceError::IO_ERROR);
  take(f->app->tree().read(f->node, [&](const DocumentView &widget) {
    REQUIRE(detail::DocumentViewAccess::state(widget).errorCaption);
  }));

  provider->failRead = false;
  take(f->reference.refresh());
  f->settle();
  const auto rejected = take(f->reference.status());
  REQUIRE(rejected.error);
  CHECK(rejected.error->code == SourceError::PAGE_TOO_LARGE);
  CHECK_FALSE(rejected.backpressured);

  take(f->reference.setModel(ui::document(std::make_shared<const std::string>("other budget"), secondBudget)));
  f->settle();
  CHECK_FALSE(take(f->reference.status()).error);
  REQUIRE(f->frame());
  CHECK(f->frame()->rows.front().bytes() == "other budget");
  take(f->reference.setModel(descriptor));
  f->settle();
  const auto returned = take(f->reference.status());
  REQUIRE(returned.error);
  CHECK(returned.error->code == SourceError::PAGE_TOO_LARGE);
  CHECK_FALSE(returned.backpressured);
  const auto retained = firstBudget->usage().bytes;
  f.reset();
  CHECK(firstBudget->usage().bytes <= retained - captionBytes);
  CHECK(secondBudget->usage().work == 0);
  CHECK(secondBudget->usage().consumers == 0);
}

TEST_CASE("failed error caption admission preserves the original source error") {
  auto budget = take(SourceBudget::create());
  auto provider = std::make_shared<Provider>();
  provider->failRead = true;
  Fixture f(ui::document(provider, budget));
  f.pump();
  REQUIRE(f.executor.job);
  f.executor.runOne();
  auto pressure = take(budget->reserve({budget->options().maxBytes - budget->usage().bytes, 0, 0, 0}));
  f.pump();
  const auto error = take(f.reference.status()).error;
  REQUIRE(error);
  CHECK(error->code == SourceError::IO_ERROR);
  take(f.app->tree().read(f.node, [&](const DocumentView &widget) {
    CHECK_FALSE(detail::DocumentViewAccess::state(widget).errorCaption);
  }));
  pressure = {};
  provider->failRead = false;
  take(f.reference.refresh());
  f.settle();
  CHECK_FALSE(take(f.reference.status()).error);
  REQUIRE(f.frame());
}

TEST_CASE("syntax uses workers and can be disabled without losing an accepted stream page") {
  auto provider = std::make_shared<Provider>();
  provider->seekable = false;
  provider->enforceSequential = true;
  for (unsigned row = 0; row < 80; ++row) {
    provider->text += "SELECT 42; -- note\n";
  }
  Fixture f(ui::document(provider), {.language = DocumentLanguage::POSTGRESQL_LEXICAL});
  f.settle();
  REQUIRE(f.frame());
  CHECK_FALSE(provider->uiOperation.load());
  CHECK(f.frame()->status.highlighting == DocumentSyntaxState::READY);
  const auto first = std::string(f.frame()->rows[0].bytes());
  const auto bytes = provider->bytesRead.load();
  take(f.reference.setLanguage(DocumentLanguage::PLAIN));
  f.settle();
  REQUIRE(f.frame());
  const auto status = take(f.reference.status());
  CHECK_FALSE(status.error);
  CHECK(status.highlighting == DocumentSyntaxState::DISABLED);
  CHECK(f.frame()->rows[0].bytes() == first);
  CHECK(provider->bytesRead.load() == bytes);
  CHECK_FALSE(f.reference.setLanguage(DocumentLanguage::MYSQL_LEXICAL));
}

TEST_CASE("syntax stream reflow retains UTF32 original bytes rather than only rendered byte count") {
  auto provider = std::make_shared<Provider>();
  provider->seekable = false;
  provider->enforceSequential = true;
  for (unsigned row = 0; row < 40; ++row) {
    for (const auto ch : std::string_view("SELECT 42; -- note\n")) {
      provider->text += ch;
      provider->text.append(3, '\0');
    }
  }
  auto descriptor = ui::document(provider, {}, {.maxPageBytes = 16});
  DocumentViewConfig config;
  config.language = DocumentLanguage::POSTGRESQL_LEXICAL;
  config.maxFrameBytes = 128;
  config.workBytes = 16;
  config.decoder.explicitEncoding = DocumentEncoding::UTF32LE;
  Fixture f(descriptor, config);
  f.size = {20, 5};
  f.settle(120);
  REQUIRE(f.frame());
  REQUIRE_FALSE(take(f.reference.status()).error);
  const auto first = std::string(f.frame()->rows[0].bytes());
  f.size.width = 19;
  f.settle(120);
  REQUIRE_FALSE(take(f.reference.status()).error);
  REQUIRE(f.frame());
  CHECK(f.frame()->rows[0].bytes().starts_with(first.substr(0, 15)));
  CHECK_FALSE(provider->uiOperation.load());
}

TEST_CASE("plain first syntax publication does not block accepted wheel navigation") {
  auto provider = std::make_shared<Provider>();
  for (unsigned row = 0; row < 80; ++row) {
    provider->text += "SELECT 42; -- note\n";
  }
  Fixture f(ui::document(provider), {.language = DocumentLanguage::POSTGRESQL_LEXICAL});
  f.settle();
  REQUIRE(f.frame());
  CHECK(f.frame()->status.highlighting == DocumentSyntaxState::READY);
  const auto before = f.frame()->status.byteOffset;
  CHECK(f.input(InputKey::DOWN));
  f.settle();
  REQUIRE_FALSE(take(f.reference.status()).error);
  REQUIRE(f.frame());
  CHECK(f.frame()->status.byteOffset > before);
  CHECK(f.frame()->status.highlighting == DocumentSyntaxState::READY);
  take(f.reference.setLanguage("md"));
  f.settle();
  CHECK(take(f.reference.status()).language == DocumentLanguage::MARKDOWN_SOURCE);
}

TEST_CASE("syntax ranges use live theme roles and theme changes do not read source") {
  Fixture f(source("SELECT 42; -- comment\n"), {.language = DocumentLanguage::POSTGRESQL_LEXICAL});
  take(f.scene->theme(take(parsePss("DocumentView{role-keyword-foreground:#AABBCC;}"))));
  f.settle();
  REQUIRE(f.frame());
  CHECK(f.frame()->status.highlighting == DocumentSyntaxState::READY);
  const auto reads = f.executor.submissions;
  const auto hasKeyword = [](const PartSink &sink, RgbColor color) {
    return std::ranges::any_of(sink.texts, [&](const PartSink::Text &text) {
      return text.first == 0 && text.count == 6 && text.style.foreground == color;
    });
  };
  CHECK(hasKeyword(paint(f), {0xAA, 0xBB, 0xCC}));
  take(f.scene->theme(take(parsePss("DocumentView{role-keyword-foreground:#112233;}"))));
  f.settle();
  CHECK(hasKeyword(paint(f), {0x11, 0x22, 0x33}));
  CHECK(f.executor.submissions == reads);
}

TEST_CASE("syntax disable drains an already accepted forward-only job before reflow") {
  auto provider = std::make_shared<Provider>();
  provider->seekable = false;
  provider->enforceSequential = true;
  provider->text = "SELECT 1;\nSELECT 2;\nSELECT 3;\nSELECT 4;\n";
  Fixture f(ui::document(provider), {.language = DocumentLanguage::POSTGRESQL_LEXICAL});
  f.pump();
  REQUIRE(f.executor.job);
  f.executor.runOne(); // The provider has accepted bytes; UI has not consumed its completion.
  REQUIRE(provider->bytesRead.load() > 0);
  take(f.reference.setLanguage(DocumentLanguage::PLAIN));
  f.settle();
  REQUIRE(f.frame());
  CHECK_FALSE(take(f.reference.status()).error);
  CHECK(f.frame()->rows[0].bytes() == "SELECT 1;");
  CHECK(take(f.reference.status()).highlighting == DocumentSyntaxState::DISABLED);
}

TEST_CASE("syntax forward navigation can skip a line longer than retained replay history") {
  auto provider = std::make_shared<Provider>();
  provider->seekable = false;
  provider->enforceSequential = true;
  provider->text = "SELECT " + std::string(1600, ' ') + "\nSELECT 42;\nSELECT 43;\nSELECT 44;\nSELECT 45;\n";
  DocumentViewConfig config;
  config.language = DocumentLanguage::POSTGRESQL_LEXICAL;
  config.wrap = false;
  config.maxFrameBytes = 64;
  config.workBytes = 16;
  Fixture f(ui::document(provider, {}, {.maxPageBytes = 16}), config);
  f.size = {20, 3};
  f.settle(60);
  REQUIRE(f.frame());
  REQUIRE_FALSE(take(f.reference.status()).error);
  take(f.reference.scrollRows(1));
  f.settle(250);
  REQUIRE_FALSE(take(f.reference.status()).error);
  REQUIRE(f.frame());
  CHECK(f.frame()->rows[0].bytes() == "SELECT 42;");
}

TEST_CASE("hex retains the selected language without doing lexical work until text mode") {
  Fixture f(source("SELECT 42;\n"), {.mode = DocumentViewMode::HEX, .language = DocumentLanguage::POSTGRESQL_LEXICAL});
  CHECK(take(f.reference.status()).language == DocumentLanguage::POSTGRESQL_LEXICAL);
  f.settle();
  CHECK(take(f.reference.status()).highlighting == DocumentSyntaxState::DISABLED);
  CHECK(take(f.reference.status()).language == DocumentLanguage::POSTGRESQL_LEXICAL);
  const auto submitted = f.executor.submissions;
  take(f.reference.setLanguage("markdown"));
  f.settle();
  CHECK(f.executor.submissions == submitted);
  CHECK(take(f.reference.status()).language == DocumentLanguage::MARKDOWN_SOURCE);
  take(f.reference.setMode(DocumentViewMode::TEXT));
  f.settle();
  CHECK(take(f.reference.status()).language == DocumentLanguage::MARKDOWN_SOURCE);
  CHECK(take(f.reference.status()).highlighting == DocumentSyntaxState::READY);
}

TEST_CASE("optional syntax admission failure preserves the readable plain frame") {
  constexpr std::size_t TIGHT_MEMORY_BUDGET = 1280 * 1024;
  auto budget = take(SourceBudget::create({.maxBytes = TIGHT_MEMORY_BUDGET}));
  auto text = std::make_shared<const std::string>("SELECT 42;\n");
  Fixture f(ui::document(text, budget), {.language = DocumentLanguage::POSTGRESQL_LEXICAL});
  f.settle();
  REQUIRE(f.frame());
  CHECK(f.frame()->rows[0].bytes() == "SELECT 42;");
  const auto status = take(f.reference.status());
  CHECK_FALSE(status.error);
  CHECK(status.highlighting == DocumentSyntaxState::RESOURCE_LIMIT);
  CHECK_FALSE(status.preparing);
}

TEST_CASE("language changes preserve already accepted relative navigation") {
  auto provider = std::make_shared<Provider>();
  for (unsigned row = 0; row < 40; ++row) {
    provider->text += "SELECT 42;\n";
  }
  Fixture f(ui::document(provider), {.language = DocumentLanguage::POSTGRESQL_LEXICAL});
  f.settle();
  const auto before = take(f.reference.status()).byteOffset;
  take(f.reference.scrollRows(2));
  take(f.reference.setLanguage("markdown"));
  f.settle();
  const auto status = take(f.reference.status());
  CHECK_FALSE(status.error);
  CHECK(status.byteOffset > before);
  CHECK(status.language == DocumentLanguage::MARKDOWN_SOURCE);
  CHECK(status.highlighting == DocumentSyntaxState::READY);
}

TEST_CASE("syntax policy commands preserve queued navigation and reject invalid policies") {
  auto provider = std::make_shared<Provider>();
  for (unsigned row = 0; row < 40; ++row) {
    provider->text += "SELECT 42;\n";
  }
  Fixture f(ui::document(provider), {.language = DocumentLanguage::MYSQL_LEXICAL});
  f.settle();
  CHECK(take(f.reference.status()).syntax == syntax::AUTO);
  const auto submitted = f.executor.submissions;
  take(f.reference.setSyntax(syntax::AUTO));
  f.settle();
  CHECK(f.executor.submissions == submitted);
  const auto before = take(f.reference.status()).byteOffset;
  take(f.reference.scrollRows(2));
  CHECK(f.reference.syntax(syntax::EXACT));
  f.settle();
  const auto status = take(f.reference.status());
  CHECK(status.byteOffset > before);
  CHECK(status.syntax == syntax::EXACT);
  CHECK(status.syntaxAccuracy == DocumentSyntaxAccuracy::EXACT);
  CHECK_FALSE(status.error);
  const auto invalid = f.reference.setSyntax(static_cast<DocumentSyntaxPolicy>(99));
  CHECK_FALSE(invalid);
  CHECK(take(f.reference.status()).syntax == syntax::EXACT);
}

TEST_CASE("syntax policy changes drain an accepted forward-only turn without losing bytes") {
  auto provider = std::make_shared<Provider>();
  provider->seekable = false;
  provider->enforceSequential = true;
  provider->text = "SELECT 1;\nSELECT 2;\nSELECT 3;\nSELECT 4;\n";
  Fixture f(ui::document(provider), {.language = DocumentLanguage::MYSQL_LEXICAL});
  f.pump();
  REQUIRE(f.executor.job);
  f.executor.runOne();
  REQUIRE(provider->bytesRead.load() > 0);
  take(f.reference.setSyntax(syntax::EXACT));
  f.settle();
  REQUIRE(f.frame());
  CHECK(f.frame()->rows[0].bytes() == "SELECT 1;");
  const auto status = take(f.reference.status());
  CHECK_FALSE(status.error);
  CHECK(status.syntax == syntax::EXACT);
  CHECK(status.syntaxAccuracy == DocumentSyntaxAccuracy::EXACT);
}

TEST_CASE("AUTO preview survives paint and policy races without stale exact publication") {
  auto provider = std::make_shared<Provider>();
  provider->text = "/*" + std::string(2 * 1024 * 1024, 'a') + "*/\nSELECT 42;\n";
  DocumentViewConfig config;
  config.language = DocumentLanguage::MYSQL_LEXICAL;
  config.maxAutoHighlightBytes = 17;
  Fixture f(ui::document(provider), config);
  f.settle();
  take(f.reference.seekByte(1024 * 1024));
  f.settle();
  REQUIRE(f.frame());
  auto preview = f.frame();
  REQUIRE(preview->text);
  CHECK(take(f.reference.status()).syntaxAccuracy == DocumentSyntaxAccuracy::PREVIEW);
  CHECK(take(f.reference.status()).highlighting == DocumentSyntaxState::PREVIEW);
  CHECK(preview->footer->bytes().find('~') != std::string_view::npos);
  const auto anchors = preview->anchors;
  const auto submissions = f.executor.submissions;
  const auto bytesRead = provider->bytesRead.load();
  take(f.scene->theme(take(parsePss("DocumentView{role-keyword-foreground:#AABBCC;}"))));
  f.settle();
  CHECK(f.executor.submissions == submissions);
  CHECK(provider->bytesRead.load() == bytesRead);
  take(f.reference.setSyntax(syntax::EXACT));
  f.pump();
  REQUIRE(f.executor.job);
  f.executor.runOne();
  take(f.reference.setSyntax(syntax::PREVIEW));
  f.settle();
  CHECK(take(f.reference.status()).syntax == syntax::PREVIEW);
  CHECK(take(f.reference.status()).syntaxAccuracy == DocumentSyntaxAccuracy::PREVIEW);
  CHECK(f.frame()->anchors == anchors);
  CHECK(preview->status.syntaxAccuracy == DocumentSyntaxAccuracy::PREVIEW);
  take(f.reference.setSyntax(syntax::EXACT));
  f.settle(100);
  REQUIRE(f.frame()->text);
  REQUIRE_FALSE(f.frame()->text->rows[0].empty());
  CHECK(f.frame()->text->rows[0][0].role == SemanticRole::COMMENT);
  CHECK(take(f.reference.status()).syntaxAccuracy == DocumentSyntaxAccuracy::EXACT);
  CHECK(f.frame()->anchors == anchors);
  CHECK(f.frame()->footer->bytes().find('~') == std::string_view::npos);
}

TEST_CASE("provider pinned storage rejects impossible view overlap instead of waiting forever") {
  auto budget = take(SourceBudget::create());
  auto provider = std::make_shared<Provider>();
  provider->text = "bounded";
  provider->accountedRoot = budget;
  Fixture f(ui::document(provider, budget));
  const auto free = budget->options().maxBytes - budget->usage().bytes;
  provider->pinnedCredit = take(budget->reserve({free - 1, 0, 0, 0}));

  f.settle(3);
  const auto status = take(f.reference.status());

  REQUIRE(status.error);
  CHECK(status.error->code == SourceError::PAGE_TOO_LARGE);
  CHECK_FALSE(status.backpressured);
  CHECK_FALSE(f.executor.job);
}

TEST_CASE("optional provider cache is reclaimed before view capacity waiting") {
  auto budget = take(SourceBudget::create());
  auto provider = std::make_shared<Provider>();
  provider->text = "cache reclaimed";
  Fixture f(ui::document(provider, budget));
  const auto free = budget->options().maxBytes - budget->usage().bytes;
  provider->optionalCache = take(budget->reserve({free, 0, 0, 0}));

  f.settle(3);

  REQUIRE(f.frame());
  CHECK(f.frame()->rows.front().bytes() == "cache reclaimed");
  CHECK(provider->cacheTrims == 1);
  CHECK_FALSE(take(f.reference.status()).backpressured);
  CHECK_FALSE(provider->uiOperation.load());
}

TEST_CASE("nested provider retry peak rejects an impossible decoder turn without resubmission") {
  auto budget = take(SourceBudget::create());
  auto provider = std::make_shared<Provider>();
  provider->text = "unread";
  DocumentError blocked{SourceError::WOULD_BLOCK, {}, {}};
  blocked.budgetDemand = SourceBudgetUsage{1, 0, 0, 0};
  blocked.budgetPeak = SourceBudgetUsage{budget->options().maxBytes, 0, 0, 0};
  provider->budgetFailure = blocked;
  Fixture f(ui::document(provider, budget));

  f.settle(3);
  const auto status = take(f.reference.status());

  REQUIRE(status.error);
  CHECK(status.error->code == SourceError::PAGE_TOO_LARGE);
  CHECK_FALSE(status.backpressured);
  CHECK(provider->reads.load() == 1);
  CHECK_FALSE(f.executor.job);
}

TEST_CASE("End discovers unknown seekable extent before bounded last-page layout") {
  for (const auto mode : {DocumentViewMode::TEXT, DocumentViewMode::HEX}) {
    CAPTURE(mode);
    constexpr std::uint64_t EXTENT = std::uint64_t{8} << 30;
    auto provider = std::make_shared<Provider>();
    provider->syntheticExtent = EXTENT;
    provider->unknown = true;
    provider->discoverable = true;
    DocumentViewConfig config;
    config.mode = mode;
    Fixture f(ui::document(provider), config);
    f.settle(3);
    REQUIRE(f.frame());
    CHECK(provider->discoveries == 0);
    CHECK_FALSE(f.frame()->status.byteExtent);

    REQUIRE(f.reference.end());
    f.settle(4);

    REQUIRE(f.frame());
    CHECK(f.frame()->status.byteExtent == EXTENT);
    CHECK(f.frame()->finalEndVisible);
    const auto rowCount = f.frame()->rows.size();
    take(f.app->tree().read(f.node, [&](const DocumentView &widget) {
      const auto height = detail::ScrollWidgetAccess::state(widget).geometry.viewport.height;
      CHECK(rowCount == static_cast<std::size_t>(height));
    }));
    CHECK(f.frame()->footer->bytes().find("100.0%") != std::string_view::npos);
    CHECK(provider->discoveries > 0);
    CHECK(provider->bytesRead.load() <= 512 * 1024);
    CHECK_FALSE(provider->uiOperation.load());
  }
}
