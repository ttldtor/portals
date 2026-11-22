// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/document_export.hpp>
#include <portals/document_search.hpp>
#include <portals/gzip_document.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <future>
#include <limits>

namespace {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> value) {
  REQUIRE(value);
  return std::move(*value);
}

template <class F> auto worker(F &&function) {
  return std::async(std::launch::async, std::forward<F>(function)).get();
}

std::uint32_t checksum(std::string_view bytes) {
  std::uint32_t crc = 0xffffffff;
  for (const auto byte : bytes) {
    crc ^= static_cast<unsigned char>(byte);
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
  }
  return ~crc;
}

void append32(std::string &bytes, std::uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8) {
    bytes.push_back(static_cast<char>((value >> shift) & 255));
  }
}

// Deterministic gzip with stored deflate blocks; tests need no compressor dependency.
std::string stored(std::string_view bytes) {
  std::string result("\x1f\x8b\x08\0\0\0\0\0\0\xff", 10);
  std::size_t offset{};
  do {
    const auto count = std::min<std::size_t>(65535, bytes.size() - offset);
    result.push_back(offset + count == bytes.size() ? '\x01' : '\0');
    for (const auto length : {static_cast<std::uint16_t>(count), static_cast<std::uint16_t>(~count)}) {
      result.push_back(static_cast<char>(length & 255));
      result.push_back(static_cast<char>(length >> 8));
    }
    result.append(bytes.substr(offset, count));
    offset += count;
  } while (offset < bytes.size());
  append32(result, checksum(bytes));
  append32(result, static_cast<std::uint32_t>(bytes.size()));
  return result;
}

std::string fromHex(std::string_view hex) {
  const auto nibble = [](char value) {
    return value <= '9' ? value - '0' : value - 'a' + 10;
  };
  std::string bytes;
  for (std::size_t index = 0; index < hex.size(); index += 2) {
    bytes.push_back(static_cast<char>((nibble(hex[index]) << 4) | nibble(hex[index + 1])));
  }
  return bytes;
}

std::string repeatedText() {
  std::string result;
  for (std::size_t row = 0; row < 4096; ++row) {
    result += "abcde0123456789\r\n";
  }
  return result;
}

std::string repeatedGzip() {
  // gzip.compress(b"abcde0123456789\r\n"*4096, mtime=0), with dynamic Huffman/back-references.
  return fromHex("1f8b08000000000002ffedc8c90d80200000b0bf09bb28a0c2385cfbafc01ea47db6f531d7fdc494dfef2f355c"
                 "4d0821841042082184104208218410420821841042082184104208218410420821841042082184104208218410"
                 "420821841042082184104208218410420821841042082184104208218410420821841042082184104208218410"
                 "420821841042082184104208218410420821841042082184104208218410420821841042082184104208218410"
                 "420821841042082184104208218410420821841042082184104208218410420821c489b1010f606d1c00100100");
}

class Input final : public ByteDocumentProvider {
public:
  std::string bytes;
  std::uint64_t revision{1};
  std::size_t metadataCalls{};
  std::size_t reads{};
  std::size_t originReads{};
  std::uint64_t readBytes{};
  std::size_t maxReply{std::numeric_limits<std::size_t>::max()};
  std::size_t largestDestination{};
  bool knownExtent{true};
  bool seekable{true};
  bool mutateDuringRead{};
  std::stop_source *cancelDuringRead{};
  std::stop_source *cancelAfterMetadata{};
  std::size_t cancelMetadataCall{};
  std::promise<void> *blockEntered{};
  std::shared_future<void> continueRead;

  ByteDocumentCapabilities capabilities() const noexcept override {
    return {seekable, false};
  }

  std::expected<ByteDocumentMetadata, DocumentError> metadata(std::stop_token) override {
    ++metadataCalls;
    if (cancelAfterMetadata && metadataCalls == cancelMetadataCall) {
      cancelAfterMetadata->request_stop();
    }
    return ByteDocumentMetadata{revision, knownExtent ? std::optional<std::uint64_t>(bytes.size()) : std::nullopt};
  }

  std::expected<ByteReadResult, DocumentError> read(const ByteReadCommand &command, std::span<std::byte> destination,
                                                    std::stop_token) override {
    ++reads;
    originReads += command.offset == 0;
    largestDestination = std::max(largestDestination, destination.size());
    if (blockEntered) {
      auto *signal = std::exchange(blockEntered, nullptr);
      signal->set_value();
      continueRead.wait();
    }
    if (command.dataRevision != revision) {
      return std::unexpected(DocumentError{SourceError::REVISION_CHANGED, {}, {}});
    }
    const auto offset = std::min<std::uint64_t>(command.offset, bytes.size());
    const auto count =
      static_cast<std::size_t>(std::min<std::uint64_t>(std::min(destination.size(), maxReply), bytes.size() - offset));
    std::memcpy(destination.data(), bytes.data() + offset, count);
    readBytes += count;
    if (mutateDuringRead) {
      ++revision;
    }
    if (cancelDuringRead) {
      cancelDuringRead->request_stop();
    }
    return ByteReadResult{count, offset + count == bytes.size()};
  }
};

struct Fixture {
  std::shared_ptr<SourceBudget> budget = take(SourceBudget::create());
  std::shared_ptr<Input> input = std::make_shared<Input>();
  std::shared_ptr<GzipDocumentProvider> gzip;

  explicit Fixture(std::string bytes, GzipDocumentOptions options = {}) {
    input->bytes = std::move(bytes);
    gzip = take(GzipDocumentProvider::create(input, budget, options));
  }

  std::expected<std::string, DocumentError> read(std::uint64_t offset, std::size_t count, std::stop_token stop = {}) {
    return worker([&]() -> std::expected<std::string, DocumentError> {
      std::string result(count, '\0');
      auto page = gzip->read({1, offset}, std::as_writable_bytes(std::span(result)), stop);
      if (!page) {
        return std::unexpected(page.error());
      }
      result.resize(page->bytesRead);
      return result;
    });
  }

  std::string all(std::size_t window = 4096) {
    std::string result;
    for (std::size_t turn = 0; turn < 10000; ++turn) {
      auto page = take(read(result.size(), window));
      if (page.empty()) {
        return result;
      }
      result += page;
    }
    FAIL("bounded test read limit exceeded");
    return {};
  }
};
} // namespace

TEST_CASE("gzip creation is cold and metadata does not force decompression") {
  Fixture f(stored("hello"));
  CHECK(f.input->metadataCalls == 0);
  CHECK(f.input->reads == 0);
  CHECK(f.gzip->capabilities().seekable);
  CHECK(f.gzip->capabilities().immutable);
  const auto metadata = take(worker([&] {
    return f.gzip->metadata({});
  }));
  CHECK(metadata.dataRevision == 1);
  CHECK_FALSE(metadata.byteExtent);
  CHECK(f.input->reads == 0);
  CHECK(take(f.read(0, 5)) == "hello");
  CHECK_FALSE(take(worker([&] {
                return f.gzip->metadata({});
              }))
                .byteExtent);
  CHECK(take(f.read(5, 1)).empty());
  CHECK(take(worker([&] {
          return f.gzip->metadata({});
        }))
          .byteExtent == 5);
}

TEST_CASE("gzip extent discovery validates concatenated members using bounded storage") {
  GzipDocumentOptions options;
  options.compressedBlockBytes = 13;
  options.inflateOutputBytes = 127;
  options.checkpointStrideBytes = 4096;
  options.maxCheckpoints = 4;
  Fixture f(repeatedGzip() + stored("") + stored("tail"), options);
  f.input->knownExtent = false;
  const auto before = take(worker([&] {
    return f.gzip->metadata({});
  }));
  CHECK_FALSE(before.byteExtent);
  CHECK(f.input->reads == 0);
  const auto fixed = f.budget->usage().bytes;
  const auto discovered = take(worker([&] {
    return f.gzip->discoverExtent({});
  }));
  CHECK(discovered.dataRevision == before.dataRevision);
  CHECK(discovered.byteExtent == repeatedText().size() + 4);
  CHECK(f.input->readBytes == f.input->bytes.size());
  CHECK(f.budget->usage().bytes <= fixed + options.maxInflateBytes);
  const auto reads = f.input->reads;
  CHECK(take(worker([&] {
          return f.gzip->discoverExtent({});
        }))
          .byteExtent == discovered.byteExtent);
  CHECK(f.input->reads == reads);
  CHECK(take(f.read(*discovered.byteExtent - 4, 4)) == "tail");
}

TEST_CASE("gzip extent discovery handles an empty validated archive") {
  Fixture f(stored(""));
  const auto discovered = take(worker([&] {
    return f.gzip->discoverExtent({});
  }));
  CHECK(discovered.byteExtent == 0);
  CHECK(take(f.read(0, 1)).empty());
}

TEST_CASE("gzip extent discovery is cancellable and a later discovery resumes safely") {
  Fixture f(repeatedGzip(), {.compressedBlockBytes = 7, .inflateOutputBytes = 113});
  CHECK(take(f.read(0, 32)) == repeatedText().substr(0, 32));
  const auto reads = f.input->reads;
  std::stop_source stop;
  bool alreadyCancelled{};
  SUBCASE("before discovery") {
    stop.request_stop();
    alreadyCancelled = true;
  }
  SUBCASE("during compressed replay") {
    f.input->cancelDuringRead = &stop;
  }
  const auto cancelled = worker([&] {
    return f.gzip->discoverExtent(stop.get_token());
  });
  REQUIRE_FALSE(cancelled);
  CHECK(cancelled.error().code == SourceError::CANCELLED);
  if (alreadyCancelled) {
    CHECK(f.input->reads == reads);
  }
  f.input->cancelDuringRead = nullptr;
  CHECK_FALSE(take(worker([&] {
                return f.gzip->metadata({});
              }))
                .byteExtent);
  CHECK(take(worker([&] {
          return f.gzip->discoverExtent({});
        }))
          .byteExtent == repeatedText().size());
  CHECK(take(f.read(0, 32)) == repeatedText().substr(0, 32));
}

TEST_CASE("gzip extent discovery never promotes an invalid member to a known extent") {
  const auto expectedText = std::string(1000, 'x');
  auto bytes = stored(expectedText);
  auto cause = GzipError::CHECKSUM_MISMATCH;
  SUBCASE("CRC") {
    bytes[bytes.size() - 8] ^= 1;
  }
  SUBCASE("truncated trailer") {
    bytes.resize(bytes.size() - 3);
    cause = GzipError::TRUNCATED;
  }
  SUBCASE("trailing garbage") {
    bytes += "garbage";
    cause = GzipError::TRAILING_DATA;
  }
  Fixture f(bytes);
  const auto prefix = take(f.read(0, 64));
  const auto discovered = worker([&] {
    return f.gzip->discoverExtent({});
  });
  REQUIRE_FALSE(discovered);
  CHECK(discovered.error().native == make_error_code(cause));
  CHECK_FALSE(take(worker([&] {
                return f.gzip->metadata({});
              }))
                .byteExtent);
  CHECK(prefix == expectedText.substr(0, 64));
}

TEST_CASE("gzip supports short reads concatenated members and empty members") {
  Fixture f(stored("first") + stored("") + stored("second"), {.compressedBlockBytes = 7, .inflateOutputBytes = 3});
  f.input->knownExtent = false;
  f.input->maxReply = 1;
  CHECK(f.all(2) == "firstsecond");
  CHECK(f.input->largestDestination == 7);
  CHECK(take(worker([&] {
          return f.gzip->metadata({});
        }))
          .byteExtent == 11);
  CHECK(take(f.read(0, 11)) == "firstsecond");
}

TEST_CASE("gzip checkpoints restore Huffman history and compressed consumed positions") {
  GzipDocumentOptions options;
  options.compressedBlockBytes = 19;
  options.inflateOutputBytes = 113;
  options.checkpointStrideBytes = 4096;
  options.maxCheckpoints = 4;
  options.replayBytes = 64;
  Fixture f(repeatedGzip(), options);
  const auto expected = repeatedText();
  CHECK(f.all() == expected);
  const auto originReads = f.input->originReads;
  CHECK(take(f.read(expected.size() - 5000, 1000)) == expected.substr(expected.size() - 5000, 1000));
  CHECK(f.input->originReads == originReads);
  CHECK(take(f.read(7, 257)) == expected.substr(7, 257));
  CHECK(f.input->originReads > originReads);
  CHECK(f.budget->usage().bytes < 1024 * 1024);
}

TEST_CASE("gzip forward reads select a retained checkpoint after extent discovery and origin replay") {
  GzipDocumentOptions options;
  options.compressedBlockBytes = 7;
  options.inflateOutputBytes = 113;
  options.checkpointStrideBytes = 4096;
  options.maxCheckpoints = 8;
  options.replayBytes = 0;
  Fixture f(repeatedGzip(), options);
  const auto expected = repeatedText();
  CHECK(take(worker([&] {
          return f.gzip->discoverExtent({});
        }))
          .byteExtent == expected.size());
  CHECK(take(f.read(0, 17)) == expected.substr(0, 17));
  const auto readBytes = f.input->readBytes;
  const auto originReads = f.input->originReads;
  CHECK(take(f.read(expected.size() - 5000, 1000)) == expected.substr(expected.size() - 5000, 1000));
  // This fixed fixture's late checkpoint lies in the last half of its input;
  // continuing the origin inflater would consume most of the compressed stream.
  CHECK(f.input->readBytes - readBytes < f.input->bytes.size() / 2);
  CHECK(f.input->originReads == originReads);
}

TEST_CASE("gzip optional checkpoint failure and eviction preserve sequential data") {
  GzipDocumentOptions options;
  options.inflateOutputBytes = 127;
  options.checkpointStrideBytes = 1024;
  options.maxCheckpoints = 2;
  options.replayBytes = 17;
  options.maxInflateBytes = 64 * 1024;
  Fixture f(repeatedGzip(), options);
  CHECK(f.all(1013) == repeatedText());
  CHECK(take(f.read(13, 113)) == repeatedText().substr(13, 113));
}

TEST_CASE("gzip evicts state-only checkpoints when the next member needs a mandatory window") {
  std::string bytes;
  for (std::size_t member = 0; member < 12; ++member) {
    bytes += stored("x");
  }
  bytes += stored(std::string(10000, 'z'));
  GzipDocumentOptions options;
  options.compressedBlockBytes = 31;
  options.inflateOutputBytes = 32;
  options.checkpointStrideBytes = 1;
  options.maxCheckpoints = 8;
  options.replayBytes = 17;
  options.maxInflateBytes = 64 * 1024;
  Fixture f(bytes, options);
  CHECK(f.all() == std::string(12, 'x') + std::string(10000, 'z'));
  CHECK(f.budget->usage().bytes > f.gzip->pinnedBudgetBytes(*f.budget) + SourceBudget::ROOT_METADATA_BYTES);
  CHECK(f.gzip->tryReleaseOptionalStorage());
  CHECK(f.budget->usage().bytes == f.gzip->pinnedBudgetBytes(*f.budget) + SourceBudget::ROOT_METADATA_BYTES);
}

TEST_CASE("gzip replay owns accepted bytes after a cancelled source read") {
  Fixture f(stored(std::string(20000, 'x')), {.compressedBlockBytes = 31, .inflateOutputBytes = 17});
  CHECK(take(f.read(0, 1000)) == std::string(1000, 'x'));
  const auto calls = f.input->reads;
  CHECK(take(f.read(500, 200)) == std::string(200, 'x'));
  CHECK(f.input->reads == calls);
  std::stop_source stop;
  f.input->cancelDuringRead = &stop;
  const auto cancelled = f.read(15000, 100, stop.get_token());
  REQUIRE_FALSE(cancelled);
  CHECK(cancelled.error().code == SourceError::CANCELLED);
  f.input->cancelDuringRead = nullptr;
  CHECK(take(f.read(15000, 100)) == std::string(100, 'x'));
  CHECK(take(f.read(0, 100)) == std::string(100, 'x'));
}

TEST_CASE("gzip retains accepted output when cancellation rejects the completed call") {
  Fixture f(stored(std::string(20000, 'x')));
  CHECK(take(f.read(0, 1000)) == std::string(1000, 'x'));
  const auto reads = f.input->reads;
  std::stop_source stop;
  f.input->cancelAfterMetadata = &stop;
  f.input->cancelMetadataCall = f.input->metadataCalls + 2;
  const auto cancelled = f.read(1000, 100, stop.get_token());
  REQUIRE_FALSE(cancelled);
  CHECK(cancelled.error().code == SourceError::CANCELLED);
  f.input->cancelAfterMetadata = nullptr;
  CHECK(take(f.read(1000, 100)) == std::string(100, 'x'));
  CHECK(f.input->reads == reads);
}

TEST_CASE("gzip integrity failures never become clean EOF") {
  std::string bytes = stored("payload");
  auto expected = GzipError::CORRUPT_DATA;
  SUBCASE("CRC") {
    bytes[bytes.size() - 8] ^= 1;
    expected = GzipError::CHECKSUM_MISMATCH;
  }
  SUBCASE("ISIZE") {
    bytes[bytes.size() - 4] ^= 1;
    expected = GzipError::SIZE_MISMATCH;
  }
  SUBCASE("truncated trailer") {
    bytes.resize(bytes.size() - 3);
    expected = GzipError::TRUNCATED;
  }
  SUBCASE("truncated header") {
    bytes.resize(3);
    expected = GzipError::TRUNCATED;
  }
  SUBCASE("non gzip") {
    bytes = "not gzip";
    expected = GzipError::INVALID_HEADER;
  }
  SUBCASE("trailing garbage") {
    bytes += "garbage";
    expected = GzipError::TRAILING_DATA;
  }
  SUBCASE("corrupt concatenated member") {
    bytes += stored("later");
    bytes[bytes.size() - 8] ^= 1;
    expected = GzipError::CHECKSUM_MISMATCH;
  }
  Fixture f(bytes, {.compressedBlockBytes = 7, .inflateOutputBytes = 3});
  const auto result = f.read(0, 1024);
  REQUIRE_FALSE(result);
  CHECK(result.error().native == make_error_code(expected));
  CHECK_FALSE(take(worker([&] {
                return f.gzip->metadata({});
              }))
                .byteExtent);
  const auto repeated = f.read(0, 1);
  REQUIRE_FALSE(repeated);
  CHECK(repeated.error().native == result.error().native);
}

TEST_CASE("gzip headers are streamed with a fixed limit and no header materialization") {
  auto bytes = stored("ok");
  bytes[3] = '\x08';
  bytes.insert(10, std::string(600, 'n') + '\0');
  SUBCASE("valid bounded name") {
    Fixture f(bytes, {.compressedBlockBytes = 7, .maxHeaderBytes = 1024});
    CHECK(f.all() == "ok");
    CHECK(f.input->largestDestination == 7);
  }
  SUBCASE("header limit") {
    Fixture f(bytes, {.compressedBlockBytes = 7, .maxHeaderBytes = 64});
    const auto result = f.read(0, 1);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == SourceError::PAGE_TOO_LARGE);
    CHECK(result.error().native == make_error_code(GzipError::HEADER_LIMIT));
  }
  SUBCASE("cancellation in header") {
    Fixture f(bytes, {.compressedBlockBytes = 7});
    std::stop_source stop;
    f.input->cancelDuringRead = &stop;
    const auto result = f.read(0, 1, stop.get_token());
    REQUIRE_FALSE(result);
    CHECK(result.error().code == SourceError::CANCELLED);
    f.input->cancelDuringRead = nullptr;
    CHECK(f.all() == "ok");
  }
}

TEST_CASE("gzip frozen revision rejects mutation before accepting decompressed bytes") {
  Fixture f(stored("unchanged"));
  take(worker([&] {
    return f.gzip->metadata({});
  }));
  SUBCASE("before read") {
    ++f.input->revision;
  }
  SUBCASE("during read") {
    f.input->mutateDuringRead = true;
  }
  const auto result = f.read(0, 4);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == SourceError::REVISION_CHANGED);
  const auto info = worker([&] {
    return f.gzip->metadata({});
  });
  REQUIRE_FALSE(info);
  CHECK(info.error().code == SourceError::REVISION_CHANGED);
}

TEST_CASE("gzip state and replay credits disappear after the final provider owner") {
  auto budget = take(SourceBudget::create());
  const auto initial = budget->usage().bytes;
  auto input = std::make_shared<Input>();
  input->bytes = repeatedGzip();
  auto gzip = take(GzipDocumentProvider::create(input, budget));
  const auto fixed = budget->usage().bytes;
  std::array<std::byte, 4096> output{};
  const auto read = worker([&] {
    return gzip->read({1, 10000}, output, {});
  });
  REQUIRE(read);
  CHECK(budget->usage().bytes > fixed);
  gzip.reset();
  CHECK(budget->usage().bytes == initial);
}

TEST_CASE("gzip pages outlive providers while retaining their independent root credit") {
  auto budget = take(SourceBudget::create());
  const auto initial = budget->usage().bytes;
  auto input = std::make_shared<Input>();
  input->bytes = stored("retained page");
  auto gzip = take(GzipDocumentProvider::create(input, budget));
  auto source = take(ByteDocumentSource::create(gzip, budget));
  const auto metadata = take(worker([&] {
    return source->inspect();
  }));
  auto page = take(worker([&] {
    return source->read({metadata.revision, 0, 13, 1});
  }));
  source.reset();
  gzip.reset();
  input.reset();
  CHECK(std::string(reinterpret_cast<const char *>(page.bytes().data()), page.bytes().size()) == "retained page");
  CHECK(budget->usage().bytes > initial);
  page = {};
  CHECK(budget->usage().bytes == initial);
}

TEST_CASE("gzip offsets remain 64 bit and overflow is rejected before replay") {
  Fixture f(stored("small"));
  CHECK(take(f.read(std::uint64_t{1} << 32, 1)).empty());
  CHECK(take(worker([&] {
          return f.gzip->metadata({});
        }))
          .byteExtent == 5);
  CHECK(take(f.read(0, 5)) == "small");
  const auto reads = f.input->reads;
  const auto overflow = f.read(std::numeric_limits<std::uint64_t>::max(), 1);
  REQUIRE_FALSE(overflow);
  CHECK(overflow.error().code == SourceError::POSITION_EXHAUSTED);
  CHECK(f.input->reads == reads);
}

TEST_CASE("gzip allocator cap and root pressure return typed failures without leaking credit") {
  SUBCASE("hard inflater cap") {
    Fixture f(stored("x"), {.maxInflateBytes = 1});
    const auto fixed = f.budget->usage().bytes;
    const auto result = f.read(0, 1);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == SourceError::PAGE_TOO_LARGE);
    CHECK(result.error().native == make_error_code(GzipError::INFLATE_LIMIT));
    CHECK(f.budget->usage().bytes == fixed);
  }
  SUBCASE("root pressure") {
    auto budget = take(SourceBudget::create({.maxBytes = 128 * 1024}));
    auto input = std::make_shared<Input>();
    // Leave the member unfinished after one output byte, forcing zlib to retain
    // its sliding window instead of completing a one-byte member in this call.
    input->bytes = stored("xy");
    GzipDocumentOptions options;
    options.compressedBlockBytes = options.inflateOutputBytes = 32;
    options.maxCheckpoints = options.replayBytes = 0;
    auto gzip = take(GzipDocumentProvider::create(input, budget, options));
    // The adapter alone fits. Another owner's retained credit temporarily
    // leaves enough room for the state but not the mandatory sliding window.
    auto pressure = take(budget->reserve({budget->options().maxBytes - budget->usage().bytes - 8192, 0, 0, 0}));
    const auto fixed = budget->usage().bytes;
    std::array<std::byte, 1> output{};
    const auto result = worker([&] {
      return gzip->read({1, 0}, output, {});
    });
    REQUIRE_FALSE(result);
    CHECK(result.error().code == SourceError::WOULD_BLOCK);
    CHECK(result.error().budgetDemand);
    REQUIRE(result.error().budgetPeak);
    CHECK(result.error().budgetPeak->bytes > result.error().budgetDemand->bytes);
    CHECK(budget->usage().bytes == fixed);
    pressure = {};
    const auto retried = worker([&] {
      return gzip->read({1, 0}, output, {});
    });
    REQUIRE(retried);
    CHECK(output[0] == std::byte{'x'});
  }
}

TEST_CASE("gzip mandatory own overlap is a permanent root limit rather than a capacity wait") {
  auto budget = take(SourceBudget::create({.maxBytes = 40 * 1024}));
  auto input = std::make_shared<Input>();
  input->bytes = stored("xy");
  GzipDocumentOptions options;
  options.compressedBlockBytes = options.inflateOutputBytes = 32;
  options.maxCheckpoints = options.replayBytes = 0;
  auto gzip = take(GzipDocumentProvider::create(input, budget, options));
  const auto fixed = budget->usage().bytes;
  std::array<std::byte, 1> output{};
  const auto result = worker([&] {
    return gzip->read({1, 0}, output, {});
  });
  REQUIRE_FALSE(result);
  CHECK(result.error().code == SourceError::PAGE_TOO_LARGE);
  CHECK(budget->usage().bytes == fixed);
}

TEST_CASE("gzip reports same-root fixed pins and releases rebuildable storage") {
  GzipDocumentOptions options;
  options.compressedBlockBytes = 7;
  options.inflateOutputBytes = 113;
  options.checkpointStrideBytes = 1024;
  options.maxCheckpoints = 4;
  options.replayBytes = 64;
  Fixture f(repeatedGzip(), options);
  auto otherRoot = take(SourceBudget::create());
  CHECK(f.gzip->pinnedBudgetBytes(*otherRoot) == 0);
  CHECK(f.gzip->pinnedBudgetBytes(*f.budget) + SourceBudget::ROOT_METADATA_BYTES == f.budget->usage().bytes);
  CHECK(take(f.read(0, 40000)) == repeatedText().substr(0, 40000));
  const auto pin = f.gzip->pinnedBudgetBytes(*f.budget);
  REQUIRE(f.budget->usage().bytes > pin + SourceBudget::ROOT_METADATA_BYTES);
  const auto reads = f.input->reads;
  CHECK(f.gzip->tryReleaseOptionalStorage());
  CHECK(f.gzip->pinnedBudgetBytes(*f.budget) == pin);
  CHECK(f.budget->usage().bytes == pin + SourceBudget::ROOT_METADATA_BYTES);
  CHECK(take(f.read(39970, 20)) == repeatedText().substr(39970, 20));
  CHECK(f.input->reads == reads);
  CHECK(take(f.read(40000, 20)) == repeatedText().substr(40000, 20));
}

TEST_CASE("gzip busy cache trim is serviced by worker completion without waiting on the caller") {
  GzipDocumentOptions options;
  options.compressedBlockBytes = 7;
  options.inflateOutputBytes = 113;
  options.checkpointStrideBytes = 1024;
  options.maxCheckpoints = 4;
  options.replayBytes = 64;
  Fixture f(repeatedGzip(), options);
  CHECK(take(f.read(0, 40000)) == repeatedText().substr(0, 40000));
  std::promise<void> entered;
  auto enteredFuture = entered.get_future();
  std::promise<void> release;
  f.input->blockEntered = &entered;
  f.input->continueRead = release.get_future().share();
  auto pending = std::async(std::launch::async, [&] {
    return f.read(60000, 100);
  });
  const bool blocked = enteredFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
  const bool releasedWhileBusy = f.gzip->tryReleaseOptionalStorage();
  release.set_value();
  const auto completed = pending.get();
  REQUIRE(blocked);
  CHECK_FALSE(releasedWhileBusy);
  CHECK(take(completed) == repeatedText().substr(60000, 100));
  CHECK(f.budget->usage().bytes == f.gzip->pinnedBudgetBytes(*f.budget) + SourceBudget::ROOT_METADATA_BYTES);
  CHECK(take(f.read(60000, 100)) == repeatedText().substr(60000, 100));
}

TEST_CASE("gzip trim without checkpoints preserves replay and rebuilds across a member boundary") {
  const auto first = std::string("first-") + std::string(1000, 'x');
  const auto second = std::string("second-") + std::string(1000, 'y');
  const auto expected = first + second;
  Fixture f(stored(first) + stored(second), {.maxCheckpoints = 0, .replayBytes = 64});
  const auto offset = first.size() + 100;
  CHECK(take(f.read(offset, 32)) == expected.substr(offset, 32));
  const auto before = take(worker([&] {
    return f.gzip->metadata({});
  }));
  const auto reads = f.input->reads;
  REQUIRE(f.gzip->tryReleaseOptionalStorage());
  CHECK(f.budget->usage().bytes == f.gzip->pinnedBudgetBytes(*f.budget) + SourceBudget::ROOT_METADATA_BYTES);
  CHECK(take(f.read(offset + 16, 8)) == expected.substr(offset + 16, 8));
  CHECK(f.input->reads == reads);
  CHECK_FALSE(f.gzip->tryReleaseOptionalStorage());
  const auto after = take(worker([&] {
    return f.gzip->metadata({});
  }));
  CHECK(after.dataRevision == before.dataRevision);
  CHECK(after.byteExtent == before.byteExtent);
  CHECK(take(f.read(first.size() - 20, 60)) == expected.substr(first.size() - 20, 60));
  CHECK(f.input->reads > reads);
}

TEST_CASE("gzip checkpoint count has a finite supported retirement bound") {
  auto input = std::make_shared<Input>();
  auto budget = take(SourceBudget::create());
  input->bytes = stored("x");
  GzipDocumentOptions options;
  options.maxCheckpoints = GzipDocumentOptions::MAX_CHECKPOINTS + 1;
  const auto result = GzipDocumentProvider::create(input, budget, options);
  REQUIRE_FALSE(result);
  CHECK(result.error().code == SourceError::INVALID_ARGUMENT);
  CHECK(input->metadataCalls == 0);
  CHECK(input->reads == 0);
}

TEST_CASE("independent gzip providers and shared logical sources retain separate state") {
  Fixture first(repeatedGzip(), {.compressedBlockBytes = 13, .replayBytes = 0});
  Fixture second(stored(std::string(10000, 'z')), {.compressedBlockBytes = 11});
  auto a = std::async(std::launch::async, [&] {
    return first.read(13000, 1000);
  });
  auto b = std::async(std::launch::async, [&] {
    return second.read(7000, 1000);
  });
  CHECK(take(a.get()) == repeatedText().substr(13000, 1000));
  CHECK(take(b.get()) == std::string(1000, 'z'));
  CHECK(take(first.read(0, 17)) == repeatedText().substr(0, 17));
  auto sourceA = take(ByteDocumentSource::create(first.gzip, first.budget));
  auto sourceB = take(ByteDocumentSource::create(first.gzip, first.budget));
  const auto infoA = take(worker([&] {
    return sourceA->inspect();
  }));
  const auto infoB = take(worker([&] {
    return sourceB->inspect();
  }));
  CHECK_FALSE(infoA.revision.source == infoB.revision.source);
  const auto pageA = take(worker([&] {
    return sourceA->read({infoA.revision, 7, 17, 1});
  }));
  const auto pageB = take(worker([&] {
    return sourceB->read({infoB.revision, 21, 17, 1});
  }));
  CHECK(std::string(reinterpret_cast<const char *>(pageA.bytes().data()), pageA.bytes().size()) ==
        repeatedText().substr(7, 17));
  CHECK(std::string(reinterpret_cast<const char *>(pageB.bytes().data()), pageB.bytes().size()) ==
        repeatedText().substr(21, 17));
}

TEST_CASE("streaming search and bounded copy use decompressed offsets across gzip members") {
  Fixture f(stored("prefix ne") + stored("edle final needle"), {.compressedBlockBytes = 7, .inflateOutputBytes = 3});
  f.input->knownExtent = false;
  auto source = take(ByteDocumentSource::create(f.gzip, f.budget, {.maxPageBytes = 3}));

  worker([&] {
    auto session =
      take(DocumentSearchSession::create(source, "needle", {.hint = DocumentEncoding::UTF8}, {.workBytes = 3}));
    const auto next = [&] {
      for (unsigned turn = 0; turn < 128; ++turn) {
        auto step = take(session->advance());

        if (step.state != DocumentSearchState::PENDING) {
          return step;
        }
      }

      FAIL("small compressed fixture did not finish within its bounded test steps");
      return DocumentSearchStep{};
    };

    auto first = next();
    REQUIRE(first.state == DocumentSearchState::FOUND);
    REQUIRE(first.match);
    CHECK(first.match->byteOffset == 7);
    CHECK(first.match->byteLength == 6);
    CHECK(first.match->text == "needle");

    auto second = next();
    REQUIRE(second.state == DocumentSearchState::FOUND);
    CHECK(second.match->byteOffset == 20);
    CHECK(next().state == DocumentSearchState::END);
    REQUIRE(session->start(DocumentSearchDirection::PREVIOUS, 20));
    CHECK(next().match->byteOffset == 7);

    auto copied = take(copyDocumentBytes(source, {7, 13}, 64));
    const auto bytes = copied->bytes();
    CHECK(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()) == "needle");
    CHECK(take(source->inspect()).byteExtent == 26);
  });
}
