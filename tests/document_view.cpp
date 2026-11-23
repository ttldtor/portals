// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../src/widgets/document_view_state.hpp"
#include <atomic>
#include <doctest.h>
#include <thread>

namespace {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> value) {
  REQUIRE(value);
  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

class Provider final : public ByteDocumentProvider {
public:
  std::string text = std::string(0, '\0');
  std::uint64_t revision{1};
  std::optional<std::uint64_t> syntheticExtent;
  std::optional<std::size_t> maxReply;
  bool seekable{true};
  bool unknown{};
  bool finalUnknown{};
  bool temporaryEmpty{};
  bool failRead{};
  std::atomic<std::size_t> reads{};
  std::atomic<std::size_t> admittedBytes{};
  std::atomic<std::uint64_t> firstReadOffset{};
  std::atomic<bool> uiOperation{};
  const std::thread::id ui = std::this_thread::get_id();

  ByteDocumentCapabilities capabilities() const noexcept override {
    return {seekable, false};
  }

  std::expected<ByteDocumentMetadata, DocumentError> metadata(std::stop_token) override {
    uiOperation = uiOperation || std::this_thread::get_id() == ui;
    return ByteDocumentMetadata{revision,
                                unknown ? std::nullopt : std::optional(syntheticExtent.value_or(text.size()))};
  }

  std::expected<ByteReadResult, DocumentError> read(const ByteReadCommand &command, std::span<std::byte> destination,
                                                    std::stop_token) override {
    uiOperation = uiOperation || std::this_thread::get_id() == ui;
    if (++reads == 1) {
      firstReadOffset = command.offset;
    }
    if (command.dataRevision != revision) {
      return std::unexpected(DocumentError{SourceError::INVALID_CURSOR, {}, {}});
    }
    if (failRead) {
      return std::unexpected(DocumentError{SourceError::IO_ERROR, {}, {}});
    }
    if (temporaryEmpty) {
      return ByteReadResult{};
    }
    const auto extent = syntheticExtent.value_or(text.size());
    const auto offset = std::min(command.offset, extent);
    const auto count = static_cast<std::size_t>(
      std::min<std::uint64_t>(std::min(destination.size(), maxReply.value_or(destination.size())), extent - offset));

    if (syntheticExtent) {
      REQUIRE_FALSE(text.empty());
      for (std::size_t byte = 0; byte < count; ++byte) {
        destination[byte] = static_cast<std::byte>(text[(offset + byte) % text.size()]);
      }
    } else {
      std::memcpy(destination.data(), text.data() + offset, count);
    }

    admittedBytes += count;
    return ByteReadResult{count, (!unknown || finalUnknown) && offset + count == extent};
  }
};

struct Fixture {
  std::shared_ptr<SourceBudget> budget = take(SourceBudget::create());
  std::shared_ptr<Provider> provider = std::make_shared<Provider>();
  std::shared_ptr<ByteDocumentSource> source;
  detail::DocumentPrepareIntent intent;

  explicit Fixture(std::string text, std::size_t page = 64 * 1024, bool seekable = true) {
    provider->text = std::move(text);
    provider->seekable = seekable;
    source = take(ByteDocumentSource::create(provider, budget, {.maxPageBytes = page}));
    intent.viewport = {20, 4};
    intent.config.workBytes = 1024;
  }

  std::shared_ptr<const detail::DocumentPreparation> run(std::shared_ptr<const detail::DocumentScan> previous = {},
                                                         std::stop_token stop = {}) {
    intent.retainedHexRows = previous && previous->hex ? previous->hex->rawRows.size() : 0;
    intent.retainedTextMaps = previous && previous->highlight ? previous->highlight->maxMaps : 0;
    intent.retainPreview = previous && previous->highlight && previous->highlight->preview;
    intent.replayBytes = !provider->seekable && intent.config.language != DocumentLanguage::PLAIN
                           ? 8 * intent.config.maxFrameBytes + std::min(source->maxPageBytes(), intent.config.workBytes)
                           : 0;
    const auto amount = take(detail::documentPreparationBytes(intent.viewport, intent.config,
                                                              {.hexRows = intent.retainedHexRows,
                                                               .textMaps = intent.retainedTextMaps,
                                                               .replayBytes = intent.replayBytes,
                                                               .preview = intent.retainPreview,
                                                               .suppressPreview = intent.suppressPreview}));
    auto credit = take(budget->reserve({amount, 0, 1, 0}));
    std::shared_ptr<const detail::DocumentPreparation> result;
    std::thread worker([&] {
      result = detail::prepareDocument(source, intent, std::move(previous), 7, std::move(credit), stop);
    });
    worker.join();
    REQUIRE(result);
    CHECK_FALSE(provider->uiOperation.load());
    return result;
  }

  std::shared_ptr<const detail::DocumentPreparation> finish(std::size_t turns = 1000,
                                                            std::shared_ptr<const detail::DocumentScan> previous = {}) {
    std::shared_ptr<const detail::DocumentPreparation> result;
    for (std::size_t turn = 0; turn < turns; ++turn) {
      intent.resume = previous != nullptr;
      result.reset();
      result = run(std::move(previous));
      if (result->error || !result->continueWork) {
        return result;
      }
      previous = result->scan;
      if (result->frame && result->frame->status.highlighting == DocumentSyntaxState::PENDING && previous->highlight &&
          !previous->highlight->sequential) {
        intent.kind = detail::DocumentPreparationKind::HIGHLIGHT;
      }
    }
    FAIL("bounded test turn limit reached");
    return result;
  }

  std::shared_ptr<const detail::DocumentPreparation> layout(std::shared_ptr<const detail::DocumentScan> previous = {}) {
    intent.kind = detail::DocumentPreparationKind::LAYOUT;
    for (std::size_t turn = 0; turn < 1000; ++turn) {
      const auto result = run(previous);
      if (result->error || result->frame || !result->continueWork) {
        return result;
      }
      previous = result->scan;
      intent.resume = true;
    }
    FAIL("bounded layout test turn limit reached");
    return {};
  }
};
} // namespace

TEST_CASE("shared source exposes the exact root without opening the provider") {
  Fixture f("hello");
  CHECK(f.source->budget() == f.budget);
  CHECK(f.provider->reads.load() == 0);
  CHECK_FALSE(f.provider->uiOperation.load());
}

TEST_CASE("chunk1 preserves ordinary graphemes and CRLF logical numbers") {
  Fixture f("a\xCC\x81\xF0\x9F\x98\x80\r\nsecond\nthird", 1);
  f.intent.config.lineNumbers = true;
  const auto result = f.finish();
  REQUIRE_FALSE(result->error);
  REQUIRE(result->frame);
  REQUIRE(result->frame->rows.size() == 3);
  CHECK(result->frame->rows[0].bytes() == "a\xCC\x81\xF0\x9F\x98\x80");
  REQUIRE(result->frame->gutters);
  CHECK((*result->frame->gutters)[0].bytes() == "      1  ");
  CHECK(result->frame->rows[1].bytes() == "second");
  CHECK((*result->frame->gutters)[1].bytes() == "      2  ");
  CHECK(result->frame->status.totalLines == 3);
  CHECK(result->frame->status.finalEnd);
}

TEST_CASE("explicit UTF16BE crosses single byte pages and omits its BOM") {
  Fixture f(std::string("\xFE\xFF\0A\0\n\0B", 8), 1);
  f.intent.config.decoder.explicitEncoding = DocumentEncoding::UTF16BE;
  const auto result = f.finish();
  REQUIRE_FALSE(result->error);
  REQUIRE(result->frame);
  REQUIRE(result->frame->rows.size() == 2);
  CHECK(result->frame->rows[0].bytes() == "A");
  CHECK(result->frame->rows[1].bytes() == "B");
}

TEST_CASE("terminal controls are escaped before retained GraphemeText") {
  Fixture f(std::string("A\x1b[31m\0Z", 9));
  f.intent.viewport.width = 80;
  const auto result = f.finish();
  REQUIRE_FALSE(result->error);
  REQUIRE(result->frame);
  const auto rendered = result->frame->rows[0].bytes();
  CHECK(rendered.find("\\u001B[31m\\u0000Z") != std::string_view::npos);
  CHECK(rendered.find('\x1b') == std::string_view::npos);
  CHECK(rendered.find('\0') == std::string_view::npos);
}

TEST_CASE("unwrapped huge line opens a horizontal window without reading its end") {
  Fixture f(std::string(2 * 1024 * 1024, 'x'));
  f.intent.config.wrap = false;
  const auto result = f.run();
  REQUIRE_FALSE(result->error);
  REQUIRE(result->frame);
  REQUIRE(result->frame->rows.size() == 1);
  CHECK(result->frame->rows[0].bytes() == std::string(20, 'x'));
  CHECK(f.provider->admittedBytes.load() <= f.intent.config.workBytes);
  CHECK_FALSE(result->frame->status.finalEnd);
  CHECK_FALSE(result->frame->status.totalLines);
  CHECK_FALSE(result->continueWork);
}

TEST_CASE("pathological combining run uses visible bounded continuation without rejecting the source") {
  std::string text("a");
  for (std::size_t mark = 0; mark < 1100; ++mark) {
    text.append("\xCC\x81");
  }
  Fixture f(std::move(text), 1);
  const auto result = f.finish();
  REQUIRE_FALSE(result->error);
  REQUIRE(result->frame);
  CHECK(result->frame->status.fragmentedCluster);
  CHECK(result->frame->rows[0].bytes().find("\xE2\x97\x8C") != std::string_view::npos);
  CHECK(result->frame->status.byteExtent == 2201);
}

TEST_CASE("temporary empty MORE waits without EOF or automatic repeated reads") {
  Fixture f("later");
  f.provider->unknown = true;
  f.provider->temporaryEmpty = true;
  auto first = f.run();
  REQUIRE_FALSE(first->error);
  REQUIRE(first->frame);
  CHECK(first->frame->status.waiting);
  CHECK_FALSE(first->frame->status.finalEnd);
  CHECK_FALSE(first->continueWork);
  CHECK(f.provider->reads.load() == 1);
  f.provider->temporaryEmpty = false;
  auto second = f.run(first->scan);
  REQUIRE_FALSE(second->error);
  REQUIRE(second->frame);
  CHECK(second->frame->status.waiting);
  CHECK_FALSE(second->frame->status.finalEnd);
}

TEST_CASE("revision and encoding changes discard previous decoder and layout checkpoints") {
  Fixture f("old\n");
  auto first = f.finish();
  REQUIRE_FALSE(first->error);
  f.provider->text = std::string("\0N\0E\0W", 6);
  f.provider->revision = 2;
  f.intent.config.decoder.explicitEncoding = DocumentEncoding::UTF16BE;
  f.intent.resume = true;
  auto second = f.run(first->scan);
  REQUIRE_FALSE(second->error);
  REQUIRE(second->frame);
  CHECK(second->frame->status.revision.dataRevision == 2);
  CHECK(second->frame->rows[0].bytes() == "NEW");
}

TEST_CASE("cold byte seek proves the prefix instead of resetting inside a malformed sequence") {
  Fixture f("A\xC2Z\nvalid\n");
  f.intent.anchor = 6;
  f.intent.config.decoder.policy = DocumentDecodePolicy::STRICT_MODE;
  auto result = f.finish();
  REQUIRE(result->error);
  CHECK(result->error->code == SourceError::INVALID_REPLY);
}

TEST_CASE("published scanner snapshots retain their own shared-budget reservation") {
  Fixture f(std::string(10000, 'x'));
  f.intent.anchor = 9000;
  f.intent.config.workBytes = 16;
  const auto baseline = f.budget->usage().bytes;
  auto result = f.run();
  REQUIRE(result->continueWork);
  auto scan = result->scan;
  result.reset();
  CHECK(f.budget->usage().bytes > baseline);
  scan.reset();
  CHECK(f.budget->usage().bytes == baseline);
}

TEST_CASE("pre-cancelled work reports cancellation and does not read") {
  Fixture f("unused");
  std::stop_source stop;
  stop.request_stop();
  auto result = f.run({}, stop.get_token());
  REQUIRE(result->error);
  CHECK(result->error->code == SourceError::CANCELLED);
  CHECK(f.provider->reads.load() == 0);
}

TEST_CASE("every supported encoding flows through the page1 viewer adapter") {
  struct Case {
    DocumentEncoding encoding;
    std::string bytes;
    std::string expected;
  };

  const std::array<Case, 7> cases{{{DocumentEncoding::UTF8, "\xD0\xAF", "\xD0\xAF"},
                                   {DocumentEncoding::UTF16LE, std::string("\x2F\x04", 2), "\xD0\xAF"},
                                   {DocumentEncoding::UTF16BE, std::string("\x04\x2F", 2), "\xD0\xAF"},
                                   {DocumentEncoding::UTF32LE, std::string("\x2F\x04\0\0", 4), "\xD0\xAF"},
                                   {DocumentEncoding::UTF32BE, std::string("\0\0\x04\x2F", 4), "\xD0\xAF"},
                                   {DocumentEncoding::WINDOWS1251, "\xDF", "\xD0\xAF"},
                                   {DocumentEncoding::WINDOWS1252, "\x80", "\xE2\x82\xAC"}}};
  for (const auto &sample : cases) {
    Fixture f(sample.bytes, 1);
    f.intent.config.decoder.explicitEncoding = sample.encoding;
    f.intent.config.workBytes = 1;
    const auto result = f.finish();
    REQUIRE_FALSE(result->error);
    REQUIRE(result->frame);
    REQUIRE(result->frame->rows.size() == 1);
    CHECK(result->frame->rows[0].bytes() == sample.expected);
  }
}

TEST_CASE("repeated cold seeks reuse proven sparse checkpoints but a changed encoding cannot") {
  Fixture f(std::string(300000, 'a'));
  f.intent.anchor = 200000;
  f.intent.config.workBytes = 65536;
  auto first = f.finish();
  REQUIRE_FALSE(first->error);
  REQUIRE(first->scan->checkpointCount > 0);
  f.intent.anchor = 150000;
  f.intent.resume = false;
  f.provider->reads = 0;
  auto second = f.run(first->scan);
  REQUIRE_FALSE(second->error);
  CHECK(f.provider->firstReadOffset.load() > 0);
  f.intent.config.decoder.explicitEncoding = DocumentEncoding::WINDOWS1252;
  f.provider->reads = 0;
  auto third = f.run(second->scan);
  REQUIRE_FALSE(third->error);
  CHECK(f.provider->firstReadOffset.load() == 0);
}

TEST_CASE("empty and trailing-separator documents agree with source222 logical line semantics") {
  for (const auto &sample : {std::string(), std::string("a\n")}) {
    Fixture f(sample);
    const auto result = f.finish();
    REQUIRE_FALSE(result->error);
    REQUIRE(result->frame);
    CHECK(result->frame->status.totalLines == (sample.empty() ? 1 : 2));
    CHECK(result->frame->rows.back().bytes() == (sample.empty() ? "" : "a"));
  }
}

TEST_CASE("separate gutter storage is charged without changing original row anchors") {
  Fixture f("first\nsecond");
  const auto plain = take(detail::documentPreparationBytes(f.intent.viewport, f.intent.config));
  f.intent.config.lineNumbers = true;
  const auto withGutter = take(detail::documentPreparationBytes(f.intent.viewport, f.intent.config));
  CHECK(withGutter - plain ==
        static_cast<std::size_t>(f.intent.viewport.height) *
          (sizeof(GraphemeText) + detail::DOCUMENT_GUTTER_CELLS * detail::DOCUMENT_PREPARED_CELL_BYTES));
  const auto result = f.finish();
  REQUIRE(result->frame);
  REQUIRE(result->frame->gutters);
  CHECK(result->frame->credit.usage().bytes == withGutter);
  CHECK(result->frame->rows[0].bytes() == "first");
  CHECK((*result->frame->gutters)[0].bytes() == "      1  ");
  CHECK(result->frame->anchors[0] == 0);
  CHECK(result->frame->anchors[1] == 6);
  CHECK(result->frame->gutterCells == detail::DOCUMENT_GUTTER_CELLS);
  f.intent.config.maxFrameBytes = 4;
  const auto tooSmall = f.finish();
  REQUIRE(tooSmall->error);
  CHECK(tooSmall->error->code == SourceError::PAGE_TOO_LARGE);
}

TEST_CASE("prepared text position shows only percent while advanced status retains the proven line") {
  Fixture f("first\nsecond\nthird");
  f.intent.viewport = {30, 6};
  const auto result = f.finish();
  REQUIRE(result->frame);
  REQUIRE(result->frame->footer);
  REQUIRE(result->frame->status.finalEnd);
  const auto caption = result->frame->footer->bytes();
  CHECK(caption == "100.0%");
  CHECK(result->frame->status.logicalLine == 1);
  CHECK(result->frame->status.byteOffset == 0);
}

TEST_CASE("full viewport terminal LF needs no extra scroll to report visible text end") {
  Fixture f("one\ntwo\nthree\n");
  f.intent.viewport = {20, 3};
  const auto result = f.finish();
  REQUIRE(result->frame);
  REQUIRE(result->frame->footer);
  REQUIRE(result->frame->rows.size() == 3);
  CHECK(result->frame->footer->bytes().find("100.0%") != std::string_view::npos);
  CHECK(result->frame->finalEndVisible);
  CHECK(result->frame->status.finalEnd);
  CHECK(result->frame->status.byteOffset == 0);
}

TEST_CASE("position arithmetic stays exact at uint64 boundaries and unknown progress stays unknown") {
  DocumentViewStatus status;
  status.byteExtent = std::numeric_limits<std::uint64_t>::max();
  for (const auto [anchor, expected] : std::array<std::pair<std::uint64_t, std::uint16_t>, 5>{
         {{0, 0}, {1, 0}, {*status.byteExtent / 2, 499}, {*status.byteExtent - 1, 999}, {*status.byteExtent, 999}}}) {
    status.byteOffset = anchor;
    CHECK(detail::documentProgress(status, false, status.byteOffset) == expected);
    CHECK(status.byteOffset == anchor);
  }
  CHECK(detail::documentProgress(status, true, status.byteOffset) == 1000);
  status.waiting = true;
  CHECK(detail::documentProgress(status, true, status.byteOffset) == 999);
  status.byteExtent.reset();
  CHECK_FALSE(detail::documentProgress(status, false, status.byteOffset));
  CHECK_FALSE(detail::documentProgress(status, true, status.byteOffset));
  status.waiting = false;
  CHECK(detail::documentProgress(status, true, status.byteOffset) == 1000);
}

TEST_CASE("forward navigation stops at the last full reading screen") {
  std::string text;
  for (int index = 1; index <= 120; ++index) {
    text += "row" + std::to_string(index) + "\n";
  }
  for (const auto delta : {1, 2, 100}) {
    Fixture f(text);
    f.intent.viewport = {40, 48};
    f.intent.anchor = text.find("row72\n");
    f.intent.rowDelta = delta;
    const auto result = f.finish();
    REQUIRE_FALSE(result->error);
    REQUIRE(result->frame);
    REQUIRE(result->frame->rows.size() == 48);
    CHECK(result->frame->rows.front().bytes() == "row73");
    CHECK(result->frame->rows.back().bytes() == "row120");
    CHECK(result->frame->finalEndVisible);
  }
}

TEST_CASE("reading a terminal separator does not invent another display row") {
  for (const auto &ending : {std::string("\n"), std::string("\r\n")}) {
    Fixture f("one" + ending + ending);
    const auto result = f.finish();
    REQUIRE_FALSE(result->error);
    REQUIRE(result->frame);
    REQUIRE(result->frame->rows.size() == 2);
    CHECK(result->frame->rows.front().bytes() == "one");
    CHECK(result->frame->rows.back().bytes().empty());
  }
}

TEST_CASE("waiting forward frames report the first rendered row") {
  Fixture f("a\nb\n");
  f.provider->unknown = true;
  f.intent.viewport = {20, 3};
  f.intent.rowDelta = 1;
  const auto result = f.finish();
  REQUIRE_FALSE(result->error);
  REQUIRE(result->frame);
  REQUIRE(result->frame->status.waiting);
  REQUIRE(result->frame->rows.size() == 1);
  CHECK(result->frame->rows.front().bytes() == "b");
  REQUIRE(result->frame->anchors.size() == 1);
  CHECK(result->frame->anchors.front() == 2);
  CHECK(result->frame->status.visibleByteOffset == 2);
  CHECK(result->frame->status.byteOffset == 2);
  CHECK(result->frame->status.logicalLine == 2);
}

TEST_CASE("predecessor retention fits the same text payload budget as the visible screen") {
  for (const auto wrap : {false, true}) {
    Fixture f(wrap ? "aabbccdd" : "aa\nbb\ncc\ndd\n");
    f.intent.viewport = {wrap ? 2 : 20, 2};
    f.intent.config.wrap = wrap;
    f.intent.config.maxFrameBytes = 4;
    f.intent.rowDelta = 1;
    const auto result = f.finish();
    REQUIRE_FALSE(result->error);
    REQUIRE(result->frame);
    REQUIRE(result->frame->rows.size() == 2);
    CHECK(result->frame->rows.front().bytes() == "bb");
    CHECK(result->frame->rows.back().bytes() == "cc");
  }
}

TEST_CASE("coverage uses the bottom of the displayed original byte range") {
  Fixture f("a\nbb\nccc\ndddd\n");
  f.intent.viewport = {20, 2};
  auto result = f.finish();
  REQUIRE(result->frame);
  CHECK(result->frame->footer->bytes() == "35.7%");
  CHECK(result->frame->status.byteOffset == 0);
  f.intent.rowDelta = 1;
  result = f.finish();
  REQUIRE(result->frame);
  CHECK(result->frame->rows.front().bytes() == "bb");
  CHECK(result->frame->footer->bytes() == "64.2%");
  f.intent.rowDelta = 20;
  result = f.finish();
  REQUIRE(result->frame);
  CHECK(result->frame->footer->bytes() == "100.0%");
}

TEST_CASE("coverage counts original UTF16 bytes rather than rendered UTF8 bytes") {
  Fixture f(std::string("A\0\n\0B\0\n\0C\0\n\0", 12), 1);
  f.intent.config.decoder.explicitEncoding = DocumentEncoding::UTF16LE;
  f.intent.viewport = {20, 2};
  const auto result = f.finish();
  REQUIRE(result->frame);
  CHECK(result->frame->footer->bytes() == "66.6%");
}

TEST_CASE("coverage excludes a hidden final wrapped grapheme") {
  Fixture f("a\xCC\x81\xE7\x95\x8C");
  f.intent.viewport = {2, 1};
  const auto result = f.finish();
  REQUIRE(result->frame);
  CHECK(result->frame->rows.front().bytes() == "a\xCC\x81");
  CHECK_FALSE(result->frame->finalEndVisible);
  CHECK(result->frame->footer->bytes() == "50.0%");
}

TEST_CASE("coverage reaches the real terminal CRLF without another wheel step") {
  Fixture f("a\r\nbb\r\n", 1);
  f.intent.viewport = {20, 2};
  const auto result = f.finish();
  REQUIRE(result->frame);
  CHECK(result->frame->footer->bytes() == "100.0%");
  CHECK(result->frame->finalEndVisible);
}

TEST_CASE("coverage of a partial unwrapped line excludes decoder lookahead") {
  Fixture f("abcdefghij");
  f.intent.viewport = {3, 1};
  f.intent.config.wrap = false;
  const auto result = f.finish();
  REQUIRE(result->frame);
  CHECK(result->frame->rows.front().bytes() == "abc");
  CHECK(result->frame->footer->bytes() == "30.0%");
}

TEST_CASE("CRLF lookahead cannot retain an arbitrary run of carriage returns") {
  Fixture f(std::string(128, '\r'));
  f.intent.viewport = {20, 1};
  f.intent.config.workBytes = 16;
  const auto result = f.finish();
  REQUIRE_FALSE(result->error);
  REQUIRE(result->frame);
  CHECK(result->frame->rows.size() == 1);
  CHECK(result->scan->rows.size() <= 2);
  CHECK(f.provider->admittedBytes.load() <= 16);
  CHECK_FALSE(result->frame->finalEndVisible);
}

TEST_CASE("tail preparation ignores a bookmark seed without a full-screen row proof") {
  std::string rows;

  for (unsigned row = 0; row < 10; ++row) {
    if (row) {
      rows += "\n";
    }
    rows += "row-" + std::to_string(row);
  }

  Fixture f(rows);
  f.intent.anchor = rows.size();
  auto seeded = f.finish();
  REQUIRE_FALSE(seeded->error);
  REQUIRE(seeded->scan);
  REQUIRE(seeded->scan->scalarCheckpoint.decoder);
  REQUIRE(seeded->scan->scalarCheckpoint.offset > rows.find("row-6"));
  f.intent.seed = seeded->scan->scalarCheckpoint;
  seeded.reset();
  f.intent.tail = true;
  f.intent.resume = false;

  const auto result = f.finish();

  REQUIRE_FALSE(result->error);
  REQUIRE(result->frame);
  REQUIRE(result->frame->rows.size() == 4);
  CHECK(result->frame->rows.front().bytes() == "row-6");
  CHECK(result->frame->rows.back().bytes() == "row-9");
}

TEST_CASE("cold tail reads a bounded suffix of a large seekable document") {
  constexpr std::uint64_t SOURCE_BYTES = 64 * 1024 * 1024;
  constexpr std::size_t VIEWPORT_ROWS = 48;
  Fixture f("row\n");
  f.provider->syntheticExtent = SOURCE_BYTES;
  f.intent.viewport = {40, VIEWPORT_ROWS};
  f.intent.config.workBytes = 256 * 1024;
  f.intent.config.wrap = false;
  f.intent.anchor = SOURCE_BYTES;
  f.intent.tail = true;

  const auto result = f.finish();

  REQUIRE_FALSE(result->error);
  REQUIRE(result->frame);
  REQUIRE(result->frame->rows.size() == VIEWPORT_ROWS);
  CHECK(result->frame->rows.front().bytes() == "row");
  CHECK(result->frame->rows.back().bytes() == "row");
  CHECK(result->frame->status.visibleByteOffset == SOURCE_BYTES - VIEWPORT_ROWS * f.provider->text.size());
  CHECK(result->frame->finalEndVisible);
  CHECK(f.provider->admittedBytes.load() <=
        2 * f.source->maxPageBytes() + VIEWPORT_ROWS * f.provider->text.size() + DocumentDecoder::LOCAL_WINDOW_BYTES);
}

namespace {
TEST_CASE("wrapped ASCII tail matches the scalar path through Unicode and source seams") {
  const std::string ascii(173, 'a');
  const std::array samples{
    ascii + "\r\n" + ascii + "\n" + ascii + "\r\nlast",
    ascii + "e\xCC\x81" + ascii + "\r\n" +
      "\xD8\x80"
      "A" +
      ascii + "\nlast",
    ascii + "\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x92\xBB" + ascii + "\r\nlast",
    ascii + "\t\x1b[31m" + ascii + "\r\n\r\nlast\r\n",
    ascii + "\xE2\x82" + ascii + "\nlast",
    std::string(70 * 1024, 'a') + "Z\r\nlast",
  };

  for (const auto &text : samples) {
    for (const bool lineNumbers : {false, true}) {
      Fixture reference(text, 1);
      reference.intent.viewport = {31, 4};
      reference.intent.config.workBytes = 4096;
      reference.intent.config.lineNumbers = lineNumbers;
      reference.intent.tail = true;
      reference.intent.anchor = text.size();
      const auto expected = reference.finish();

      REQUIRE_FALSE(expected->error);
      REQUIRE(expected->frame);
      REQUIRE(expected->frame->finalEndVisible);

      for (const std::size_t page : {2, 3, 7, 64, 64 * 1024}) {
        CAPTURE(page);
        CAPTURE(lineNumbers);
        Fixture candidate(text, page);
        candidate.intent = reference.intent;
        const auto actual = candidate.finish();

        REQUIRE_FALSE(actual->error);
        REQUIRE(actual->frame);
        REQUIRE(actual->scan);
        REQUIRE(actual->scan->rows.size() == expected->scan->rows.size());
        REQUIRE(actual->frame->rows.size() == expected->frame->rows.size());
        CHECK(actual->frame->finalEndVisible);
        CHECK(actual->frame->visibleByteEnd == expected->frame->visibleByteEnd);
        CHECK(actual->frame->status.visibleByteOffset == expected->frame->status.visibleByteOffset);
        CHECK(actual->frame->status.logicalLine == expected->frame->status.logicalLine);
        CHECK(actual->frame->status.fragmentedCluster == expected->frame->status.fragmentedCluster);
        CHECK(actual->frame->anchors == expected->frame->anchors);

        for (std::size_t row = 0; row < actual->frame->rows.size(); ++row) {
          CHECK(actual->frame->rows[row].bytes() == expected->frame->rows[row].bytes());
          CHECK(actual->scan->rows[row].offset == expected->scan->rows[row].offset);
          CHECK(actual->scan->rows[row].end == expected->scan->rows[row].end);
        }
      }
    }
  }
}

TEST_CASE("ASCII tail discards hidden rows with exact wrap phase and sparse checkpoints") {
  constexpr std::uint64_t SOURCE_BYTES = 80 * 1024 + 37;
  constexpr std::uint64_t CHECKPOINT_BYTES = 64 * 1024;
  constexpr std::size_t VIEWPORT_ROWS = 4;
  const std::string pattern = "abCde123";

  for (const int width : {1, 2, 17, 31}) {
    for (const bool lineNumbers : {false, true}) {
      CAPTURE(width);
      CAPTURE(lineNumbers);
      Fixture f(pattern);
      f.provider->syntheticExtent = SOURCE_BYTES;
      f.intent.viewport = {width + (lineNumbers ? detail::DOCUMENT_GUTTER_CELLS : 0), VIEWPORT_ROWS};
      f.intent.config.workBytes = 256 * 1024;
      f.intent.config.lineNumbers = lineNumbers;
      f.intent.anchor = SOURCE_BYTES;
      f.intent.tail = true;

      const auto result = f.finish();

      REQUIRE_FALSE(result->error);
      REQUIRE(result->frame);
      REQUIRE(result->scan);
      REQUIRE(result->frame->rows.size() == VIEWPORT_ROWS);
      CHECK(result->frame->finalEndVisible);
      CHECK(result->frame->visibleByteEnd == SOURCE_BYTES);
      CHECK(result->frame->status.logicalLine == 1);
      CHECK_FALSE(result->frame->status.fragmentedCluster);
      const auto last = (SOURCE_BYTES - 1) / static_cast<std::uint64_t>(width) * width;
      const auto first = last - (VIEWPORT_ROWS - 1) * static_cast<std::uint64_t>(width);

      CHECK(result->frame->status.visibleByteOffset == first);

      for (std::size_t row = 0; row < VIEWPORT_ROWS; ++row) {
        const auto begin = first + row * static_cast<std::uint64_t>(width);
        const auto end = std::min(SOURCE_BYTES, begin + width);
        std::string expected;

        for (auto offset = begin; offset < end; ++offset) {
          expected += pattern[offset % pattern.size()];
        }

        CHECK(result->frame->anchors[row] == begin);
        CHECK(result->frame->rows[row].bytes() == expected);
        CHECK(result->scan->rows[row].offset == begin);
        CHECK(result->scan->rows[row].end == end);
      }

      REQUIRE(result->scan->checkpointCount > 0);
      std::uint64_t previous{};

      for (const auto &point : std::span(result->scan->checkpoints).first(result->scan->checkpointCount)) {
        CHECK(point.offset % static_cast<std::uint64_t>(width) == 0);
        CHECK(point.offset - previous >= CHECKPOINT_BYTES);
        CHECK(point.offset - previous < CHECKPOINT_BYTES + static_cast<std::uint64_t>(width));
        CHECK(point.line == 1);
        CHECK(point.lineKnown);
        previous = point.offset;
      }
    }
  }
}

TEST_CASE("ASCII tail checkpoints preserve small decoder work and narrow encoding seams") {
  for (const auto encoding : {DocumentEncoding::UTF8, DocumentEncoding::WINDOWS1251, DocumentEncoding::WINDOWS1252}) {
    const auto edge = encoding == DocumentEncoding::UTF8 ? "\xF0\x9F\x98\x80" : "\xE9";
    const auto text = std::string(edge) + std::string(97, 'a') + edge + "\r\nlast";

    for (const std::size_t workLimit : {1, 3}) {
      for (const int width : {1, 2}) {
        const auto prepare = [&](std::size_t page) {
          Fixture f(text, page);
          f.intent.viewport = {width, 4};
          f.intent.config.decoder.explicitEncoding = encoding;
          f.intent.config.decoder.workLimit = workLimit;
          f.intent.tail = true;
          f.intent.anchor = text.size();

          return f.finish();
        };
        const auto expected = prepare(1);
        const auto actual = prepare(64 * 1024);

        CAPTURE(encoding);
        CAPTURE(workLimit);
        CAPTURE(width);
        REQUIRE_FALSE(expected->error);
        REQUIRE_FALSE(actual->error);
        REQUIRE(actual->frame);
        REQUIRE(expected->frame);
        REQUIRE(actual->frame->rows.size() == expected->frame->rows.size());
        CHECK(actual->frame->finalEndVisible);
        CHECK(actual->frame->anchors == expected->frame->anchors);
        CHECK(actual->frame->visibleByteEnd == expected->frame->visibleByteEnd);

        for (std::size_t row = 0; row < actual->frame->rows.size(); ++row) {
          CHECK(actual->frame->rows[row].bytes() == expected->frame->rows[row].bytes());
          CHECK(actual->scan->rows[row].offset == expected->scan->rows[row].offset);
          CHECK(actual->scan->rows[row].end == expected->scan->rows[row].end);
        }
      }
    }
  }
}

TEST_CASE("wrapped ASCII tail preserves strict malformed and invalid width failures") {
  const auto check = [](WidthPolicy width, DocumentDecodePolicy policy) {
    const std::string text = std::string(511, 'a') + "\xE2\x82";
    std::optional<SourceError> expected;

    for (const std::size_t page : {1, 64 * 1024}) {
      Fixture f(text, page);
      f.intent.viewport = {31, 4};
      f.intent.width = width;
      f.intent.config.decoder.policy = policy;
      f.intent.tail = true;
      f.intent.anchor = text.size();
      const auto result = f.finish();

      REQUIRE(result->error);
      if (expected) {
        CHECK(result->error->code == *expected);
      }
      expected = result->error->code;
    }
  };

  check({}, DocumentDecodePolicy::STRICT_MODE);
  check({.emoji = static_cast<EmojiWidth>(99)}, DocumentDecodePolicy::REPLACE);
}

std::string tailEncodedRows(DocumentEncoding encoding, std::string_view separator, bool bom, unsigned count = 6) {
  std::string bytes;
  const bool bigEndian = encoding == DocumentEncoding::UTF16BE || encoding == DocumentEncoding::UTF32BE;
  const unsigned width = encoding == DocumentEncoding::UTF16LE || encoding == DocumentEncoding::UTF16BE   ? 2
                         : encoding == DocumentEncoding::UTF32LE || encoding == DocumentEncoding::UTF32BE ? 4
                                                                                                          : 1;
  const auto appendUnit = [&](std::uint32_t value) {
    for (unsigned byte = 0; byte < width; ++byte) {
      const auto shift = (bigEndian ? width - byte - 1 : byte) * 8;
      bytes += static_cast<char>((value >> shift) & 0xff);
    }
  };

  if (bom) {
    if (encoding == DocumentEncoding::UTF8) {
      bytes += "\xEF\xBB\xBF";
    } else if (width > 1) {
      appendUnit(0xfeff);
    }
  }

  for (unsigned row = 0; row < count; ++row) {
    for (const auto value : "row-" + std::to_string(row) + ":") {
      appendUnit(static_cast<unsigned char>(value));
    }
    if (encoding == DocumentEncoding::UTF8) {
      bytes += "\xF0\x9F\x99\x82";
    } else if (width == 2) {
      appendUnit(0xd83d);
      appendUnit(0xde42);
    } else if (width == 4) {
      appendUnit(0x1f642);
    } else {
      appendUnit(encoding == DocumentEncoding::WINDOWS1251 ? 0xc6 : 0xe9);
    }
    for (const auto value : separator) {
      appendUnit(static_cast<unsigned char>(value));
    }
  }

  return bytes;
}
} // namespace

TEST_CASE("tail restart preserves encodings and CRLF across one-byte work and page seams") {
  const std::array encodings{DocumentEncoding::UTF8,       DocumentEncoding::UTF16LE, DocumentEncoding::UTF16BE,
                             DocumentEncoding::UTF32LE,    DocumentEncoding::UTF32BE, DocumentEncoding::WINDOWS1251,
                             DocumentEncoding::WINDOWS1252};

  for (const auto encoding : encodings) {
    for (std::size_t page = 1; page <= 4; ++page) {
      CAPTURE(encoding);
      CAPTURE(page);
      const bool unicode = encoding != DocumentEncoding::WINDOWS1251 && encoding != DocumentEncoding::WINDOWS1252;
      Fixture f(tailEncodedRows(encoding, "\r\n", unicode), page);
      f.intent.viewport = {40, 3};
      f.intent.config.workBytes = page;
      f.intent.config.decoder.workLimit = 1;
      if (!unicode) {
        f.intent.config.decoder.explicitEncoding = encoding;
      }
      f.intent.anchor = f.provider->text.size();
      f.intent.tail = true;

      const auto result = f.finish();

      REQUIRE_FALSE(result->error);
      REQUIRE(result->frame);
      REQUIRE(result->frame->rows.size() == 3);
      const auto glyph = encoding == DocumentEncoding::WINDOWS1251   ? "\xD0\x96"
                         : encoding == DocumentEncoding::WINDOWS1252 ? "\xC3\xA9"
                                                                     : "\xF0\x9F\x99\x82";
      CHECK(result->frame->rows.front().bytes() == std::string("row-3:") + glyph);
      CHECK(result->frame->rows.back().bytes() == std::string("row-5:") + glyph);
      CHECK(result->frame->finalEndVisible);
      CHECK_FALSE(result->frame->status.logicalLine);
      CHECK_FALSE(result->frame->status.totalLines);
    }
  }
}

TEST_CASE("tail preserves blank rows and terminal separators without a phantom row") {
  struct Case {
    std::string text;
    std::vector<std::string> rows;
    std::optional<std::uint64_t> firstLine;
  };

  const std::array cases{Case{"", {""}, 1},
                         Case{"\n", {""}, 1},
                         Case{"\r\n", {""}, 1},
                         Case{"\n\n\n\n", {"", "", ""}, {}},
                         Case{"a\r\n\r\nb\r\n\r\n", {"", "b", ""}, {}},
                         Case{"a\rb\rc\rd", {"b", "c", "d"}, {}},
                         Case{"a\nb\nc\n", {"a", "b", "c"}, 1}};

  for (const auto &entry : cases) {
    CAPTURE(entry.text);
    Fixture f(entry.text, 1);
    f.intent.viewport = {20, 3};
    f.intent.config.workBytes = 1;
    f.intent.anchor = entry.text.size();
    f.intent.tail = true;

    const auto result = f.finish();

    REQUIRE_FALSE(result->error);
    REQUIRE(result->frame);
    REQUIRE(result->frame->rows.size() == entry.rows.size());
    for (std::size_t row = 0; row < entry.rows.size(); ++row) {
      CHECK(result->frame->rows[row].bytes() == entry.rows[row]);
    }
    CHECK(result->frame->status.finalEnd);
    CHECK(result->frame->status.logicalLine == entry.firstLine);
  }
}

TEST_CASE("tail retries short backward prefixes without changing their order") {
  Fixture f("row\r\n");
  f.provider->syntheticExtent = 5 * 1024 * 1024;
  f.provider->maxReply = 3;
  f.intent.viewport = {40, 5};
  f.intent.config.workBytes = 512;
  f.intent.anchor = *f.provider->syntheticExtent;
  f.intent.tail = true;

  const auto result = f.finish();

  REQUIRE_FALSE(result->error);
  REQUIRE(result->frame);
  REQUIRE(result->frame->rows.size() == 5);
  CHECK(result->frame->rows.front().bytes() == "row");
  CHECK(result->frame->rows.back().bytes() == "row");
  CHECK(result->frame->status.visibleByteOffset == *f.provider->syntheticExtent - 25);
  CHECK(f.provider->admittedBytes.load() < 1024);
}

TEST_CASE("tail strict mode validates selected suffix without certifying omitted prefix") {
  std::string rows = "ok\n\xFF\n";
  for (unsigned row = 0; row < 20; ++row) {
    rows += "row-" + std::to_string(row) + "\n";
  }

  SUBCASE("the malformed omitted prefix does not prevent a valid suffix") {
    Fixture f(rows, 7);
    f.intent.viewport = {20, 3};
    f.intent.config.decoder.policy = DocumentDecodePolicy::STRICT_MODE;
    f.intent.anchor = rows.size();
    f.intent.tail = true;

    const auto result = f.finish();

    REQUIRE_FALSE(result->error);
    REQUIRE(result->frame);
    REQUIRE(result->frame->rows.size() == 3);
    CHECK(result->frame->rows.front().bytes() == "row-17");
    CHECK(result->frame->rows.back().bytes() == "row-19");
    CHECK_FALSE(result->frame->status.logicalLine);
  }

  SUBCASE("a malformed selected suffix remains a typed failure") {
    rows += "bad-\xFF\n";
    Fixture f(rows, 7);
    f.intent.viewport = {20, 3};
    f.intent.config.decoder.policy = DocumentDecodePolicy::STRICT_MODE;
    f.intent.anchor = rows.size();
    f.intent.tail = true;

    const auto result = f.finish();

    REQUIRE(result->error);
    CHECK(result->error->code == SourceError::INVALID_REPLY);
  }
}

TEST_CASE("tail encoding selection supports hints explicit choice and BOM-only sources") {
  SUBCASE("a hint resolves an unmarked UTF32 source") {
    Fixture f(tailEncodedRows(DocumentEncoding::UTF32BE, "\n", false), 3);
    f.intent.viewport = {40, 3};
    f.intent.config.decoder.hint = DocumentEncoding::UTF32BE;
    f.intent.config.workBytes = 2;
    f.intent.tail = true;
    f.intent.anchor = f.provider->text.size();
    const auto result = f.finish();
    REQUIRE_FALSE(result->error);
    REQUIRE(result->frame);
    REQUIRE(result->frame->rows.size() == 3);
    CHECK(result->frame->rows.front().bytes().starts_with("row-3:"));
    CHECK(result->frame->rows.back().bytes().starts_with("row-5:"));
  }

  SUBCASE("an explicit selection resolves an unmarked UTF16 source") {
    Fixture f(tailEncodedRows(DocumentEncoding::UTF16LE, "\r\n", false), 3);
    f.intent.viewport = {40, 3};
    f.intent.config.decoder.explicitEncoding = DocumentEncoding::UTF16LE;
    f.intent.config.workBytes = 2;
    f.intent.tail = true;
    f.intent.anchor = f.provider->text.size();
    const auto result = f.finish();
    REQUIRE_FALSE(result->error);
    REQUIRE(result->frame);
    REQUIRE(result->frame->rows.size() == 3);
    CHECK(result->frame->rows.front().bytes().starts_with("row-3:"));
    CHECK(result->frame->rows.back().bytes().starts_with("row-5:"));
  }

  SUBCASE("BOM-only sources keep the real empty row") {
    const std::array encodings{DocumentEncoding::UTF8, DocumentEncoding::UTF16LE, DocumentEncoding::UTF16BE,
                               DocumentEncoding::UTF32LE, DocumentEncoding::UTF32BE};
    for (const auto encoding : encodings) {
      CAPTURE(encoding);
      Fixture f(tailEncodedRows(encoding, "", true, 0), 1);
      f.intent.config.workBytes = 1;
      f.intent.tail = true;
      f.intent.anchor = f.provider->text.size();
      const auto result = f.finish();
      REQUIRE_FALSE(result->error);
      REQUIRE(result->frame);
      REQUIRE(result->frame->rows.size() == 1);
      CHECK(result->frame->rows.front().bytes().empty());
      CHECK(result->frame->status.finalEnd);
    }
  }
}

TEST_CASE("tail validates partial UTF code units at the original EOF alignment") {
  const std::array encodings{DocumentEncoding::UTF16LE, DocumentEncoding::UTF16BE, DocumentEncoding::UTF32LE,
                             DocumentEncoding::UTF32BE};
  const std::array policies{DocumentDecodePolicy::REPLACE, DocumentDecodePolicy::STRICT_MODE};

  for (const auto encoding : encodings) {
    for (const auto policy : policies) {
      CAPTURE(encoding);
      CAPTURE(policy);
      auto text = tailEncodedRows(encoding, "\n", true);
      text += 'x';
      Fixture f(text, 1);
      f.intent.viewport = {40, 3};
      f.intent.config.workBytes = 1;
      f.intent.config.decoder.policy = policy;
      f.intent.tail = true;
      f.intent.anchor = text.size();

      const auto result = f.finish();

      if (policy == DocumentDecodePolicy::STRICT_MODE) {
        REQUIRE(result->error);
        CHECK(result->error->code == SourceError::INVALID_REPLY);
      } else {
        REQUIRE_FALSE(result->error);
        REQUIRE(result->frame);
        REQUIRE(result->frame->rows.size() == 3);
        CHECK(result->frame->rows.front().bytes().starts_with("row-4:"));
        CHECK(result->frame->rows.back().bytes() == "\xEF\xBF\xBD");
      }
    }
  }
}

TEST_CASE("tail resumes waiting search and invalidates incompatible partial searches") {
  Fixture f("zero\none\ntwo\nthree\nfour\nfive\n", 1);
  f.intent.viewport = {20, 3};
  f.intent.config.workBytes = 1;
  f.intent.tail = true;
  f.intent.anchor = f.provider->text.size();
  std::shared_ptr<const detail::DocumentPreparation> searching;

  for (unsigned turn = 0; turn < DocumentDecoder::LOCAL_WINDOW_BYTES; ++turn) {
    f.intent.resume = searching != nullptr;
    searching = f.run(searching ? searching->scan : nullptr);
    REQUIRE_FALSE(searching->error);
    REQUIRE(searching->continueWork);
  }

  REQUIRE(searching->scan->tail);
  REQUIRE(searching->scan->tail->phase == detail::DocumentTailPhase::SEARCH);

  SUBCASE("waiting is not a final EOF and resumes the same search") {
    f.provider->temporaryEmpty = true;
    f.intent.resume = true;
    const auto waiting = f.run(searching->scan);
    REQUIRE_FALSE(waiting->error);
    REQUIRE(waiting->scan);
    CHECK(waiting->scan->waiting);
    REQUIRE(waiting->frame);
    CHECK_FALSE(waiting->frame->status.finalEnd);
    f.provider->temporaryEmpty = false;
    const auto result = f.finish(1000, waiting->scan);
    REQUIRE_FALSE(result->error);
    REQUIRE(result->frame);
    REQUIRE(result->frame->rows.size() == 3);
    CHECK(result->frame->rows.front().bytes() == "three");
    CHECK(result->frame->rows.back().bytes() == "five");
  }

  SUBCASE("a taller viewport invalidates a waiting suffix boundary") {
    for (unsigned turn = 0; turn < 100 && searching->scan->tail->phase != detail::DocumentTailPhase::RESTART; ++turn) {
      f.intent.resume = true;
      searching = f.run(searching->scan);
      REQUIRE_FALSE(searching->error);
      REQUIRE(searching->scan->tail);
    }

    REQUIRE(searching->scan->tail->phase == detail::DocumentTailPhase::RESTART);
    f.provider->temporaryEmpty = true;
    f.intent.resume = true;
    const auto waiting = f.run(searching->scan);
    REQUIRE(waiting->scan);
    REQUIRE(waiting->scan->waiting);
    f.provider->temporaryEmpty = false;
    f.intent.viewport.height = 5;

    const auto result = f.finish(1000, waiting->scan);

    REQUIRE_FALSE(result->error);
    REQUIRE(result->frame);
    REQUIRE(result->frame->rows.size() == 5);
    CHECK(result->frame->rows.front().bytes() == "one");
    CHECK(result->frame->rows.back().bytes() == "five");
  }

  SUBCASE("cancellation stops the partial worker without publishing EOF") {
    std::stop_source stop;
    stop.request_stop();
    f.intent.resume = true;
    const auto cancelled = f.run(searching->scan, stop.get_token());
    REQUIRE(cancelled->error);
    CHECK(cancelled->error->code == SourceError::CANCELLED);
    CHECK_FALSE(cancelled->frame);
  }

  SUBCASE("a revision change restarts origin proof before reading the new suffix") {
    ++f.provider->revision;
    f.provider->text = "new-a\nnew-b\nnew-c\nnew-d\n";
    f.provider->reads = 0;
    const auto result = f.finish(1000, searching->scan);
    REQUIRE_FALSE(result->error);
    REQUIRE(result->frame);
    REQUIRE(result->frame->rows.size() == 3);
    CHECK(f.provider->firstReadOffset.load() == 0);
    CHECK(result->frame->rows.front().bytes() == "new-b");
    CHECK(result->frame->rows.back().bytes() == "new-d");
  }
}

TEST_CASE("tail keeps the bounded fallback for unknown extents and forward-only sources") {
  SUBCASE("an unknown extent with a definitive final reply") {
    Fixture f("zero\none\ntwo\nthree\nfour\n", 3);
    f.provider->unknown = true;
    f.provider->finalUnknown = true;
    f.intent.viewport = {20, 3};
    f.intent.tail = true;
    const auto result = f.finish();
    REQUIRE_FALSE(result->error);
    REQUIRE(result->frame);
    REQUIRE(result->frame->rows.size() == 3);
    CHECK(result->frame->rows.front().bytes() == "two");
    CHECK(result->frame->rows.back().bytes() == "four");
    CHECK(result->frame->status.finalEnd);
  }

  SUBCASE("a known forward-only source") {
    Fixture f("zero\none\ntwo\nthree\nfour\n", 3, false);
    f.intent.viewport = {20, 3};
    f.intent.tail = true;
    f.intent.anchor = f.provider->text.size();
    const auto result = f.finish();
    REQUIRE_FALSE(result->error);
    REQUIRE(result->frame);
    REQUIRE(result->frame->rows.size() == 3);
    CHECK(result->frame->rows.front().bytes() == "two");
    CHECK(result->frame->rows.back().bytes() == "four");
    CHECK(result->frame->status.finalEnd);
  }
}

TEST_CASE("tail keeps the last visual rows when one logical suffix line wraps many times") {
  std::string text;
  for (unsigned row = 0; row < 10; ++row) {
    text += "prefix\n";
  }
  text += std::string(120, 'x') + "last";
  Fixture f(text, 3);
  f.intent.viewport = {10, 3};
  f.intent.config.workBytes = 7;
  f.intent.tail = true;
  f.intent.anchor = text.size();
  const auto result = f.finish();
  REQUIRE_FALSE(result->error);
  REQUIRE(result->frame);
  REQUIRE(result->frame->rows.size() == 3);
  CHECK(result->frame->rows[0].bytes() == std::string(10, 'x'));
  CHECK(result->frame->rows[1].bytes() == std::string(10, 'x'));
  CHECK(result->frame->rows.back().bytes() == "last");
  CHECK(result->frame->finalEndVisible);
}

TEST_CASE("HEX raw rows preserve BOM controls and absolute offsets with one-byte turns") {
  Fixture f(std::string("\xEF\xBB\xBF\x1B\0 A\xFF", 8), 1);
  f.intent.config.mode = DocumentViewMode::HEX;
  f.intent.config.workBytes = 1;
  f.intent.viewport = {100, 4};
  const auto result = f.finish();
  REQUIRE_FALSE(result->error);
  REQUIRE(result->frame);
  CHECK(result->frame->status.mode == DocumentViewMode::HEX);
  CHECK_FALSE(result->frame->status.logicalLine);
  CHECK_FALSE(result->frame->status.totalLines);
  REQUIRE(result->frame->rows.size() == 1);
  CHECK(result->frame->rows[0].bytes().starts_with("0000000000000000  EF BB BF 1B 00 20 41 FF"));
  CHECK(result->frame->rows[0].bytes().find("\x1B") == std::string_view::npos);
  CHECK(result->frame->visibleByteEnd == 8);
  CHECK(result->frame->finalEndVisible);
  CHECK(f.provider->admittedBytes.load() == 8);
  REQUIRE(result->scan->hex);
  CHECK(result->scan->hex->pending.empty());
}

TEST_CASE("HEX known End and E Eplus1 use a full last page without prefix reads") {
  constexpr std::uint64_t EXTENT = (std::uint64_t{8} << 30) + 19;
  for (const bool tail : {false, true}) {
    for (const auto anchor : {EXTENT, EXTENT + 1}) {
      Fixture f("z");
      f.provider->syntheticExtent = EXTENT;
      f.intent.viewport = {100, 4};
      f.intent.config.mode = DocumentViewMode::HEX;
      f.intent.anchor = anchor;
      f.intent.tail = tail;
      const auto result = f.finish();
      REQUIRE_FALSE(result->error);
      REQUIRE(result->frame);
      REQUIRE(result->frame->anchors.size() == 4);
      const auto first = (EXTENT / 16 + 1 - 4) * 16;
      CHECK(result->frame->anchors.front() == first);
      CHECK(f.provider->firstReadOffset.load() == first);
      CHECK(result->frame->rows.front().bytes().starts_with("00000001FFFFFFE0"));
      CHECK(result->frame->visibleByteEnd == EXTENT);
      CHECK(result->frame->finalEndVisible);
      CHECK(result->frame->status.byteOffset == (tail ? first : anchor));
      CHECK(result->frame->status.anchorBeyondExtent == (anchor > EXTENT));
      CHECK(f.provider->admittedBytes.load() <= 64);
    }
  }
}

TEST_CASE("HEX empty source remains empty and complete") {
  Fixture f("");
  f.intent.config.mode = DocumentViewMode::HEX;
  f.intent.tail = true;
  const auto result = f.finish();
  REQUIRE_FALSE(result->error);
  REQUIRE(result->frame);
  CHECK(result->frame->rows.empty());
  CHECK(result->frame->finalEndVisible);
  CHECK(result->frame->status.finalEnd);
  CHECK(f.provider->reads.load() == 0);
}

TEST_CASE("HEX unknown End retains only the bounded final row window") {
  Fixture f(std::string(81, 'Q'), 1);
  f.provider->unknown = true;
  f.provider->finalUnknown = true;
  f.intent.config.mode = DocumentViewMode::HEX;
  f.intent.config.workBytes = 3;
  f.intent.tail = true;
  f.intent.viewport = {100, 3};
  const auto result = f.finish();
  REQUIRE_FALSE(result->error);
  REQUIRE(result->frame);
  REQUIRE(result->frame->anchors.size() == 3);
  CHECK(result->frame->anchors.front() == 48);
  CHECK(result->frame->visibleByteEnd == 81);
  CHECK(result->frame->status.byteExtent == 81);
}

TEST_CASE("HEX fixed row cap is independent of viewport and auto-fit is optional") {
  Fixture f(std::string(5000, 'R'));
  f.intent.config.mode = DocumentViewMode::HEX;
  f.intent.config.hex.bytesPerRow = MAX_HEX_ROW_BYTES;
  f.intent.viewport = {80, 1};
  const auto fixed = f.finish();
  REQUIRE_FALSE(fixed->error);
  REQUIRE(fixed->frame);
  CHECK(fixed->frame->visibleByteEnd == MAX_HEX_ROW_BYTES);
  CHECK(fixed->frame->rows[0].measure() == 80);
  CHECK(fixed->scan->hex->bytesPerRow == MAX_HEX_ROW_BYTES);
  f.intent.config.hex.autoFit = true;
  const auto fitted = f.finish();
  REQUIRE_FALSE(fitted->error);
  REQUIRE(fitted->frame);
  CHECK(fitted->scan->hex->bytesPerRow < 16);
  CHECK(fitted->frame->hex->columns <= 80);
}

TEST_CASE("HEX preparation waiting is not EOF and revision changes discard partial rows") {
  Fixture f(std::string(70, 'S'), 1);
  f.intent.config.mode = DocumentViewMode::HEX;
  f.intent.config.workBytes = 1;
  f.intent.viewport = {100, 2};
  auto partial = f.run();
  REQUIRE_FALSE(partial->error);
  REQUIRE(partial->continueWork);
  REQUIRE(partial->scan->hex->pending.size() == 1);
  ++f.provider->revision;
  f.provider->text[0] = 'T';
  const auto next = f.finish(1000, partial->scan);
  REQUIRE_FALSE(next->error);
  REQUIRE(next->frame);
  CHECK(next->frame->rows.front().bytes().starts_with("0000000000000000  54 53"));
  f.provider->unknown = true;
  f.provider->temporaryEmpty = true;
  f.intent.resume = false;
  const auto waiting = f.finish();
  REQUIRE_FALSE(waiting->error);
  REQUIRE(waiting->frame);
  CHECK(waiting->frame->status.waiting);
  CHECK_FALSE(waiting->frame->status.finalEnd);
  CHECK_FALSE(waiting->continueWork);
  std::stop_source stop;
  stop.request_stop();
  const auto cancelled = f.run({}, stop.get_token());
  REQUIRE(cancelled->error);
  CHECK(cancelled->error->code == SourceError::CANCELLED);
}

static_assert(!std::is_nothrow_move_constructible_v<detail::DocumentHexRoleRow>);

TEST_CASE("TEXT scans and frames never allocate HEX extension owners") {
  Fixture f("plain text");
  const auto result = f.finish();
  REQUIRE_FALSE(result->error);
  REQUIRE(result->frame);
  CHECK_FALSE(result->scan->hex);
  CHECK_FALSE(result->frame->hex);
}

TEST_CASE("HEX unknown distant EOF does not invent an exact extent from an empty reply") {
  Fixture f("abc");
  f.provider->unknown = true;
  f.provider->finalUnknown = true;
  f.intent.config.mode = DocumentViewMode::HEX;
  f.intent.anchor = std::uint64_t{1} << 32;
  const auto result = f.finish();
  REQUIRE_FALSE(result->error);
  REQUIRE(result->frame);
  CHECK(result->frame->status.finalEnd);
  CHECK_FALSE(result->frame->status.byteExtent);
  CHECK(result->frame->rows.empty());
  CHECK(result->frame->footer->bytes().starts_with("progress unknown"));
}

TEST_CASE("forward HEX transient replay prefix owns subcredit and is evicted at accepted anchor") {
  Fixture f(std::string(1000, 'R'), 64 * 1024, false);
  f.intent.config.mode = DocumentViewMode::HEX;
  f.intent.viewport = {100, 4};
  auto initial = f.run();
  REQUIRE_FALSE(initial->error);
  std::weak_ptr<const detail::DocumentHexRawRow> prefix = initial->scan->hex->rawRows.front();
  f.intent.rowDelta = 1;
  f.intent.resume = false;
  auto moved = f.run(initial->scan);
  REQUIRE_FALSE(moved->error);
  REQUIRE(moved->frame);
  CHECK(moved->frame->status.byteOffset == 16);
  CHECK(moved->scan->hex->rawRows.front()->offset == 0);
  CHECK(moved->scan->hex->rawRows.size() == 5);
  f.intent.anchor = 16;
  f.intent.rowDelta = 0;
  f.intent.resume = false;
  const auto accepted = f.run(moved->scan);
  REQUIRE_FALSE(accepted->error);
  REQUIRE(accepted->frame);
  CHECK(accepted->scan->hex->rawRows.front()->offset == 16);
  CHECK(accepted->scan->hex->rawRows.size() == 4);
  initial.reset();
  moved.reset();
  CHECK(prefix.expired());
}

TEST_CASE("HEX accounting charges only bounded prepared columns and preserves surface limits") {
  DocumentViewConfig config;
  config.mode = DocumentViewMode::HEX;
  const auto columns = take(hexRowWidth({}));
  const auto wide = take(detail::documentPreparationBytes({190, 50}, config));
  const auto exact = take(detail::documentPreparationBytes({static_cast<std::int32_t>(columns), 50}, config));
  CHECK(wide == exact);
  config.maxFrameCells = 100;
  const auto tooManyCells = detail::documentPreparationBytes({190, 50}, config);
  REQUIRE_FALSE(tooManyCells);
  CHECK(tooManyCells.error() == SourceError::PAGE_TOO_LARGE);
  config.maxFrameCells = 16384;
  const auto overflow =
    detail::documentPreparationBytes({190, 50}, config, {.hexRows = std::numeric_limits<std::size_t>::max()});
  REQUIRE_FALSE(overflow);
  CHECK(overflow.error() == SourceError::PAGE_TOO_LARGE);
  config.hex = {.bytesPerRow = MAX_HEX_ROW_BYTES, .autoFit = true};
  const auto fitted = take(detail::documentHexRowOptions(config.hex, 30));
  REQUIRE(fitted.bytesPerRow < MAX_HEX_ROW_BYTES);
  config.hex.bytesPerRow = fitted.bytesPerRow;
  config.hex.autoFit = false;
  const auto fixed = take(detail::documentPreparationBytes({30, 4}, config));
  config.hex.bytesPerRow = MAX_HEX_ROW_BYTES;
  config.hex.autoFit = true;
  CHECK(take(detail::documentPreparationBytes({30, 4}, config)) == fixed);
}

TEST_CASE("HEX horizontal and auto-fit maps and run capacities follow prepared row bytes") {
  std::string bytes(0, '\0');
  for (unsigned repeat = 0; repeat < 32; ++repeat) {
    bytes.append("\0A\x1B\xFF", 4);
  }
  Fixture f(bytes);
  f.intent.config.mode = DocumentViewMode::HEX;
  f.intent.viewport = {190, 4};
  for (const bool autoFit : {false, true}) {
    f.intent.config.hex = {.bytesPerRow = autoFit ? MAX_HEX_ROW_BYTES : 16, .autoFit = autoFit};
    f.intent.viewport.width = autoFit ? 30 : 190;
    f.intent.horizontal = autoFit ? 5 : 70;
    const auto result = f.finish();
    REQUIRE_FALSE(result->error);
    REQUIRE(result->frame);
    const auto options =
      take(detail::documentHexRowOptions(f.intent.config.hex, static_cast<std::size_t>(f.intent.viewport.width)));
    const auto width = take(hexRowWidth(options));
    const auto first = static_cast<std::size_t>(std::min<std::uint64_t>(f.intent.horizontal, width));
    const auto count = std::min(static_cast<std::size_t>(f.intent.viewport.width), width - first);
    for (std::size_t row = 0; row < result->frame->rows.size(); ++row) {
      CHECK(result->scan->rows[row].bytes.size() == count);
      const auto &roles = result->scan->hex->roles[row].columns;
      CHECK(roles.size() == count);
      CHECK(roles.capacity() <= count);
      const auto &runs = result->frame->hex->rows[row];
      CHECK(runs.capacity() <= count);
      CHECK(result->frame->rows[row].graphemes().size() == count);
      std::size_t classified{};
      for (const auto &run : runs) {
        classified += run.text.graphemes().size();
      }
      CHECK(classified <= count);
    }
  }
  f.intent.config.maxFrameBytes = 20;
  const auto payload = f.finish();
  REQUIRE(payload->error);
  CHECK(payload->error->code == SourceError::PAGE_TOO_LARGE);
}

TEST_CASE("SQL syntax follows a plain first frame and preserves multiline context on cold seek") {
  Fixture f("/* opening\ninside comment\nclosing */\nSELECT 42, 'value';\n", 7);
  f.intent.config.language = DocumentLanguage::POSTGRESQL_LEXICAL;
  f.intent.config.syntax = DocumentSyntaxPolicy::EXACT;
  f.intent.viewport = {40, 2};
  f.intent.anchor = 11;
  std::shared_ptr<const detail::DocumentScan> scan;
  std::shared_ptr<const detail::DocumentPreparedFrame> plain;
  for (std::size_t turn = 0; turn < 100 && !plain; ++turn) {
    f.intent.resume = scan != nullptr;
    const auto result = f.run(scan);
    REQUIRE_FALSE(result->error);
    scan = result->scan;
    plain = result->frame;
  }
  REQUIRE(plain);
  CHECK_FALSE(plain->text);
  CHECK(plain->status.highlighting == DocumentSyntaxState::PENDING);
  f.intent.kind = detail::DocumentPreparationKind::HIGHLIGHT;
  const auto colored = f.finish(100, scan);
  REQUIRE_FALSE(colored->error);
  REQUIRE(colored->frame);
  REQUIRE(colored->frame->text);
  CHECK(colored->frame->status.highlighting == DocumentSyntaxState::READY);
  REQUIRE_FALSE(colored->frame->text->rows[0].empty());
  CHECK(colored->frame->text->rows[0][0].role == SemanticRole::COMMENT);
  CHECK(colored->frame->rows[0].bytes() == plain->rows[0].bytes());
}

TEST_CASE("Markdown syntax includes headings and SQL fenced source") {
  Fixture f("# Guide\n\n```sql\nSELECT 42;\n```\n", 1);
  f.intent.config.language = DocumentLanguage::MARKDOWN_SOURCE;
  f.intent.viewport = {40, 8};
  const auto result = f.finish();
  REQUIRE_FALSE(result->error);
  REQUIRE(result->frame);
  REQUIRE(result->frame->text);
  CHECK(result->frame->status.highlighting == DocumentSyntaxState::READY);
  REQUIRE_FALSE(result->frame->text->rows[0].empty());
  CHECK(result->frame->text->rows[0][0].role == SemanticRole::HEADING);
  REQUIRE_FALSE(result->frame->text->rows[3].empty());
  CHECK(result->frame->text->rows[3][0].role == SemanticRole::KEYWORD);
}

TEST_CASE("UTF16 syntax maps original bytes to a combining grapheme and escaped control") {
  std::string text("\xff\xfe", 2);
  for (const auto scalar : std::u16string_view(u"SELECT 'a\u0301\x001b';\n")) {
    text.push_back(static_cast<char>(scalar & 255));
    text.push_back(static_cast<char>(scalar >> 8));
  }
  Fixture f(text, 1);
  f.intent.config.language = DocumentLanguage::POSTGRESQL_LEXICAL;
  f.intent.config.decoder.explicitEncoding = DocumentEncoding::UTF16LE;
  f.intent.viewport = {40, 4};
  const auto result = f.finish();
  REQUIRE_FALSE(result->error);
  REQUIRE(result->frame);
  REQUIRE(result->frame->text);
  CHECK(result->frame->status.highlighting == DocumentSyntaxState::READY);
  const auto &row = result->frame->rows[0];
  const auto &runs = result->frame->text->rows[0];
  REQUIRE_FALSE(runs.empty());
  CHECK(runs[0].role == SemanticRole::KEYWORD);
  CHECK(row.bytes().find("a\xcc\x81") != std::string_view::npos);
  CHECK(row.bytes().find('\x1b') == std::string_view::npos);
  bool stringFound{};
  for (const auto &run : runs) {
    stringFound = stringFound || run.role == SemanticRole::STRING;
    CHECK(run.first + run.count <= row.graphemes().size());
  }
  CHECK(stringFound);
}

TEST_CASE("AUTO cold fragments stay provisional until explicit exact context replaces them") {
  std::string opening;
  std::string closing;
  auto language = DocumentLanguage::POSTGRESQL_LEXICAL;
  auto expected = SemanticRole::COMMENT;

  SUBCASE("SQL comment") {
    opening = "/*\n";
    closing = "*/\n";
  }
  SUBCASE("SQL string") {
    opening = "'";
    closing = "';\n";
    expected = SemanticRole::STRING;
  }
  SUBCASE("Markdown SQL fence") {
    opening = "```sql\n";
    closing = "```\n";
    language = DocumentLanguage::MARKDOWN_SOURCE;
    expected = SemanticRole::KEYWORD;
  }

  const auto prefix = opening + std::string(12 * 1024, 'x') + "\n";
  Fixture f(prefix + "SELECT 42;\n" + closing, 127);
  f.intent.config.language = language;
  f.intent.config.maxAutoHighlightBytes = 31;
  f.intent.anchor = prefix.size();
  f.intent.viewport = {40, 1};
  const auto provisional = f.layout();
  REQUIRE_FALSE(provisional->error);
  REQUIRE(provisional->frame);
  REQUIRE(provisional->frame->text);
  CHECK(provisional->frame->status.syntaxAccuracy == DocumentSyntaxAccuracy::PREVIEW);
  CHECK(provisional->frame->status.highlighting == DocumentSyntaxState::PENDING);
  REQUIRE(provisional->frame->footer);
  CHECK(provisional->frame->footer->bytes().ends_with(" ~"));
  REQUIRE(provisional->scan->highlight->preview);
  const auto &highlight = *provisional->scan->highlight;
  CHECK(highlight.checkpointCount == 0);
  CHECK_FALSE(highlight.started);
  CHECK_FALSE(highlight.replay);
  for (const auto &map : highlight.maps) {
    CHECK_FALSE(map.classified);
  }

  const auto admitted = f.provider->admittedBytes.load();
  f.intent.kind = detail::DocumentPreparationKind::HIGHLIGHT;
  f.intent.resume = true;
  auto stopped = f.run(provisional->scan);
  REQUIRE_FALSE(stopped->error);
  CHECK_FALSE(stopped->continueWork);
  CHECK(stopped->syntax == DocumentSyntaxState::PREVIEW);
  CHECK(stopped->syntaxAccuracy == DocumentSyntaxAccuracy::PREVIEW);
  CHECK(f.provider->admittedBytes.load() == admitted);
  CHECK(stopped->scan->highlight->preview != provisional->scan->highlight->preview);

  f.intent.config.syntax = DocumentSyntaxPolicy::EXACT;
  f.intent.resume = false;
  const auto retained = f.layout(stopped->scan);
  REQUIRE_FALSE(retained->error);
  REQUIRE(retained->frame);
  CHECK(retained->frame->status.syntax == DocumentSyntaxPolicy::EXACT);
  CHECK(retained->frame->status.syntaxAccuracy == DocumentSyntaxAccuracy::PREVIEW);
  CHECK(retained->frame->rows[0].bytes() == provisional->frame->rows[0].bytes());

  // The UI releases delivered preparation envelopes; frames and scans retain
  // their own independent credits. Do not pin an obsolete work slot here.
  stopped.reset();

  f.intent.kind = detail::DocumentPreparationKind::HIGHLIGHT;
  const auto exact = f.finish(1000, retained->scan);
  if (exact->error) {
    INFO("exact source error code=" << static_cast<int>(exact->error->code));
    REQUIRE_FALSE(exact->error);
  }
  REQUIRE_FALSE(exact->error);
  REQUIRE(exact->frame);
  REQUIRE(exact->frame->text);
  CHECK(exact->frame->status.highlighting == DocumentSyntaxState::READY);
  CHECK(exact->frame->status.syntaxAccuracy == DocumentSyntaxAccuracy::EXACT);
  REQUIRE_FALSE(exact->frame->text->rows[0].empty());
  CHECK(exact->frame->text->rows[0][0].role == expected);
  CHECK(exact->frame->rows[0].bytes() == provisional->frame->rows[0].bytes());
  CHECK(exact->frame->anchors == provisional->frame->anchors);
  REQUIRE(exact->frame->footer);
  CHECK_FALSE(exact->frame->footer->bytes().ends_with(" ~"));
  CHECK(provisional->frame->status.syntaxAccuracy == DocumentSyntaxAccuracy::PREVIEW);
}

TEST_CASE("AUTO admission caps the final request and survives repeated same-window staging") {
  std::size_t page = 4;
  SUBCASE("budget smaller than a page") {
    page = 64;
  }
  SUBCASE("budget not divisible by page") {
    page = 4;
  }

  Fixture f("SELECTxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx;\n", page);
  f.intent.config.language = DocumentLanguage::POSTGRESQL_LEXICAL;
  f.intent.config.maxAutoHighlightBytes = 5;
  f.intent.viewport = {4, 1};
  const auto provisional = f.layout();
  REQUIRE_FALSE(provisional->error);
  REQUIRE(provisional->frame);
  CHECK(provisional->frame->status.syntaxAccuracy == DocumentSyntaxAccuracy::PREVIEW);
  const auto admitted = f.provider->admittedBytes.load();
  f.intent.kind = detail::DocumentPreparationKind::HIGHLIGHT;
  f.intent.resume = true;
  auto stopped = f.run(provisional->scan);
  REQUIRE_FALSE(stopped->error);
  CHECK_FALSE(stopped->continueWork);
  CHECK(stopped->syntax == DocumentSyntaxState::PREVIEW);
  CHECK(f.provider->admittedBytes.load() - admitted == 5);
  REQUIRE(stopped->scan->highlight->attempt);
  CHECK(stopped->scan->highlight->attempt->remaining == 0);
  CHECK(provisional->scan->highlight->attempt->remaining == 5);

  for (std::size_t retry = 0; retry < 3; ++retry) {
    stopped = f.run(stopped->scan);
    REQUIRE_FALSE(stopped->error);
    CHECK_FALSE(stopped->continueWork);
    CHECK(stopped->syntax == DocumentSyntaxState::PREVIEW);
    CHECK(f.provider->admittedBytes.load() - admitted == 5);
  }

  f.intent.anchor = 1;
  f.intent.resume = false;
  const auto navigated = f.layout(stopped->scan);
  REQUIRE_FALSE(navigated->error);
  REQUIRE(navigated->scan->highlight->attempt);
  CHECK(navigated->scan->highlight->attempt->remaining == 5);
}

TEST_CASE("PREVIEW maps original UTF32 scalars before escaped display expansion") {
  std::string text("\xff\xfe\0\0", 4);
  for (const auto scalar : std::u32string_view(U"SELECT 'a\u0301\x001b';\n")) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
      text.push_back(static_cast<char>((scalar >> shift) & 255));
    }
  }
  Fixture f(text, 7);
  f.intent.config.language = DocumentLanguage::POSTGRESQL_LEXICAL;
  f.intent.config.syntax = DocumentSyntaxPolicy::PREVIEW;
  f.intent.config.decoder.explicitEncoding = DocumentEncoding::UTF32LE;
  f.intent.viewport = {40, 2};
  const auto result = f.finish();
  REQUIRE_FALSE(result->error);
  REQUIRE(result->frame);
  REQUIRE(result->frame->text);
  CHECK(result->frame->status.highlighting == DocumentSyntaxState::PREVIEW);
  CHECK(result->frame->status.syntaxAccuracy == DocumentSyntaxAccuracy::PREVIEW);
  REQUIRE_FALSE(result->frame->text->rows[0].empty());
  CHECK(result->frame->text->rows[0][0].role == SemanticRole::KEYWORD);
  CHECK(result->frame->rows[0].bytes().find("a\xcc\x81") != std::string_view::npos);
  CHECK(result->frame->rows[0].bytes().find("\\u001B") != std::string_view::npos);
  bool stringFound{};
  for (const auto &run : result->frame->text->rows[0]) {
    stringFound = stringFound || run.role == SemanticRole::STRING;
    CHECK(run.first + run.count <= result->frame->rows[0].graphemes().size());
  }
  CHECK(stringFound);
  for (const auto &point : result->scan->highlight->preview->maps) {
    CHECK(point.sourceOffset % 4 == 0);
  }
}

TEST_CASE("PREVIEW tail ASCII shortcut retains a bounded classified suffix") {
  Fixture f(std::string(32 * 1024, 'x') + " SELECT 42;", 4096);
  f.intent.config.language = DocumentLanguage::POSTGRESQL_LEXICAL;
  f.intent.config.syntax = DocumentSyntaxPolicy::PREVIEW;
  f.intent.tail = true;
  f.intent.viewport = {20, 2};
  const auto result = f.finish();
  REQUIRE_FALSE(result->error);
  REQUIRE(result->frame);
  REQUIRE(result->frame->text);
  CHECK(result->frame->status.syntaxAccuracy == DocumentSyntaxAccuracy::PREVIEW);
  REQUIRE(result->scan->highlight->preview);
  const auto &highlight = *result->scan->highlight;
  CHECK(highlight.preview->maps.size() <= highlight.maxMaps);
  CHECK(highlight.preview->maps.capacity() <= highlight.maxMaps);
  CHECK(highlight.checkpointCount == 0);
  bool keyword{};
  for (const auto &row : result->frame->text->rows) {
    for (const auto &run : row) {
      keyword = keyword || run.role == SemanticRole::KEYWORD;
    }
  }
  CHECK(keyword);
}

TEST_CASE("bounded tail preview matches exact text anchors and grapheme roles across pages") {
  constexpr std::size_t PAGE_BOUNDARY = 1024 * 1024;
  std::string text;
  bool cutoffInsideCluster{};

  SUBCASE("printable ASCII with an extender across a page seam") {
    const std::string suffix = " SELECT 'a";
    // The final ASCII scalar of this page joins a UTF-8 extender on the next.
    text = std::string(PAGE_BOUNDARY - suffix.size(), 'x') + suffix + "\xcc\x81';";
  }
  SUBCASE("short ASCII runs with a cutoff inside a multi-scalar grapheme") {
    for (std::size_t cluster = 0; cluster < PAGE_BOUNDARY / 3; ++cluster) {
      text += "a\xcc\x81";
    }
    text += " SELECT 'a\xcc\x81';";
    cutoffInsideCluster = true;
  }

  Fixture automatic(text, 4096);
  automatic.intent.config.language = DocumentLanguage::POSTGRESQL_LEXICAL;
  automatic.intent.config.maxFrameBytes = 4096;
  automatic.intent.config.workBytes = 4 * PAGE_BOUNDARY;
  automatic.intent.config.maxAutoHighlightBytes = 1;
  automatic.intent.tail = true;
  automatic.intent.viewport = {20, 3};
  const auto preview = automatic.layout();
  REQUIRE_FALSE(preview->error);
  REQUIRE(preview->frame);
  REQUIRE(preview->frame->text);
  CHECK(preview->frame->status.syntaxAccuracy == DocumentSyntaxAccuracy::PREVIEW);
  CHECK(preview->frame->status.finalEnd);
  CHECK(preview->frame->finalEndVisible);
  CHECK(preview->frame->visibleByteEnd == text.size());
  REQUIRE(preview->scan->highlight->preview);
  const auto &highlight = *preview->scan->highlight;
  REQUIRE(highlight.previewOriginalByteCutoff > 0);
  CHECK(highlight.preview->fragmentBase >= highlight.previewOriginalByteCutoff);
  CHECK(highlight.preview->lexer.nextByteOffset() <= text.size() - highlight.previewOriginalByteCutoff);
  for (const auto &map : highlight.maps) {
    CHECK(map.sourceOffset >= highlight.previewOriginalByteCutoff);
  }
  if (cutoffInsideCluster) {
    CHECK(highlight.previewOriginalByteCutoff % 3 == 1);
    CHECK(static_cast<unsigned char>(text[highlight.previewOriginalByteCutoff]) == 0xcc);
  }
  CHECK(highlight.maps.size() <= highlight.maxMaps);
  CHECK(highlight.preview->maps.size() <= highlight.maxMaps);
  CHECK(highlight.preview->maps.capacity() <= highlight.maxMaps);

  Fixture explicitExact(text, 4096);
  explicitExact.intent = automatic.intent;
  explicitExact.intent.config.syntax = DocumentSyntaxPolicy::EXACT;
  explicitExact.intent.resume = false;
  const auto layout = explicitExact.layout();
  REQUIRE_FALSE(layout->error);
  REQUIRE(layout->frame);
  CHECK_FALSE(layout->scan->highlight->preview);
  CHECK(layout->scan->highlight->previewOriginalByteCutoff == 0);
  explicitExact.intent.kind = detail::DocumentPreparationKind::HIGHLIGHT;
  const auto exact = explicitExact.finish(1000, layout->scan);
  REQUIRE_FALSE(exact->error);
  REQUIRE(exact->frame);
  REQUIRE(exact->frame->text);
  CHECK(exact->frame->status.syntaxAccuracy == DocumentSyntaxAccuracy::EXACT);
  CHECK(exact->frame->status.finalEnd);
  CHECK(exact->frame->finalEndVisible);
  CHECK(exact->frame->visibleByteEnd == preview->frame->visibleByteEnd);
  CHECK(exact->frame->anchors == preview->frame->anchors);
  REQUIRE(exact->frame->rows.size() == preview->frame->rows.size());

  const auto roleAt = [](const detail::DocumentTextFrame &projection, std::size_t row, std::size_t cluster) {
    for (const auto &run : projection.rows[row]) {
      if (cluster >= run.first && cluster - run.first < run.count) {
        return run.role;
      }
    }
    return SemanticRole::NORMAL;
  };

  for (std::size_t row = 0; row < exact->frame->rows.size(); ++row) {
    CHECK(exact->frame->rows[row].bytes() == preview->frame->rows[row].bytes());
    for (std::size_t cluster = 0; cluster < exact->frame->rows[row].graphemes().size(); ++cluster) {
      CHECK(roleAt(*exact->frame->text, row, cluster) == roleAt(*preview->frame->text, row, cluster));
    }
  }
  CHECK(preview->frame->rows.back().bytes().find("a\xcc\x81") != std::string_view::npos);
}

TEST_CASE("forward-only known-origin syntax remains exact with no automatic reread budget") {
  Fixture f("/* comment\nSELECT hidden;\n*/\nSELECT 42;\n", 7, false);
  f.intent.config.language = DocumentLanguage::POSTGRESQL_LEXICAL;
  f.intent.config.maxAutoHighlightBytes = 1;
  f.intent.viewport = {40, 4};
  const auto result = f.finish();
  REQUIRE_FALSE(result->error);
  REQUIRE(result->frame);
  REQUIRE(result->frame->text);
  CHECK(result->frame->status.highlighting == DocumentSyntaxState::READY);
  CHECK(result->frame->status.syntaxAccuracy == DocumentSyntaxAccuracy::EXACT);
  CHECK_FALSE(result->scan->highlight->preview);
  CHECK(f.provider->admittedBytes.load() == f.provider->text.size());
}

TEST_CASE("optional exact source failure preserves the immutable preview frame and accuracy") {
  Fixture f("SELECT 42;\n", 7);
  f.intent.config.language = DocumentLanguage::POSTGRESQL_LEXICAL;
  const auto provisional = f.layout();
  REQUIRE_FALSE(provisional->error);
  REQUIRE(provisional->frame);
  REQUIRE(provisional->frame->text);
  f.provider->failRead = true;
  f.intent.kind = detail::DocumentPreparationKind::HIGHLIGHT;
  f.intent.resume = true;
  const auto failed = f.run(provisional->scan);
  REQUIRE(failed->error);
  CHECK(failed->error->code == SourceError::IO_ERROR);
  CHECK(failed->baseFrame == provisional->frame);
  CHECK(failed->syntaxAccuracy == DocumentSyntaxAccuracy::PREVIEW);
  REQUIRE(failed->scan);
  CHECK(failed->scan->highlight->plain == provisional->frame);
  CHECK(provisional->frame->status.syntaxAccuracy == DocumentSyntaxAccuracy::PREVIEW);
  CHECK(provisional->frame->rows[0].bytes() == "SELECT 42;");
}

TEST_CASE("optional preview admission can be suppressed without changing syntax policy") {
  Fixture f("SELECT 42;\n", 7);
  f.intent.config.language = DocumentLanguage::POSTGRESQL_LEXICAL;
  f.intent.config.syntax = DocumentSyntaxPolicy::PREVIEW;
  const auto normal = take(detail::documentPreparationBytes(f.intent.viewport, f.intent.config));
  const auto suppressed = take(
    detail::documentPreparationBytes(f.intent.viewport, f.intent.config, {.preview = true, .suppressPreview = true}));
  CHECK(suppressed < normal);
  f.intent.suppressPreview = true;
  const auto plain = f.finish();
  REQUIRE_FALSE(plain->error);
  REQUIRE(plain->frame);
  CHECK(plain->frame->rows[0].bytes() == "SELECT 42;");
  CHECK_FALSE(plain->frame->text);
  CHECK_FALSE(plain->scan->highlight->preview);
  CHECK(plain->frame->status.syntax == DocumentSyntaxPolicy::PREVIEW);
  CHECK(plain->frame->status.syntaxAccuracy == DocumentSyntaxAccuracy::NONE);
  CHECK(plain->frame->status.highlighting == DocumentSyntaxState::RESOURCE_LIMIT);
  CHECK_FALSE(plain->continueWork);
}

TEST_CASE("retained preview roles require the current horizontal source mappings") {
  Fixture f("SELECT 42, 'value';\n", 7);
  f.intent.config.language = DocumentLanguage::POSTGRESQL_LEXICAL;
  f.intent.config.wrap = false;
  f.intent.viewport = {6, 1};
  const auto provisional = f.layout();
  REQUIRE_FALSE(provisional->error);
  REQUIRE(provisional->frame);
  CHECK(provisional->frame->status.syntaxAccuracy == DocumentSyntaxAccuracy::PREVIEW);
  f.intent.config.syntax = DocumentSyntaxPolicy::EXACT;
  f.intent.horizontal = 11;
  f.intent.resume = false;
  const auto moved = f.layout(provisional->scan);
  REQUIRE_FALSE(moved->error);
  REQUIRE(moved->frame);
  CHECK(moved->frame->rows[0].bytes() == "'value");
  CHECK(moved->frame->status.syntaxAccuracy == DocumentSyntaxAccuracy::NONE);
  CHECK_FALSE(moved->frame->text);
  REQUIRE(moved->scan->highlight->preview);
  CHECK(moved->scan->highlight->preview == provisional->scan->highlight->preview);
}

TEST_CASE("ordinary seekable scrolling preserves proven exact roles in viewport overlap") {
  const auto prefix = std::string("/*\n") + std::string(12 * 1024, 'x') + "\n";
  std::string text = prefix;
  for (std::size_t row = 0; row < 20; ++row) {
    text += "SELECT 42;\n";
  }
  text += "*/\n";
  Fixture f(text, 127);
  f.intent.config.language = DocumentLanguage::POSTGRESQL_LEXICAL;
  f.intent.anchor = prefix.size();
  f.intent.viewport = {20, 3};
  const auto exact = f.finish();
  REQUIRE_FALSE(exact->error);
  REQUIRE(exact->frame);
  REQUIRE(exact->frame->text);
  CHECK(exact->frame->status.syntaxAccuracy == DocumentSyntaxAccuracy::EXACT);
  CHECK(exact->frame->text->rows[0][0].role == SemanticRole::COMMENT);

  // Rebuilding the same projection needs no new lexical recovery, even when
  // the remaining automatic budget cannot restore this distant comment.
  f.intent.resume = false;
  f.intent.config.maxAutoHighlightBytes = 1;
  const auto reused = f.layout(exact->scan);
  REQUIRE_FALSE(reused->error);
  REQUIRE(reused->frame);
  CHECK_FALSE(reused->continueWork);
  CHECK(reused->frame->status.highlighting == DocumentSyntaxState::READY);
  CHECK(reused->frame->status.syntaxAccuracy == DocumentSyntaxAccuracy::EXACT);

  f.intent.resume = false;
  f.intent.anchor = exact->frame->anchors[1];
  const auto scrolled = f.layout(reused->scan);
  REQUIRE_FALSE(scrolled->error);
  REQUIRE(scrolled->frame);
  REQUIRE(scrolled->frame->text);
  REQUIRE(scrolled->frame->text->rows.size() == 3);
  CHECK(scrolled->frame->status.syntaxAccuracy == DocumentSyntaxAccuracy::PREVIEW);
  CHECK(scrolled->frame->status.highlighting == DocumentSyntaxState::PENDING);
  for (std::size_t row = 0; row < 2; ++row) {
    CHECK(scrolled->frame->rows[row].bytes() == exact->frame->rows[row + 1].bytes());
    REQUIRE_FALSE(scrolled->frame->text->rows[row].empty());
    CHECK(scrolled->frame->text->rows[row][0].role == SemanticRole::COMMENT);
  }
  REQUIRE_FALSE(scrolled->frame->text->rows[2].empty());
  CHECK(scrolled->frame->text->rows[2][0].role == SemanticRole::KEYWORD);
  CHECK(exact->frame->text->rows[0][0].role == SemanticRole::COMMENT);
  bool unresolved{};
  for (const auto &point : scrolled->scan->highlight->maps) {
    unresolved = unresolved || !point.classified;
  }
  CHECK(unresolved);
}
