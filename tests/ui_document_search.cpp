// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../src/widgets/document_find_binding.hpp"
#include "../src/widgets/document_view_state.hpp"
#include "../src/widgets/external_log_view.hpp"
#include "../src/widgets/log_view_state.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <doctest.h>
#include <filesystem>
#include <fstream>
#include <limits>
#include <portals/ui.hpp>
#include <portals/ui_document_search.hpp>
#include <source_location>
#include <thread>

namespace {
using namespace portals;

template <class T, class E>
T take(std::expected<T, E> value, std::source_location origin = std::source_location::current()) {
  INFO("checked call at ", origin.file_name(), ":", origin.line());

  if (!value) {
    if constexpr (std::same_as<E, ManagedError>) {
      INFO("managed phase ", static_cast<int>(value.error().phase), " source ",
           value.error().source ? static_cast<int>(*value.error().source) : -1, " tree ",
           std::get_if<TreeError>(&value.error().cause) ? static_cast<int>(std::get<TreeError>(value.error().cause))
                                                        : -1);
      REQUIRE(value);
    } else if constexpr (std::is_enum_v<E>) {
      INFO("typed cause ", static_cast<int>(value.error()));
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

  std::expected<BackgroundTicket, BackgroundError> submit(std::unique_ptr<BackgroundJob> &input) override {
    if (job) {
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
    CHECK((published || published.error() == AsyncError::CANCELLED || published.error() == AsyncError::CLOSED));
    ++epoch;
  }
};

ui::DocumentSource document(std::string text) {
  return ui::document(std::make_shared<const std::string>(std::move(text)));
}

class CapacityProvider final : public ByteDocumentProvider {
public:
  std::shared_ptr<SourceBudget> budget;
  std::string text;
  SourceCredit held;
  unsigned holdAtMetadata{};
  unsigned metadataCalls{};
  bool holdFailed{};

  ByteDocumentCapabilities capabilities() const noexcept override {
    return {};
  }

  std::expected<ByteDocumentMetadata, DocumentError> metadata(std::stop_token) override {
    ++metadataCalls;

    if (holdAtMetadata && !--holdAtMetadata) {
      constexpr std::size_t CONVERSION_HEADROOM = 8 * 1024;
      const auto free = budget->options().maxBytes - budget->usage().bytes;
      auto reserved = budget->reserve({free > CONVERSION_HEADROOM ? free - CONVERSION_HEADROOM : 0, 0, 0, 0});
      holdFailed = !reserved;

      if (reserved) {
        held = std::move(*reserved);
      }
    }

    return ByteDocumentMetadata{1, text.size()};
  }

  std::expected<ByteReadResult, DocumentError> read(const ByteReadCommand &request, std::span<std::byte> destination,
                                                    std::stop_token) override {
    const auto first = static_cast<std::size_t>(std::min<std::uint64_t>(request.offset, text.size()));
    const auto count = std::min(destination.size(), text.size() - first);
    std::memcpy(destination.data(), text.data() + first, count);

    return ByteReadResult{count, first + count == text.size()};
  }
};

class SearchTarget final : public ui::detail::DocumentFindTarget {
public:
  std::shared_ptr<ByteDocumentSource> source;
  std::function<std::expected<ui::detail::DocumentFindLocation, DocumentError>(const DocumentSearchMatch &,
                                                                               std::stop_token)>
    locate;

  std::expected<std::optional<ui::detail::DocumentFindSnapshot>, TreeError> capture(ManagedScene &) override {
    ui::detail::DocumentFindSnapshot snapshot;
    snapshot.source = source;
    snapshot.decoder.hint = DocumentEncoding::UTF8;
    snapshot.locate = locate;

    return std::optional<ui::detail::DocumentFindSnapshot>(std::move(snapshot));
  }

  std::expected<void, ManagedError> select(ManagedScene &, const ui::detail::DocumentFindSnapshot &,
                                           std::optional<ui::detail::DocumentFindSelection>, bool) override {
    return {};
  }
};

struct Fixture {
  Executor executor;
  std::unique_ptr<Application> app = take(Application::create({.backgroundExecutor = &executor}));
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));
  ui::Ref<DocumentView> viewer;
  ui::Ref<FindBar> find;
  ui::Ref<LogView> log;
  std::vector<FindMatch> callbacks;
  CellSize size{60, 12};

  Fixture(std::string text, std::string_view query, DocumentSearchOptions search = {}) {
    take(ui::mount(*scene, ui::vbox(ui::findBar(find, viewer, query,
                                                {.selected =
                                                   [this](FindMatch match) {
                                                     callbacks.push_back(std::move(match));
                                                   }},
                                                search),
                                    ui::viewer(viewer, document(std::move(text))))));
  }

  Fixture(ui::LogSource source, std::string_view query, DocumentSearchOptions search = {}) {
    take(ui::mount(*scene, ui::vbox(ui::findBar(find, log, query, {}, search),
                                    ui::logView(log, std::move(source), {.followTail = false}))));
    INFO("published log node session ", log.node().id().session, " slot ", log.node().id().slot);
    REQUIRE(log.node().id().session != 0);
  }

  FindMatch requiredLogMatch() {
    const auto diagnostics = take(scene->diagnostics());
    INFO("log node session ", log.node().id().session, " slot ", log.node().id().slot, " generation ",
         log.node().id().generation, " diagnostics ", diagnostics.errors.size());

    if (!diagnostics.errors.empty()) {
      const auto &error = diagnostics.errors.front().error;
      INFO("first diagnostic phase ", static_cast<int>(error.phase), " source ",
           error.source ? static_cast<int>(*error.source) : -1, " tree ",
           std::get_if<TreeError>(&error.cause) ? static_cast<int>(std::get<TreeError>(error.cause)) : -1);
      REQUIRE(diagnostics.errors.empty());
    }

    auto match = take(find.match());
    REQUIRE(match);

    return match;
  }

  std::shared_ptr<const detail::ExternalLogFrame> logFrame() {
    std::shared_ptr<const detail::ExternalLogFrame> result;
    take(app->tree().read(log.node(), [&](const LogView &view) {
      const auto &state = detail::LogViewAccess::state(view).external;
      result = state ? state->frame : nullptr;
    }));

    return result;
  }

  Fixture(std::shared_ptr<ui::detail::DocumentFindTarget> target, std::string_view query,
          DocumentSearchOptions search = {}) {
    take(ui::mount(*scene, ui::detail::documentFindBar(std::move(target), query, {}, search).reference(find)));
  }

  ~Fixture() {
    scene.reset();
    executor.runOne();
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

  void settle(unsigned turns = 48) {
    for (unsigned turn = 0; turn < turns; ++turn) {
      pump();
      executor.runOne();
    }

    pump();
  }

  std::uint64_t anchor() {
    std::uint64_t result{};
    take(app->tree().read(viewer.node(), [&](const DocumentView &view) {
      result = detail::DocumentViewAccess::state(view).anchor;
    }));

    return result;
  }
};

struct TemporaryLog {
  std::filesystem::path path =
    std::filesystem::temp_directory_path() /
    ("portals-document-search-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");

  ~TemporaryLog() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }

  void write(std::string_view bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(stream.good());
  }

  std::string utf8() const {
    const auto bytes = path.u8string();

    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
  }
};
} // namespace

TEST_CASE("streaming FindBar mounts before viewer and navigates all overlapping matches") {
  Fixture f("aaaa\n", "aa", {.workBytes = 2});
  f.settle();

  auto first = take(f.find.match());
  REQUIRE(first);
  CHECK(first.sourceByteOffset() == 0);
  CHECK(first.sourceByteLength() == 2);
  CHECK(first.text() == "aa");
  CHECK(first.storedCount() == 1);

  take(f.app->tree().consumeFrameRequest());
  CHECK(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  take(f.find.navigate(FindDirection::NEXT));
  CHECK(take(f.app->tree().consumeFrameRequest()) == Invalidation::LAYOUT);
  auto busy = f.find.navigate(FindDirection::NEXT);
  CHECK_FALSE(busy);
  f.settle();
  CHECK(take(f.find.match()).sourceByteOffset() == 1);

  take(f.find.navigate(FindDirection::NEXT));
  f.settle();
  CHECK(take(f.find.match()).sourceByteOffset() == 2);

  take(f.find.navigate(FindDirection::NEXT));
  f.settle();
  CHECK(take(f.find.match()).sourceByteOffset() == 0);

  take(f.app->tree().consumeFrameRequest());
  take(f.find.navigate(FindDirection::PREVIOUS));
  CHECK(take(f.app->tree().consumeFrameRequest()) == Invalidation::LAYOUT);
  f.settle();
  CHECK(take(f.find.match()).sourceByteOffset() == 2);
  CHECK(first.text() == "aa");
  CHECK_FALSE(take(f.find.current(first)));
}

TEST_CASE("streaming FindBar model replacement retains draft without automatic search or jump") {
  Fixture f("old needle\n", "needle");
  f.settle();

  auto old = take(f.find.match());
  REQUIRE(old);
  take(f.viewer.setModel(document("new prefix needle\n")));
  f.settle();
  CHECK_FALSE(take(f.find.match()));
  CHECK(take(f.find.text().get()) == "needle");
  CHECK(f.anchor() == 0);
  CHECK(old.text() == "needle");

  take(f.find.navigate(FindDirection::NEXT));
  f.settle();
  CHECK(take(f.find.match()).sourceByteOffset() == 11);
}

TEST_CASE("manual movement during bounded search suppresses automatic reveal across continuation quanta") {
  std::string text;

  for (unsigned row = 0; row < 64; ++row) {
    text += "plain row\n";
  }

  text += "needle\n";
  Fixture f(std::move(text), {}, {.workBytes = 8});
  f.settle();
  take((f.find.text() = "needle").result());
  f.pump();
  f.executor.runOne();
  take(f.viewer.seekByte(20));
  f.settle(240);

  REQUIRE(take(f.find.match()));
  CHECK(take(f.find.match()).sourceByteOffset() == 640);
  CHECK(f.anchor() == 20);
}

TEST_CASE("query replacement discards old worker completion and empty query clears selection") {
  Fixture f("first second first\n", {}, {.workBytes = 4});
  f.settle();
  take((f.find.text() = "first").result());
  f.pump();
  take((f.find.text() = "second").result());
  f.settle();

  REQUIRE(take(f.find.match()));
  CHECK(take(f.find.match()).text() == "second");
  CHECK(take(f.find.match()).sourceByteOffset() == 6);
  take((f.find.text() = "").result());
  f.settle();
  CHECK_FALSE(take(f.find.match()));

  take(f.app->tree().read(f.viewer.node(), [&](const DocumentView &view) {
    const auto &state = detail::DocumentViewAccess::state(view);
    CHECK((!state.search || !state.search->selection));
  }));
}

TEST_CASE("shared-budget backpressure resumes streaming search after an independent pin is released") {
  Fixture f("prefix needle suffix\n", {});
  f.settle();
  std::shared_ptr<SourceBudget> budget;
  take(f.app->tree().read(f.viewer.node(), [&](const DocumentView &view) {
    budget = detail::DocumentViewAccess::state(view).source->budget();
  }));

  auto held = take(budget->reserve({budget->options().maxBytes - budget->usage().bytes, 0, 0, 0}));
  take((f.find.text() = "needle").result());
  f.settle(12);
  CHECK_FALSE(take(f.find.match()));
  held = {};
  f.settle();

  REQUIRE(take(f.find.match()));
  CHECK(take(f.find.match()).sourceByteOffset() == 7);
}

TEST_CASE("file-backed FindBar proves record start while retaining exact bytes inside a long wrapped record") {
  TemporaryLog file;
  file.write("first\r\n" + std::string(4096, 'x') + "needle tail\r\nlast\r\n");
  Fixture f(ui::logFile(file.utf8()), "needle");
  f.settle(120);

  auto match = f.requiredLogMatch();
  CHECK(match.sourceByteOffset() == 4103);
  CHECK(match.sourceByteLength() == 6);
  CHECK(match.text() == "needle");
  const auto frame = f.logFrame();
  REQUIRE(frame);
  CHECK(std::any_of(frame->rows.begin(), frame->rows.end(), [](const auto &row) {
    return row->bytes().find("needle") != std::string_view::npos;
  }));
  CHECK(take(f.scene->diagnostics()).errors.empty());
}

TEST_CASE("file-backed FindBar navigates complete records separated by CRLF") {
  TemporaryLog file;
  file.write("alpha\r\nbeta\r\nbeta gamma\r\n");
  Fixture f(ui::logFile(file.utf8()), "beta");
  f.settle();

  auto match = f.requiredLogMatch();
  CHECK(match.sourceByteOffset() == 7);
  CHECK(match.sourceByteLength() == 4);
  take(f.find.navigate(FindDirection::NEXT));
  f.settle();
  CHECK(take(f.find.match()).sourceByteOffset() == 13);
  CHECK(take(f.scene->diagnostics()).errors.empty());
}

TEST_CASE("file replacement before search completion rejects old result and keeps query for explicit navigation") {
  TemporaryLog oldFile;
  TemporaryLog newFile;
  oldFile.write("old needle\n");
  newFile.write("new prefix needle\n");
  Fixture f(ui::logFile(oldFile.utf8()), {}, {.workBytes = 2});
  f.settle();
  take((f.find.text() = "needle").result());
  f.pump();
  take(f.log.setModel(ui::logFile(newFile.utf8())));
  f.settle();

  CHECK_FALSE(take(f.find.match()));
  CHECK(take(f.find.text().get()) == "needle");
  take(f.find.navigate(FindDirection::NEXT));
  f.settle();
  REQUIRE(take(f.find.match()));
  CHECK(take(f.find.match()).sourceByteOffset() == 11);
  CHECK(take(f.scene->diagnostics()).errors.empty());
}

TEST_CASE("post-match conversion and retry-inspect backpressure retain the unpublished hit across navigation") {
  auto budget = take(SourceBudget::create());
  auto provider = std::make_shared<CapacityProvider>();
  provider->budget = budget;
  provider->text = std::string(1000, 'x') + "\nneedle\nneedle\n";
  auto target = std::make_shared<SearchTarget>();
  target->source = take(ByteDocumentSource::create(provider, budget));
  Fixture f(target, {});
  f.settle();
  provider->holdAtMetadata = 3; // Initial inspect, admitted-page guard, then the owning match guard.
  take((f.find.text() = "needle").result());
  f.pump();
  f.executor.runOne();
  REQUIRE_FALSE(provider->holdFailed);
  REQUIRE(provider->held.usage().bytes != 0);
  REQUIRE(provider->metadataCalls == 3);
  const auto beforeRetry = provider->metadataCalls;
  f.pump();
  CHECK_FALSE(take(f.find.match()));

  provider->held = {};
  f.pump();
  REQUIRE(f.executor.job);
  auto retryBlocker = take(budget->reserve({budget->options().maxBytes - budget->usage().bytes, 0, 0, 0}));
  f.executor.runOne();
  f.pump();
  CHECK(provider->metadataCalls == beforeRetry); // The retry refused its envelope before calling the provider.
  CHECK_FALSE(take(f.find.match()));
  retryBlocker = {};
  f.settle();

  auto first = take(f.find.match());
  REQUIRE(first);
  CHECK(first.sourceByteOffset() == 1001);
  take(f.find.navigate(FindDirection::NEXT));
  f.settle();
  REQUIRE(take(f.find.match()));
  CHECK(take(f.find.match()).sourceByteOffset() == 1008);
  take(f.find.navigate(FindDirection::PREVIOUS));
  f.settle();
  CHECK(take(f.find.match()).sourceByteOffset() == 1001);
  CHECK(take(f.scene->diagnostics()).errors.empty());
}

TEST_CASE("locator retry rejects impossible cursor-match-preview overlap instead of waiting on its own owners") {
  auto budget = take(SourceBudget::create({.maxBytes = 256 * 1024}));
  auto target = std::make_shared<SearchTarget>();
  target->source = take(ByteDocumentSource::create(
    std::make_shared<MemoryDocumentProvider>(std::make_shared<const std::string>(std::string(300, 'x') + "needle")),
    budget));
  unsigned locatorCalls{};
  target->locate = [budget,
                    &locatorCalls](const DocumentSearchMatch &match,
                                   std::stop_token) -> std::expected<ui::detail::DocumentFindLocation, DocumentError> {
    ++locatorCalls;
    constexpr std::size_t UNAVAILABLE_OVERLAP = 32 * 1024;
    const SourceBudgetUsage demand{budget->options().maxBytes - UNAVAILABLE_OVERLAP, 0, 1, 0};
    auto reserved = budget->reserve(demand);

    if (!reserved) {
      auto error = DocumentError{reserved.error(), {}, {}};
      error.budgetDemand = demand;

      return std::unexpected(error);
    }

    return ui::detail::DocumentFindLocation{match.byteOffset, {}};
  };
  Fixture f(target, "needle");
  f.settle();

  const auto diagnostics = take(f.scene->diagnostics());
  REQUIRE(diagnostics.errors.size() == 1);
  const auto &error = diagnostics.errors.front().error;
  CHECK(error.source == SourceError::PAGE_TOO_LARGE);
  REQUIRE(error.document);
  REQUIRE(error.document->error.budgetPeak);
  CHECK(error.document->error.budgetPeak->bytes > budget->options().maxBytes);
  CHECK(locatorCalls == 1);
  CHECK_FALSE(take(f.find.match()));
}

TEST_CASE("explicit file-log seek while search is pending keeps the chosen reading position") {
  TemporaryLog file;
  std::string contents = "needle start\n";

  for (unsigned row = 1; row < 100; ++row) {
    contents += "record " + std::to_string(row) + "\n";
  }

  file.write(contents);
  Fixture f(ui::logFile(file.utf8()), {});
  f.settle();
  take((f.find.text() = "needle").result());
  f.pump();
  REQUIRE(f.executor.job); // Search captured the old reading intent; its worker has not run yet.

  const auto requested = contents.find("record 60\n");
  REQUIRE(requested != std::string::npos);
  take(seekLogByte(f.app->tree(), f.log.node(), requested));
  f.executor.runOne();
  f.settle();

  CHECK(f.requiredLogMatch().sourceByteOffset() == 0);
  const auto frame = f.logFrame();
  REQUIRE(frame);
  REQUIRE_FALSE(frame->rows.empty());
  CHECK(frame->rows.front()->bytes() == "record 60");
  CHECK_FALSE(frame->following);
  CHECK(take(f.scene->diagnostics()).errors.empty());

  take(f.app->tree().edit(f.log.node(), [&](LogView &widget) {
    const auto intent = detail::ScrollWidgetAccess::state(widget).intent.lock();
    REQUIRE(intent);
    intent->serial = std::numeric_limits<std::uint64_t>::max();
  }));
  const auto exhausted = seekLogByte(f.app->tree(), f.log.node(), 0);
  REQUIRE_FALSE(exhausted);
  CHECK(exhausted.error() == SourceError::IDENTITY_EXHAUSTED);
  take(f.app->tree().read(f.log.node(), [&](const LogView &widget) {
    const auto &state = detail::LogViewAccess::state(widget).external;
    REQUIRE(state);
    CHECK_FALSE(state->byteRequest);
    const auto intent = detail::ScrollWidgetAccess::state(widget).intent.lock();
    REQUIRE(intent);
    CHECK(intent->serial == std::numeric_limits<std::uint64_t>::max());
  }));
}
