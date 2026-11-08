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
    const auto amount = take(detail::documentPreparationBytes(intent.viewport, intent.config));
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
    }
    FAIL("bounded test turn limit reached");
    return result;
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
