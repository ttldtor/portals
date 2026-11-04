// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <portals/log_file.hpp>

#include <array>
#include <chrono>
#include <fstream>
#include <future>

#ifndef _WIN32
#  include <csignal>
#  include <sys/stat.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> result) {
  REQUIRE(result.has_value());

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

struct FileFixture {
  std::filesystem::path path =
    std::filesystem::temp_directory_path() /
    ("portals-log-file-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");

  explicit FileFixture(std::string_view text) {
    write(text);
  }

  ~FileFixture() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(path.string() + ".old", ignored);
  }

  void write(std::string_view text, bool append = false) {
    std::ofstream file(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    REQUIRE(file.is_open());
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(file.good());
  }
};

struct Delivery {
  std::unique_ptr<Scheduler> scheduler = take(Scheduler::create());
  std::unique_ptr<WidgetTree> tree = take(WidgetTree::create(*scheduler));
  Node<Widget> root = take(tree->root());
  std::shared_ptr<FileLogModel> model;
  std::shared_ptr<SourceBudget> budget = take(SourceBudget::create());
  std::unique_ptr<WindowSource<LogLine>> source;
  WindowConsumer<LogLine> consumer;

  explicit Delivery(std::filesystem::path path, FileLogOptions options = {}, WindowSourceOptions delivery = {}) {
    model = take(FileLogModel::create(std::move(path), options));
    source = take(WindowSource<LogLine>::create(*tree, root, model, delivery, budget));
    consumer = take(source->openConsumer());
  }

  std::expected<SourceReadReply<LogLine>, SourceError>
  read(SourceAnchor anchor = {}, SourceDirection direction = SourceDirection::FORWARD, std::size_t count = 16) {
    auto request = take(consumer.request(anchor, direction, count));
    auto result = std::async(std::launch::async, [request] {
                    return request.load();
                  }).get();
    take(request.complete(result));
    take(scheduler->dispatch());

    return result;
  }
};

SourceAnchor cursor(SourceCursor value) {
  SourceAnchor anchor;
  anchor.kind = SourceAnchorKind::CURSOR;
  anchor.cursor = value;

  return anchor;
}
} // namespace

TEST_CASE("file descriptor creation performs no I/O and preserves native open failure") {
  FileFixture file("one\n");
  auto absent = file.path.string() + ".missing";
  auto model = take(FileLogModel::create(absent));
  CHECK(model->capabilities().backward);
  CHECK(model->capabilities().seekRow);
  CHECK(model->capabilities().tail);
  auto result = model->metadata();
  REQUIRE_FALSE(result);
  CHECK(result.error().error == FileLogError::OPEN_FAILED);
  CHECK(result.error().operation == FileLogOperation::OPEN);
  CHECK(result.error().nativeCode != 0);

  Delivery delivery(absent);
  auto loaded = delivery.read();
  REQUIRE_FALSE(loaded);
  CHECK(loaded.error() == SourceError::IO_ERROR);
  REQUIRE(delivery.model->lastFailure());
  CHECK(delivery.model->lastFailure()->error == FileLogError::OPEN_FAILED);
}

TEST_CASE("CR LF CRLF empty records and UTF8 survive every block boundary") {
  FileFixture file("first\r\n\rsecond\n\xc3\xa9\xf0\x9f\x98\x80\rtail");
  FileLogOptions options;
  options.ioBlockBytes = 1;
  Delivery delivery(file.path, options);
  auto reply = take(delivery.read());
  auto rows = reply.page.rows();
  REQUIRE(rows.size() == 5);
  CHECK(rows[0].value.text == "first");
  CHECK(rows[1].value.text.empty());
  CHECK(rows[2].value.text == "second");
  CHECK(rows[3].value.text == "\xc3\xa9\xf0\x9f\x98\x80");
  CHECK(rows[4].value.text == "tail");
  CHECK(rows[0].value.byteOffset == 0);
  CHECK(rows[1].value.byteOffset == 7);
  CHECK(rows[2].id.value == rows[2].value.byteOffset + 1);
  CHECK(reply.edge == SourceEdge::TEMPORARY_TAIL);
  CHECK(delivery.budget->usage().bytes <= delivery.budget->options().maxBytes);

  auto eof = take(delivery.read(cursor(reply.page.after())));
  CHECK(eof.page.rows().empty());
  CHECK(eof.edge == SourceEdge::TEMPORARY_TAIL);
}

TEST_CASE("tail backward traversal and stable row lookup need no row index") {
  FileFixture file("zero\none\rtwo\r\nthree\n");
  Delivery delivery(file.path);
  SourceAnchor tail;
  tail.kind = SourceAnchorKind::TAIL;
  auto last = take(delivery.read(tail, SourceDirection::BACKWARD, 2));
  REQUIRE(last.page.rows().size() == 2);
  CHECK(last.page.rows()[0].value.text == "two");
  CHECK(last.page.rows()[1].value.text == "three");
  CHECK(last.edge == SourceEdge::MORE);
  auto first = take(delivery.read(cursor(last.page.before()), SourceDirection::BACKWARD, 2));
  REQUIRE(first.page.rows().size() == 2);
  CHECK(first.page.rows()[0].value.text == "zero");
  CHECK(first.page.rows()[1].value.text == "one");
  CHECK(first.edge == SourceEdge::FINAL_END);

  SourceAnchor row;
  row.kind = SourceAnchorKind::ROW;
  row.row = last.page.rows()[1].id;
  auto sought = take(delivery.read(row, SourceDirection::FORWARD, 1));
  REQUIRE(sought.page.rows().size() == 1);
  CHECK(sought.page.rows()[0].id == row.row);
  CHECK(sought.page.rows()[0].value.text == "three");
  auto soughtBackward = take(delivery.read(row, SourceDirection::BACKWARD, 1));
  REQUIRE(soughtBackward.page.rows().size() == 1);
  CHECK(soughtBackward.page.rows()[0].id == row.row);
}

TEST_CASE("append preserves row identity and exposes changed unterminated tail") {
  FileFixture file("first\npar");
  Delivery delivery(file.path);
  auto initial = take(delivery.read());
  auto oldTail = initial.page.rows().back().id;
  auto oldRevision = initial.page.rows().back().valueRevision;
  file.write("tial\nnext\n", true);
  SourceAnchor tail;
  tail.kind = SourceAnchorKind::TAIL;
  auto grown = take(delivery.read(tail, SourceDirection::BACKWARD, 2));
  REQUIRE(grown.page.rows().size() == 2);
  CHECK(grown.page.rows()[0].id == oldTail);
  CHECK(grown.page.rows()[0].valueRevision != oldRevision);
  CHECK(grown.page.rows()[0].value.text == "partial");
  CHECK(grown.page.rows()[1].value.text == "next");
  CHECK(initial.page.rows().back().value.text == "par");
}

TEST_CASE("an incomplete UTF8 tail waits at its record gap until append completes it") {
  FileFixture file("complete\n\xf0\x9f");
  Delivery delivery(file.path);
  auto initial = take(delivery.read());
  REQUIRE(initial.page.rows().size() == 1);
  CHECK(initial.edge == SourceEdge::TEMPORARY_TAIL);
  CHECK(initial.page.after().token[0] == 9);
  file.write("\x98\x80\n", true);
  auto completed = take(delivery.read(cursor(initial.page.after())));
  REQUIRE(completed.page.rows().size() == 1);
  CHECK(completed.page.rows()[0].value.text == "\xf0\x9f\x98\x80");
  CHECK(completed.page.rows()[0].value.byteOffset == 9);
}

TEST_CASE("truncate and rotation invalidate old generation without mutating retained pages") {
  FileFixture file("original\nsecond\n");
  Delivery delivery(file.path);
  auto old = take(delivery.read({}, SourceDirection::FORWARD, 1));
  auto generation = old.page.rows()[0].id.generation;
  file.write("new\n");
  auto stale = take(delivery.read(cursor(old.page.after()), SourceDirection::FORWARD, 1));
  CHECK(stale.state == SourceReadState::HISTORY_UNAVAILABLE);
  REQUIRE(stale.recovery);
  CHECK(stale.recovery->token[1] != generation);
  CHECK(old.page.rows()[0].value.text == "original");

  auto current = take(delivery.read({}, SourceDirection::FORWARD, 1));
  std::filesystem::rename(file.path, file.path.string() + ".old");
  file.write("replacement\n");
  auto rotated = take(delivery.read(cursor(current.page.before()), SourceDirection::FORWARD, 1));
  CHECK(rotated.state == SourceReadState::HISTORY_UNAVAILABLE);
  CHECK(take(delivery.model->metadata()).generation != current.page.rows()[0].id.generation);
}

TEST_CASE("oversized and invalid UTF8 records have explicit typed outcomes") {
  FileFixture file(std::string(65, 'x') + "\n");
  FileLogOptions options;
  options.maxRecordBytes = 64;
  Delivery delivery(file.path, options);
  auto oversized = delivery.read();
  REQUIRE_FALSE(oversized);
  CHECK(oversized.error() == SourceError::ROW_TOO_LARGE);
  REQUIRE(delivery.model->lastFailure());
  CHECK(delivery.model->lastFailure()->error == FileLogError::RECORD_TOO_LONG);
  file.write("\xc0\x80\n");
  auto invalid = delivery.read();
  REQUIRE_FALSE(invalid);
  CHECK(invalid.error() == SourceError::IO_ERROR);
  CHECK(delivery.model->lastFailure()->error == FileLogError::INVALID_UTF8);
}

TEST_CASE("byte extent and bounded positional read do not require locating a row") {
  FileFixture file("abcdefghij\n");
  FileLogOptions options;
  options.ioBlockBytes = 4;
  auto model = take(FileLogModel::create(file.path, options));
  CHECK(take(model->metadata()).byteExtent == 11);
  std::array<std::byte, 4> bytes{};
  CHECK(take(model->readBytes(3, bytes)) == 4);
  CHECK(std::to_integer<char>(bytes[0]) == 'd');
  CHECK(std::to_integer<char>(bytes[3]) == 'g');
  CHECK(take(model->readBytes(11, bytes)) == 0);
  CHECK_FALSE(model->readBytes(12, bytes));
  std::array<std::byte, 5> excessive{};
  CHECK_FALSE(model->readBytes(0, excessive));
}

TEST_CASE("literal search resumes across work chunks and block boundaries") {
  FileFixture file("xxxxxxneedle and another needle\n");
  FileLogOptions options;
  options.ioBlockBytes = 3;
  auto model = take(FileLogModel::create(file.path, options));
  auto first = take(model->findLiteral("needle", 0, 8));
  CHECK_FALSE(first.matchByteOffset);
  CHECK_FALSE(first.reachedEnd);
  CHECK(first.nextByteOffset == 6);
  auto second = take(model->findLiteral("needle", first.nextByteOffset, 8));
  REQUIRE(second.matchByteOffset);
  CHECK(*second.matchByteOffset == 6);
  auto last = take(model->findLiteral("absent", second.nextByteOffset, 1024));
  CHECK_FALSE(last.matchByteOffset);
  CHECK(last.reachedEnd);
  CHECK_FALSE(model->findLiteral("", 0, 8));
  CHECK_FALSE(model->findLiteral("needle", 0, 5));
}

TEST_CASE("cancelled file operations do not open or scan the source") {
  FileFixture file("one\ntwo\n");
  auto model = take(FileLogModel::create(file.path));
  std::stop_source stop;
  stop.request_stop();
  auto metadata = model->metadata(stop.get_token());
  REQUIRE_FALSE(metadata);
  CHECK(metadata.error().error == FileLogError::CANCELLED);
  std::array<std::byte, 4> bytes{};
  CHECK(model->readBytes(0, bytes, stop.get_token()).error().error == FileLogError::CANCELLED);
  CHECK(model->findLiteral("one", 0, 8, stop.get_token()).error().error == FileLogError::CANCELLED);
}

TEST_CASE("backward windows fill only the available arena suffix") {
  FileFixture file(std::string(20 * 1024, 'a') + "\n" + std::string(20 * 1024, 'b') + "\n" +
                   std::string(20 * 1024, 'c') + "\n");
  WindowSourceOptions options;
  options.pageBytes = 40 * 1024;
  Delivery delivery(file.path, {}, options);
  SourceAnchor tail;
  tail.kind = SourceAnchorKind::TAIL;
  auto reply = take(delivery.read(tail, SourceDirection::BACKWARD, 3));
  REQUIRE(reply.page.rows().size() == 1);
  CHECK(reply.page.rows()[0].value.text.front() == 'c');
  CHECK(reply.edge == SourceEdge::MORE);
}

TEST_CASE("workspace filled exactly by IO cache returns a page error without terminating") {
  FileFixture file("one\n");
  FileLogOptions fileOptions;
  WindowSourceOptions deliveryOptions;
  deliveryOptions.workspaceBytes = fileOptions.ioBlockBytes + SourceArena::METADATA_BYTES;
  Delivery delivery(file.path, fileOptions, deliveryOptions);
  auto result = delivery.read({}, SourceDirection::FORWARD, 1);
  REQUIRE_FALSE(result);
  CHECK(result.error() == SourceError::PAGE_TOO_LARGE);
}

TEST_CASE("special files are rejected without blocking the worker") {
#ifdef _WIN32
  auto model = take(FileLogModel::create(L"NUL"));
  auto result = model->metadata();
  REQUIRE_FALSE(result);
  CHECK(result.error().error == FileLogError::OPEN_FAILED);
#else
  FileFixture file("");
  REQUIRE(std::filesystem::remove(file.path));
  REQUIRE(::mkfifo(file.path.c_str(), 0600) == 0);
  auto child = ::fork();
  REQUIRE(child >= 0);

  if (!child) {
    auto model = FileLogModel::create(file.path);

    if (!model) {
      ::_exit(2);
    }

    auto result = (*model)->metadata();
    ::_exit(!result && result.error().error == FileLogError::OPEN_FAILED ? 0 : 3);
  }

  int status{};
  bool finished{};
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);

  while (std::chrono::steady_clock::now() < deadline) {
    if (::waitpid(child, &status, WNOHANG) == child) {
      finished = true;

      break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  if (!finished) {
    // This test owns the child; never allow its regression to hang the test suite.
    ::kill(child, SIGKILL);
    ::waitpid(child, &status, 0);
  }

  REQUIRE(finished);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
#endif
}
