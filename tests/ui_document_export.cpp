// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/ui.hpp>
#include <portals/ui_document_export.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace portals;
using namespace std::chrono_literals;

template <class T, class E>
T take(std::expected<T, E> result, std::source_location origin = std::source_location::current()) {
  INFO("checked call ", origin.line());
  REQUIRE(result);
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

struct OutputFile {
  static inline std::atomic<std::size_t> sequence{};
  std::filesystem::path path =
    std::filesystem::temp_directory_path() /
    ("portals-ui-document-export-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
     std::to_string(sequence++) + ".bin");

  ~OutputFile() {
    std::error_code ignored;

    std::filesystem::remove(path, ignored);
  }

  void write(std::string_view bytes) const {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);

    REQUIRE(output.is_open());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(output.good());
  }

  std::string read() const {
    std::ifstream input(path, std::ios::binary);

    REQUIRE(input.is_open());

    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }
};

class Provider final : public ByteDocumentProvider {
public:
  std::string text;
  const std::thread::id ui = std::this_thread::get_id();
  std::atomic<std::size_t> metadataCalls{};
  std::atomic<std::size_t> readCalls{};
  std::atomic<bool> uiOperation{};
  std::atomic<bool> stopObserved{};
  bool failRead{};
  bool blockRead{};
  std::promise<void> entered;

  explicit Provider(std::string bytes) : text(std::move(bytes)) {
  }

  ByteDocumentCapabilities capabilities() const noexcept override {
    return {true, false};
  }

  std::expected<ByteDocumentMetadata, DocumentError> metadata(std::stop_token stop) override {
    ++metadataCalls;
    uiOperation = uiOperation || std::this_thread::get_id() == ui;
    if (stop.stop_requested()) {
      stopObserved = true;

      return std::unexpected(DocumentError{SourceError::CANCELLED, {}, {}});
    }

    return ByteDocumentMetadata{1, text.size()};
  }

  std::expected<ByteReadResult, DocumentError> read(const ByteReadCommand &command, std::span<std::byte> destination,
                                                    std::stop_token stop) override {
    ++readCalls;
    uiOperation = uiOperation || std::this_thread::get_id() == ui;
    if (blockRead) {
      std::stop_callback observe(stop, [&] {
        stopObserved = true;
      });
      std::unique_lock lock(mutex_);

      if (!entered_) {
        entered_ = true;
        entered.set_value();
      }

      // The host gate intentionally ignores cancellation. The scene must return
      // before the test releases this provider; a watchdog bounds a broken join.
      released_.wait(lock, [&] {
        return release_;
      });
    }
    if (stop.stop_requested()) {
      stopObserved = true;

      return std::unexpected(DocumentError{SourceError::CANCELLED, {}, {}});
    }
    if (failRead) {
      return std::unexpected(DocumentError{SourceError::IO_ERROR, std::make_error_code(std::errc::io_error), {}});
    }
    if (command.dataRevision != 1 || command.offset > text.size()) {
      return std::unexpected(DocumentError{SourceError::INVALID_ARGUMENT, {}, {}});
    }

    const auto offset = static_cast<std::size_t>(command.offset);
    const auto count = std::min(destination.size(), text.size() - offset);

    for (std::size_t index = 0; index < count; ++index) {
      destination[index] = static_cast<std::byte>(static_cast<unsigned char>(text[offset + index]));
    }

    return ByteReadResult{count, offset + count == text.size()};
  }

  void release() {
    {
      std::lock_guard lock(mutex_);

      release_ = true;
    }

    released_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable released_;
  bool entered_{};
  bool release_{};
};

class Executor final : public BackgroundExecutor {
public:
  std::shared_ptr<BackgroundJob> pending;
  std::shared_ptr<BackgroundJob> running;
  std::uint64_t epoch{};
  std::atomic<bool> finished{};

  std::expected<BackgroundTicket, BackgroundError> submit(std::unique_ptr<BackgroundJob> &job) override {
    if (pending || running) {
      return std::unexpected(BackgroundError::WOULD_BLOCK);
    }

    pending = std::move(job);

    return BackgroundTicket(pending);
  }

  void service() noexcept override {
  }

  std::uint64_t capacityEpoch() const noexcept override {
    return epoch;
  }

  void startOne() {
    REQUIRE(pending);
    REQUIRE_FALSE(worker_.joinable());
    running = std::exchange(pending, {});
    finished = false;
    worker_ = std::jthread([this, work = running] {
      work->run(work->stopToken());
      published_ = work->publish();
      finished = true;
    });
  }

  void joinOne() {
    if (!worker_.joinable()) {
      return;
    }

    worker_.join();
    CHECK((published_ || published_.error() == AsyncError::CANCELLED || published_.error() == AsyncError::CLOSED));
    running.reset();
    ++epoch;
  }

  void runOne() {
    startOne();
    joinOne();
  }

private:
  std::jthread worker_;
  std::expected<void, AsyncError> published_;
};

struct Fixture {
  std::shared_ptr<Provider> provider;
  Executor executor;
  SchedulerTime now{};
  std::unique_ptr<Application> app =
    take(Application::create({.backgroundExecutor = &executor}, {[](void *context) noexcept {
                                                                   return *static_cast<SchedulerTime *>(context);
                                                                 },
                                                                 &now}));
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));
  ui::Ref<DocumentView> viewer;
  Node<DocumentView> node;

  explicit Fixture(std::string bytes) : provider(std::make_shared<Provider>(std::move(bytes))) {
    node = take(ui::mount(*scene, ui::viewer(viewer, ui::document(provider))));
  }

  ~Fixture() {
    provider->release();
    scene.reset();
    executor.joinOne();
    executor.pending.reset();
    app.reset();
  }

  void dispatch() {
    take(app->scheduler().dispatch());
    take(scene->afterDispatch(*app));
  }
};

void checkOrigin(const ManagedDiagnostic &diagnostic, std::source_location origin) {
  CHECK(diagnostic.origin.line() == origin.line());
  CHECK(std::string_view(diagnostic.origin.file_name()) == origin.file_name());
}
} // namespace

TEST_CASE("managed document export is cold and delivers exact binary output on UI") {
  std::string bytes("first\0second", 12);

  bytes.append("\r\n\xFF\xFE\x1B", 5);
  bytes.append(70 * 1024, 'z');

  SUBCASE("original binary bytes across source pages") {
  }
  SUBCASE("empty source still creates a completed output") {
    bytes.clear();
  }

  OutputFile output;
  Fixture f(bytes);
  std::size_t completions{};
  std::optional<DocumentExportResult> completed;
  const auto uiThread = std::this_thread::get_id();

  take(ui::startExport(f.viewer, output.path, [&](const DocumentExportResult &result) {
    CHECK(std::this_thread::get_id() == uiThread);
    ++completions;
    completed = result;
  }));
  REQUIRE(f.executor.pending);
  CHECK_FALSE(f.executor.pending->stopToken().stop_requested());
  CHECK_FALSE(std::filesystem::exists(output.path));
  CHECK(f.provider->metadataCalls == 0);
  CHECK(f.provider->readCalls == 0);
  CHECK(completions == 0);
  f.executor.runOne();
  CHECK(completions == 0);
  f.dispatch();

  REQUIRE(completed);
  CHECK(completed->complete);
  CHECK_FALSE(completed->error);
  CHECK(completed->bytesWritten == bytes.size());
  CHECK(completed->bytesRead == bytes.size());
  CHECK(completed->range.begin == 0);
  CHECK(completed->range.end == bytes.size());
  CHECK(completed->writtenCountExact);
  CHECK(output.read() == bytes);
  CHECK_FALSE(f.provider->uiOperation);
  f.dispatch();
  CHECK(completions == 1);
  CHECK(take(f.scene->diagnostics()).errors.empty());
}

TEST_CASE("managed document export preserves an existing path and collects its worker failure once") {
  const std::string preserved("keep\0me", 7);
  OutputFile output;

  output.write(preserved);
  Fixture f("replacement bytes");
  std::size_t completions{};
  std::size_t notifications{};
  std::optional<DocumentExportResult> completed;
  const auto uiThread = std::this_thread::get_id();

  take(f.scene->onError([&](const ManagedDiagnostic &) {
    CHECK(std::this_thread::get_id() == uiThread);
    ++notifications;
  }));
  const auto origin = std::source_location::current();

  take(ui::startExport(
    f.viewer, output.path,
    [&](const DocumentExportResult &result) {
      CHECK(std::this_thread::get_id() == uiThread);
      ++completions;
      completed = result;
    },
    origin));
  f.executor.runOne();
  f.dispatch();

  REQUIRE(completed);
  REQUIRE(completed->error);
  CHECK(completed->error->code == SourceError::IO_ERROR);
  CHECK_FALSE(completed->complete);
  CHECK(completed->bytesWritten == 0);
  CHECK(output.read() == preserved);
  const auto diagnostics = take(f.scene->diagnostics());

  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(diagnostics.errors.front().error.source == SourceError::IO_ERROR);
  checkOrigin(diagnostics.errors.front(), origin);
  f.dispatch();
  CHECK(completions == 1);
  CHECK(notifications == 1);
  CHECK(take(f.scene->diagnostics()).errors.size() == 1);
  CHECK_FALSE(f.provider->uiOperation);
}

TEST_CASE("managed document export captures its source before a viewer model replacement") {
  OutputFile output;
  Fixture f("admitted source");
  auto replacement = std::make_shared<Provider>("replacement source");
  std::optional<DocumentExportResult> completed;

  take(ui::startExport(f.viewer, output.path, [&](const DocumentExportResult &result) {
    completed = result;
  }));
  REQUIRE(f.executor.pending);
  take(f.viewer.setModel(ui::document(replacement)));
  CHECK_FALSE(f.executor.pending->stopToken().stop_requested());
  f.executor.runOne();
  f.dispatch();

  REQUIRE(completed);
  CHECK(completed->complete);
  CHECK_FALSE(completed->error);
  CHECK(output.read() == "admitted source");
  CHECK(replacement->metadataCalls == 0);
  CHECK(replacement->readCalls == 0);
  CHECK_FALSE(f.provider->uiOperation);
  CHECK_FALSE(replacement->uiOperation);
}

TEST_CASE("managed document export retires queued work when the node or scene closes") {
  OutputFile output;
  Fixture f("cancelled bytes");
  auto capture = std::make_shared<int>(1);
  std::weak_ptr<int> retained = capture;
  std::size_t completions{};

  take(ui::startExport(f.viewer, output.path, [&, capture](const DocumentExportResult &) {
    ++completions;
  }));
  capture.reset();
  REQUIRE(f.executor.pending);

  SUBCASE("node removal") {
    take(f.scene->remove(f.node));
    f.dispatch();
  }
  SUBCASE("scene close") {
    take(f.scene->close());
  }

  CHECK(f.executor.pending->stopToken().stop_requested());
  CHECK(retained.expired());
  f.executor.runOne();
  f.dispatch();
  CHECK(completions == 0);
  CHECK_FALSE(std::filesystem::exists(output.path));
  CHECK_FALSE(f.provider->uiOperation);
}

TEST_CASE("managed document export retires a queued completion without deleting the finished output") {
  OutputFile output;
  Fixture f("finished bytes");
  std::size_t completions{};

  take(ui::startExport(f.viewer, output.path, [&](const DocumentExportResult &) {
    ++completions;
  }));
  f.executor.runOne();
  REQUIRE(output.read() == "finished bytes");
  CHECK(completions == 0);

  SUBCASE("node removal before UI dispatch") {
    take(f.scene->remove(f.node));
  }
  SUBCASE("scene close before UI dispatch") {
    take(f.scene->close());
  }

  f.dispatch();
  CHECK(completions == 0);
  CHECK(output.read() == "finished bytes");
  CHECK_FALSE(f.provider->uiOperation);
}

TEST_CASE("managed document export cancellation never joins a running provider on UI") {
  OutputFile output;
  Fixture f("blocked worker");

  f.provider->blockRead = true;
  auto entered = f.provider->entered.get_future();
  std::size_t completions{};

  take(ui::startExport(f.viewer, output.path, [&](const DocumentExportResult &) {
    ++completions;
  }));
  f.executor.startOne();
  REQUIRE(entered.wait_for(5s) == std::future_status::ready);

  std::promise<void> retired;
  auto returned = retired.get_future();
  std::atomic<bool> returnedBeforeRelease{};
  std::jthread releaseAfterReturn([&] {
    returnedBeforeRelease = returned.wait_for(2s) == std::future_status::ready;
    f.provider->release();
  });

  SUBCASE("node removal") {
    take(f.scene->remove(f.node));
    f.dispatch();
  }
  SUBCASE("scene close") {
    take(f.scene->close());
  }

  CHECK_FALSE(f.executor.finished);
  retired.set_value();
  releaseAfterReturn.join();
  f.executor.joinOne();
  CHECK(returnedBeforeRelease);
  CHECK(f.provider->stopObserved);
  CHECK(f.executor.finished);
  f.dispatch();
  CHECK(completions == 0);
  CHECK_FALSE(std::filesystem::exists(output.path));
  CHECK_FALSE(f.provider->uiOperation);
}

TEST_CASE("managed document export source and completion failures reach the UI error collector") {
  OutputFile output;
  Fixture f("source bytes");
  std::size_t completions{};
  std::optional<DocumentExportResult> completed;
  bool throwCompletion{};
  const auto uiThread = std::this_thread::get_id();

  SUBCASE("source failure") {
    f.provider->failRead = true;
  }
  SUBCASE("throwing completion") {
    throwCompletion = true;
  }

  const auto origin = std::source_location::current();

  take(ui::startExport(
    f.viewer, output.path,
    [&](const DocumentExportResult &result) {
      CHECK(std::this_thread::get_id() == uiThread);
      ++completions;
      completed = result;
      if (throwCompletion) {
        throw std::runtime_error("completion failed");
      }
    },
    origin));
  f.executor.runOne();
  f.dispatch();

  REQUIRE(completed);
  CHECK(completions == 1);
  const auto diagnostics = take(f.scene->diagnostics());

  REQUIRE(diagnostics.errors.size() == 1);
  checkOrigin(diagnostics.errors.front(), origin);
  if (throwCompletion) {
    CHECK(completed->complete);
    CHECK(output.read() == "source bytes");
    const auto *cause = std::get_if<TreeError>(&diagnostics.errors.front().error.cause);

    REQUIRE(cause);
    CHECK(*cause == TreeError::CALLBACK_FAILED);
  } else {
    REQUIRE(completed->error);
    CHECK(completed->error->code == SourceError::IO_ERROR);
    CHECK(diagnostics.errors.front().error.source == SourceError::IO_ERROR);
  }
  f.dispatch();
  CHECK(take(f.scene->diagnostics()).errors.size() == 1);
  CHECK_FALSE(f.provider->uiOperation);
}

TEST_CASE("managed document export checked rejection stays explicit and compact rejection is collected once") {
  Fixture f("source bytes");
  const auto checked = ui::startExport(f.viewer, {});

  REQUIRE_FALSE(checked);
  CHECK(checked.error().source == SourceError::INVALID_ARGUMENT);
  CHECK(take(f.scene->diagnostics()).errors.empty());
  const auto origin = std::source_location::current();

  CHECK_FALSE(ui::exportFile(f.viewer, {}, {}, origin));
  const auto diagnostics = take(f.scene->diagnostics());

  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(diagnostics.errors.front().error.source == SourceError::INVALID_ARGUMENT);
  checkOrigin(diagnostics.errors.front(), origin);
  CHECK_FALSE(f.executor.pending);
  CHECK(f.provider->metadataCalls == 0);
  CHECK(f.provider->readCalls == 0);
}
