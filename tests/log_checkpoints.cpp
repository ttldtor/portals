// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <portals/log_file.hpp>
#include <portals/ui.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <future>

namespace {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> value) {
  REQUIRE(value);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

struct TemporaryFile {
  std::filesystem::path path =
    std::filesystem::temp_directory_path() /
    ("portals-checkpoint-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");

  explicit TemporaryFile(std::string_view text) {
    write(text);
  }

  ~TemporaryFile() {
    std::error_code ignored;

    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".old", ignored);
  }

  void write(std::string_view text, bool append = false) {
    std::ofstream output(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));

    REQUIRE(output);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(output.good());
  }
};

struct CaptureFixture {
  std::unique_ptr<Scheduler> scheduler = take(Scheduler::create());
  std::unique_ptr<WidgetTree> tree = take(WidgetTree::create(*scheduler));
  std::shared_ptr<FileLogModel> model;
  std::unique_ptr<WindowSource<LogLine>> source;
  WindowConsumer<LogLine> consumer;
  SourceReadCommand command;
  SourceReadReply<LogLine> page;

  explicit CaptureFixture(const std::filesystem::path &path) {
    model = take(FileLogModel::create(path));
    source = take(WindowSource<LogLine>::create(*tree, take(tree->root()), model));
    consumer = take(source->openConsumer());
    auto request = take(consumer.request({}, SourceDirection::FORWARD, 8));

    command = request.command();
    auto result = std::async(std::launch::async, [request] {
                    return request.load();
                  }).get();

    page = take(result);
    take(request.complete(std::move(result)));
    take(scheduler->dispatch());
  }

  ModelBookmark capture(std::size_t row = 0) {
    return take(std::async(std::launch::async, [&] {
                  return model->capture(command, page.page.rows()[row], {});
                }).get());
  }

  PositionReply resolve(const ModelBookmark &bookmark, std::shared_ptr<FileLogModel> provider = {}) {
    PositionRequest request{source->stamp(), command.dataRevision, 19, bookmark};
    auto result = std::async(std::launch::async, [&] {
                    return (provider ? provider : model)->resolve(request);
                  }).get();
    auto reply = take(std::move(result));

    CHECK(positionReplyMatches(request, reply));

    return reply;
  }
};
} // namespace

TEST_CASE("native bookmarks preserve a paused record through append") {
  TemporaryFile file("first\nsecond\nthird\n");
  CaptureFixture fixture(file.path);
  const auto bookmark = fixture.capture(1);

  file.write("fourth\n", true);
  const auto reply = fixture.resolve(bookmark);

  CHECK(reply.state == PositionResolution::EXACT);
  CHECK(reply.position.row == bookmark.position().row);
  CHECK(reply.position.offset == 6);
  CHECK(bookmark.storageBytes() <= BookmarkOptions{}.maxBytes);
}

TEST_CASE("explicit logical continuation still validates the native file and bounded witness") {
  TemporaryFile file("first\nsecond\n");
  CaptureFixture fixture(file.path);
  const auto bookmark = fixture.capture(1);
  auto independent = take(FileLogModel::create(file.path));
  auto continuation = take(FileLogModel::create(file.path, {}, bookmark.resource()));

  CHECK(fixture.resolve(bookmark, independent).state == PositionResolution::INCOMPATIBLE);
  CHECK(fixture.resolve(bookmark, continuation).state == PositionResolution::EXACT);
  std::filesystem::rename(file.path, file.path.string() + ".old");
  file.write("first\nsecond\n");

  CHECK(fixture.resolve(bookmark, continuation).state == PositionResolution::INCOMPATIBLE);
}

TEST_CASE("observed truncation and a changed anchor never resolve to a guessed record") {
  TemporaryFile file("first\nsecond\nthird\n");
  CaptureFixture fixture(file.path);
  const auto bookmark = fixture.capture(1);

  file.write("x\n");

  CHECK(fixture.resolve(bookmark).state == PositionResolution::UNAVAILABLE);

  TemporaryFile edited("first\nsecond\nthird\n");
  CaptureFixture editing(edited.path);
  const auto original = editing.capture(1);

  edited.write("first\nCHANGED\nthird\nlonger tail\n");
  PositionRequest request{editing.source->stamp(), 1, 7, original};
  auto reply = std::async(std::launch::async, [&] {
                 return editing.model->resolve(request);
               }).get();

  REQUIRE_FALSE(reply);
  CHECK(reply.error() == SourceError::INVALID_CURSOR);
}

TEST_CASE("a growing unterminated record keeps its captured identity and prefix") {
  TemporaryFile file("first\npartial");
  CaptureFixture fixture(file.path);
  const auto bookmark = fixture.capture(1);

  file.write(" completed\n", true);

  CHECK(fixture.resolve(bookmark).state == PositionResolution::EXACT);
}

TEST_CASE("bookmark cancellation precedes file access and metadata limits are checked") {
  TemporaryFile file(std::string(3000, 'x') + "\n");
  CaptureFixture fixture(file.path);
  const auto bookmark = fixture.capture();
  std::stop_source cancellation;

  cancellation.request_stop();
  std::filesystem::remove(file.path);
  PositionRequest request{fixture.source->stamp(), 1, 1, bookmark};
  auto cancelled = fixture.model->resolve(request, cancellation.get_token());

  REQUIRE_FALSE(cancelled);
  CHECK(cancelled.error() == SourceError::CANCELLED);
  auto limited = std::async(std::launch::async, [&] {
                   return fixture.model->capture(fixture.command, fixture.page.page.rows().front(), {.maxBytes = 32});
                 }).get();

  REQUIRE_FALSE(limited);
  CHECK(limited.error() == SourceError::WOULD_BLOCK);
}

TEST_CASE("checkpoint sessions do not consume Property slots and close with their scene") {
  auto application = take(Application::create());
  auto scene = take(ManagedScene::create(*application, {.maxStates = 1}));
  auto first = take(scene->logNavigation());
  auto second = take(scene->logNavigation());

  take(scene->state(1));
  CHECK(take(first.status()).reason == LogRestoreReason::NONE);
  CHECK(take(second.status()).reason == LogRestoreReason::NONE);
  auto foreign = std::async(std::launch::async, [&] {
                   return first.status();
                 }).get();

  REQUIRE_FALSE(foreign);
  CHECK(foreign.error() == SourceError::WRONG_THREAD);
  scene.reset();

  REQUIRE_FALSE(first.status());
  CHECK(first.status().error() == SourceError::CLOSED);
}

TEST_CASE("native bookmark metadata retains no raw record prefix") {
  constexpr std::string_view MARKER = "PRIVATE_LOG_RECORD_PREFIX_7b4869fca0";
  TemporaryFile file(std::string(MARKER) + std::string(2000, 'x') + "\n");
  CaptureFixture fixture(file.path);
  const auto bookmark = fixture.capture();
  const auto metadata = bookmark.metadata();
  const auto raw = std::as_bytes(std::span(MARKER.data(), MARKER.size()));

  CHECK(std::search(metadata.begin(), metadata.end(), raw.begin(), raw.end()) == metadata.end());
  CHECK(metadata.size() < 1024);
  CHECK(fixture.resolve(bookmark).state == PositionResolution::EXACT);
}

TEST_CASE("a blank logical record captures and resolves a zero-length witness") {
  TemporaryFile file("\nnext\n");
  CaptureFixture fixture(file.path);

  REQUIRE(fixture.page.page.rows().front().value.text.empty());
  const auto bookmark = fixture.capture();
  const auto resolved = fixture.resolve(bookmark);

  CHECK(resolved.state == PositionResolution::EXACT);
  CHECK(resolved.position.row == bookmark.position().row);
  CHECK(resolved.position.offset == 0);
}
