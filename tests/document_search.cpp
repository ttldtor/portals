// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/document_search.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <future>
#include <limits>

namespace {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> result) {
  REQUIRE(result);
  return std::move(*result);
}

template <class F> auto worker(F &&function) {
  return std::async(std::launch::async, std::forward<F>(function)).get();
}

class Input final : public ByteDocumentProvider {
public:
  std::string text;
  std::uint64_t revision{1};
  std::size_t reads{};
  std::size_t metadataCalls{};
  std::uint64_t admitted{};
  std::size_t maxReply{std::numeric_limits<std::size_t>::max()};
  bool knownExtent{true};
  bool seekable{true};
  bool mutateDuringRead{};
  std::optional<DocumentError> readError;
  std::stop_source *cancelDuringRead{};
  std::stop_source *cancelOnMetadata{};
  std::size_t cancelMetadataCall{};

  ByteDocumentCapabilities capabilities() const noexcept override {
    return {seekable, false};
  }

  std::expected<ByteDocumentMetadata, DocumentError> metadata(std::stop_token) override {
    ++metadataCalls;
    if (cancelOnMetadata && metadataCalls == cancelMetadataCall) {
      cancelOnMetadata->request_stop();
    }
    return ByteDocumentMetadata{revision, knownExtent ? std::optional<std::uint64_t>(text.size()) : std::nullopt};
  }

  std::expected<ByteReadResult, DocumentError> read(const ByteReadCommand &command, std::span<std::byte> destination,
                                                    std::stop_token) override {
    ++reads;
    if (readError) {
      return std::unexpected(*readError);
    }
    if (command.dataRevision != revision) {
      return std::unexpected(DocumentError{SourceError::REVISION_CHANGED, {}, {}});
    }
    const auto offset = std::min<std::uint64_t>(command.offset, text.size());
    const auto count =
      static_cast<std::size_t>(std::min<std::uint64_t>(std::min(destination.size(), maxReply), text.size() - offset));
    std::memcpy(destination.data(), text.data() + offset, count);
    admitted += count;
    if (mutateDuringRead) {
      ++revision;
    }
    if (cancelDuringRead) {
      cancelDuringRead->request_stop();
    }
    return ByteReadResult{count, offset + count == text.size()};
  }
};

struct Fixture {
  std::shared_ptr<SourceBudget> budget;
  std::shared_ptr<Input> input = std::make_shared<Input>();
  std::shared_ptr<ByteDocumentSource> source;

  explicit Fixture(std::string bytes, std::size_t page = 4096, SourceBudgetOptions limits = {})
      : budget(take(SourceBudget::create(limits))) {
    input->text = std::move(bytes);
    source = take(ByteDocumentSource::create(input, budget, {.maxPageBytes = page}));
  }

  std::shared_ptr<DocumentSearchSession> search(std::string_view query, DocumentSearchOptions options = {},
                                                DocumentDecoderOptions decoder = {.hint = DocumentEncoding::UTF8}) {
    return take(DocumentSearchSession::create(source, query, decoder, options));
  }
};

std::expected<DocumentSearchStep, DocumentSearchFailure> finish(DocumentSearchSession &session,
                                                                std::size_t turns = 100000) {
  for (std::size_t turn = 0; turn < turns; ++turn) {
    auto step = session.advance();
    if (!step || step->state != DocumentSearchState::PENDING) {
      return step;
    }
  }
  FAIL("bounded test advance count exceeded");
  return std::unexpected(DocumentSearchFailure{{SourceError::CONTINUATION_LIMIT, {}, {}}, {}});
}

std::string encode(std::u32string_view text, DocumentEncoding encoding) {
  const bool utf16 = encoding == DocumentEncoding::UTF16LE || encoding == DocumentEncoding::UTF16BE;
  const bool little = encoding == DocumentEncoding::UTF16LE || encoding == DocumentEncoding::UTF32LE;
  std::string bytes;
  const auto append = [&](std::uint32_t value, unsigned count) {
    for (unsigned index = 0; index < count; ++index) {
      const auto shift = (little ? index : count - 1 - index) * 8;
      bytes.push_back(static_cast<char>((value >> shift) & 255));
    }
  };
  append(0xfeff, utf16 ? 2 : 4);
  for (const auto scalar : text) {
    if (utf16 && scalar > 0xffff) {
      const auto value = static_cast<std::uint32_t>(scalar - 0x10000);
      append(0xd800 + (value >> 10), 2);
      append(0xdc00 + (value & 1023), 2);
    } else {
      append(static_cast<std::uint32_t>(scalar), utf16 ? 2 : 4);
    }
  }
  return bytes;
}
} // namespace

TEST_CASE("document literal search is cold and preserves overlaps across byte and work boundaries") {
  Fixture f("banana", 2);
  f.input->maxReply = 1;
  auto session = f.search("ana", {.workBytes = 2});
  CHECK(f.input->reads == 0);
  CHECK(f.input->metadataCalls == 0);
  worker([&] {
    const auto first = take(finish(*session));
    REQUIRE(first.state == DocumentSearchState::FOUND);
    REQUIRE(first.match);
    CHECK(first.match->byteOffset == 1);
    CHECK(first.match->byteLength == 3);
    CHECK(first.match->text == "ana");
    const auto second = take(finish(*session));
    REQUIRE(second.match);
    CHECK(second.match->byteOffset == 3);
    CHECK(second.match->byteLength == 3);
    CHECK(take(finish(*session)).state == DocumentSearchState::END);
    CHECK(first.match->text == "ana");
  });
}

TEST_CASE("document search work quantum bounds actual source admissions and reports pending") {
  Fixture f(std::string(1000, 'x') + "needle", 64);
  auto session = f.search("needle", {.workBytes = 7});
  worker([&] {
    bool pending{};
    for (std::size_t turn = 0; turn < 1000; ++turn) {
      const auto admitted = f.input->admitted;
      const auto step = take(session->advance());
      CHECK(f.input->admitted - admitted <= 7);
      pending = pending || step.state == DocumentSearchState::PENDING;
      if (step.state == DocumentSearchState::FOUND) {
        CHECK(step.match->byteOffset == 1000);
        CHECK(pending);
        return;
      }
    }
    FAIL("expected bounded pending search to find its suffix");
  });
}

TEST_CASE("search snapshot final revision guard includes the still-live match in its retry peak") {
  Fixture measured("a", 1);
  auto measuredSession = measured.search("a");
  const auto measuredMatch = worker([&] {
    return take(finish(*measuredSession)).match;
  });
  REQUIRE(measuredMatch);
  const auto sessionBytes = measuredSession->retainedUsage().bytes;
  const auto matchBytes = measuredMatch->retainedBytes();
  const auto maximum = SourceBudget::ROOT_METADATA_BYTES + measured.source->envelopeBytes() + sessionBytes +
                       matchBytes + measured.source->envelopeBytes() - 1;
  Fixture f("a", 1, {.maxBytes = maximum});
  auto session = f.search("a");

  worker([&] {
    for (int attempt = 0; attempt != 2; ++attempt) {
      const auto result = finish(*session);

      REQUIRE_FALSE(result);
      CHECK(result.error().error.code == SourceError::PAGE_TOO_LARGE);
      REQUIRE(result.error().error.budgetDemand);
      REQUIRE(result.error().error.budgetPeak);
      CHECK(result.error().error.budgetDemand->bytes == f.source->envelopeBytes());
      CHECK(result.error().error.budgetPeak->bytes == sessionBytes + matchBytes + f.source->envelopeBytes());
      CHECK(result.error().error.budgetPeak->work == 1);
      CHECK(f.budget->usage().bytes == SourceBudget::ROOT_METADATA_BYTES + f.source->envelopeBytes() + sessionBytes);
    }
  });
}

TEST_CASE("document search retained usage separates cursor pages from published matches") {
  Fixture f("a" + std::string(5000, 'x'), 4096);
  auto session = f.search("a");
  worker([&] {
    const auto initial = session->retainedUsage();
    CHECK(initial.pages == 0);
    CHECK(initial.work == 0);
    CHECK(initial.consumers == 0);
    const auto found = take(finish(*session));
    REQUIRE(found.match);
    const auto current = session->retainedUsage();
    CHECK(current.pages == 1);
    CHECK(current.bytes == initial.bytes + 4096 + SourceArena::METADATA_BYTES);
    CHECK(f.budget->usage().bytes ==
          SourceBudget::ROOT_METADATA_BYTES + f.source->envelopeBytes() + current.bytes + found.match->retainedBytes());
    REQUIRE(session->start(DocumentSearchDirection::NEXT));
    CHECK(session->retainedUsage().bytes == initial.bytes);
    CHECK(session->retainedUsage().pages == 0);
    CHECK(found.match->text == "a");
  });
}

TEST_CASE("document search maps Unicode literals back to every supported original encoding") {
  auto encoding = DocumentEncoding::UTF8;
  std::string query = "\xd1\x82\xd0\xb5\xd1\x81\xd1\x82";
  std::string bytes = "head " + query + " tail";
  std::uint64_t first = 5, count = 8;
  SUBCASE("UTF8") {
  }
  SUBCASE("UTF16LE") {
    encoding = DocumentEncoding::UTF16LE;
  }
  SUBCASE("UTF16BE") {
    encoding = DocumentEncoding::UTF16BE;
  }
  SUBCASE("UTF32LE") {
    encoding = DocumentEncoding::UTF32LE;
  }
  SUBCASE("UTF32BE") {
    encoding = DocumentEncoding::UTF32BE;
  }
  SUBCASE("Windows1251") {
    encoding = DocumentEncoding::WINDOWS1251;
    bytes = "head \xf2\xe5\xf1\xf2 tail";
    count = 4;
  }
  SUBCASE("Windows1252") {
    encoding = DocumentEncoding::WINDOWS1252;
    query = "caf\xc3\xa9";
    bytes = "head caf\xe9 tail";
    count = 4;
  }
  if (encoding >= DocumentEncoding::UTF16LE && encoding <= DocumentEncoding::UTF32BE) {
    bytes = encode(U"head тест tail", encoding);
    const auto unit = encoding == DocumentEncoding::UTF16LE || encoding == DocumentEncoding::UTF16BE ? 2 : 4;
    first = 6 * unit;
    count = 4 * unit;
  }
  Fixture f(bytes, 3);
  auto session = f.search(query, {.workBytes = 5}, {.explicitEncoding = encoding});
  const auto step = take(worker([&] {
    return finish(*session);
  }));
  REQUIRE(step.match);
  CHECK(step.match->byteOffset == first);
  CHECK(step.match->byteLength == count);
  CHECK(step.match->text == query);
}

TEST_CASE("document literal matches require complete source grapheme endpoints") {
  std::string text = "a\xcc\x81 a";
  std::string query = "a";
  std::uint64_t offset = 4, length = 1;
  SUBCASE("combining suffix does not match an initial scalar alone") {
  }
  SUBCASE("whole combining grapheme") {
    query = "a\xcc\x81";
    offset = 0;
    length = 3;
  }
  SUBCASE("combining mark is not an independent endpoint") {
    query = "\xcc\x81";
    offset = 99;
  }
  SUBCASE("ZWJ prefix is not a whole emoji") {
    text = "\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb";
    query = "\xf0\x9f\x91\xa9";
    offset = 99;
  }
  SUBCASE("regional indicator parity comes from the real origin") {
    text = "\xf0\x9f\x87\xa6\xf0\x9f\x87\xa6\xf0\x9f\x87\xa6";
    query = "\xf0\x9f\x87\xa6";
    offset = 8;
    length = 4;
  }
  SUBCASE("CR alone is not the complete CRLF grapheme") {
    text = "\r\n";
    query = "\r";
    offset = 99;
  }
  Fixture f(text, 1);
  auto session = f.search(query, {.workBytes = 2});
  const auto step = take(worker([&] {
    return finish(*session);
  }));
  if (offset == 99) {
    CHECK(step.state == DocumentSearchState::END);
  } else {
    REQUIRE(step.match);
    CHECK(step.match->byteOffset == offset);
    CHECK(step.match->byteLength == length);
  }
}

TEST_CASE("document search is exact case sensitive and performs no normalization") {
  Fixture f("A a e\xcc\x81", 2);
  auto lower = f.search("a");
  CHECK(take(worker([&] {
          return finish(*lower);
        }))
          .match->byteOffset == 2);
  auto composed = f.search("\xc3\xa9");
  CHECK(take(worker([&] {
          return finish(*composed);
        }))
          .state == DocumentSearchState::END);
}

TEST_CASE("document Previous rescans a prefix and returns its last overlapping eligible start") {
  Fixture f("banana", 2);
  auto session = f.search("ana", {.workBytes = 2});
  worker([&] {
    REQUIRE(session->start(DocumentSearchDirection::PREVIOUS, 4));
    CHECK(take(finish(*session)).match->byteOffset == 3);
    CHECK(take(finish(*session)).state == DocumentSearchState::END);
    REQUIRE(session->start(DocumentSearchDirection::PREVIOUS, 3));
    CHECK(take(finish(*session)).match->byteOffset == 1);
    REQUIRE(session->start(DocumentSearchDirection::PREVIOUS, 1));
    CHECK(take(finish(*session)).state == DocumentSearchState::END);
    REQUIRE(session->start(DocumentSearchDirection::NEXT, 2));
    CHECK(take(finish(*session)).match->byteOffset == 3);
    REQUIRE(session->start(DocumentSearchDirection::PREVIOUS, 0));
    CHECK(take(finish(*session)).state == DocumentSearchState::END);
  });
}

TEST_CASE("document Previous does not scan an unrelated document suffix") {
  Fixture f("needle " + std::string(1024 * 1024, 'x'), 31);
  auto session = f.search("needle", {.workBytes = 64});
  const auto step = take(worker([&] {
    REQUIRE(session->start(DocumentSearchDirection::PREVIOUS, 1));
    return finish(*session);
  }));
  REQUIRE(step.match);
  CHECK(step.match->byteOffset == 0);
  CHECK(f.input->admitted <= 64);
}

TEST_CASE("document search has no document-size or total-hit prefix ceiling") {
  SUBCASE("a hit beyond the old synchronous document limit") {
    Fixture f(std::string(1024 * 1024 + 17, 'x') + "needle", 4096);
    auto session = f.search("needle");
    const auto step = take(worker([&] {
      return finish(*session);
    }));
    REQUIRE(step.match);
    CHECK(step.match->byteOffset == 1024 * 1024 + 17);
  }
  SUBCASE("many Next hits and Previous keep bounded storage") {
    Fixture f(std::string(70000, 'a'), 512, {.maxBytes = 64 * 1024});
    auto session = f.search("a");
    worker([&] {
      for (std::uint64_t hit = 0; hit < f.input->text.size(); ++hit) {
        const auto step = take(finish(*session));
        REQUIRE(step.match);
        CHECK(step.match->byteOffset == hit);
        CHECK(f.budget->usage().bytes <= 64 * 1024);
      }
      CHECK(take(finish(*session)).state == DocumentSearchState::END);
      REQUIRE(session->start(DocumentSearchDirection::PREVIOUS, f.input->text.size()));
      CHECK(take(finish(*session)).match->byteOffset == f.input->text.size() - 1);
    });
  }
}

TEST_CASE("document search reports unknown extent without fabricating a hit count") {
  Fixture f("a needle b", 3);
  f.input->knownExtent = false;
  auto session = f.search("needle");
  worker([&] {
    const auto found = take(finish(*session));
    CHECK_FALSE(found.byteExtent);
    REQUIRE(found.match);
    CHECK(found.match->byteOffset == 2);
    const auto end = take(finish(*session));
    CHECK(end.state == DocumentSearchState::END);
    CHECK_FALSE(end.byteExtent);
  });
}

TEST_CASE("document search preserves precise malformed decoder causes and replacement ranges") {
  Fixture f(std::string("a") + std::bit_cast<char>(std::uint8_t{0xff}) + "b", 3);
  SUBCASE("replace") {
    auto session = f.search("\xef\xbf\xbd");
    const auto found = take(worker([&] {
      return finish(*session);
    }));
    REQUIRE(found.match);
    CHECK(found.match->byteOffset == 1);
    CHECK(found.match->byteLength == 1);
  }
  SUBCASE("strict") {
    auto session = f.search("b", {}, {.hint = DocumentEncoding::UTF8, .policy = DocumentDecodePolicy::STRICT_MODE});
    const auto result = worker([&] {
      return finish(*session);
    });
    REQUIRE_FALSE(result);
    REQUIRE(result.error().decode);
    CHECK(result.error().decode->status == DocumentDecodeStatus::MALFORMED);
    CHECK(result.error().decode->errorOffset == 1);
    CHECK(result.error().decode->errorLength == 1);
  }
}

TEST_CASE("strict search can return a valid early hit before a later malformed scalar") {
  Fixture f(std::string("ok ") + std::bit_cast<char>(std::uint8_t{0xff}), 64);
  auto session = f.search("ok", {}, {.hint = DocumentEncoding::UTF8, .policy = DocumentDecodePolicy::STRICT_MODE});
  worker([&] {
    const auto found = take(finish(*session));
    REQUIRE(found.match);
    CHECK(found.match->byteOffset == 0);
    const auto failed = finish(*session);
    REQUIRE_FALSE(failed);
    REQUIRE(failed.error().decode);
    CHECK(failed.error().decode->errorOffset == 3);
  });
}

TEST_CASE("document search preserves provider errors cancellation and revision guards") {
  SUBCASE("provider error") {
    Fixture f("needle");
    f.input->readError = DocumentError{SourceError::IO_ERROR, std::make_error_code(std::errc::permission_denied), {}};
    auto session = f.search("needle");
    const auto result = worker([&] {
      return finish(*session);
    });
    REQUIRE_FALSE(result);
    CHECK(result.error().error.code == SourceError::IO_ERROR);
    CHECK(result.error().error.native == f.input->readError->native);
  }
  SUBCASE("cancel accepted input and retry") {
    Fixture f("needle");
    auto session = f.search("needle");
    std::stop_source stop;
    f.input->cancelDuringRead = &stop;
    const auto result = worker([&] {
      return session->advance(stop.get_token());
    });
    REQUIRE_FALSE(result);
    CHECK(result.error().error.code == SourceError::CANCELLED);
    f.input->cancelDuringRead = nullptr;
    CHECK(take(worker([&] {
            return finish(*session);
          }))
            .match->byteOffset == 0);
  }
  SUBCASE("mutation after read") {
    Fixture f("needle");
    auto session = f.search("needle");
    f.input->mutateDuringRead = true;
    const auto result = worker([&] {
      return finish(*session);
    });
    REQUIRE_FALSE(result);
    CHECK(result.error().error.code == SourceError::REVISION_CHANGED);
    f.input->mutateDuringRead = false;
    const auto restarted = worker([&] {
      REQUIRE(session->start(DocumentSearchDirection::NEXT));
      return finish(*session);
    });
    REQUIRE(restarted);
    REQUIRE(restarted->match);
    CHECK(restarted->match->revision.dataRevision == 2);
  }
}

TEST_CASE("a cancelled search publication retains the pending hit and accepted decoded batch") {
  Fixture f("aaa", 64);
  auto session = f.search("a");
  std::stop_source stop;
  f.input->cancelOnMetadata = &stop;
  f.input->cancelMetadataCall = 3;
  const auto cancelled = worker([&] {
    return session->advance(stop.get_token());
  });
  REQUIRE_FALSE(cancelled);
  CHECK(cancelled.error().error.code == SourceError::CANCELLED);
  f.input->cancelOnMetadata = nullptr;
  const auto reads = f.input->reads;
  worker([&] {
    CHECK(take(finish(*session)).match->byteOffset == 0);
    CHECK(take(finish(*session)).match->byteOffset == 1);
  });
  CHECK(f.input->reads == reads);
}

TEST_CASE("document search snapshots own bounded whole-grapheme context after session destruction") {
  Fixture f("e\xcc\x81"
            "aX");
  auto session = f.search("a", {.maxSnippetBytes = 3});
  const auto found = take(worker([&] {
    return finish(*session);
  }));
  REQUIRE(found.match);
  auto snapshot = found.match;
  CHECK(snapshot->snippet == "a");
  CHECK(snapshot->snippetMatchOffset == 0);
  CHECK(snapshot->snippetMatchLength == 1);
  session.reset();
  f.source.reset();
  f.input.reset();
  CHECK(snapshot->text == "a");
  CHECK(snapshot->byteOffset == 3);
  CHECK(snapshot->retainedBytes() <= f.budget->usage().bytes);
}

TEST_CASE("document query and result limits are explicit and nonseekable sources are unsupported") {
  Fixture f("needle");
  SUBCASE("empty") {
    CHECK_FALSE(DocumentSearchSession::create(f.source, ""));
  }
  SUBCASE("malformed query") {
    CHECK_FALSE(DocumentSearchSession::create(f.source, std::string(1, std::bit_cast<char>(std::uint8_t{0xff}))));
  }
  SUBCASE("query limit") {
    const auto result = DocumentSearchSession::create(f.source, "needle", {}, {.maxQueryBytes = 3});
    REQUIRE_FALSE(result);
    CHECK(result.error().error.code == SourceError::PAGE_TOO_LARGE);
  }
  SUBCASE("snippet cap preserves full match text") {
    auto session = f.search("needle", {.maxSnippetBytes = 3});
    const auto found = take(worker([&] {
      return finish(*session);
    }));
    REQUIRE(found.match);
    CHECK(found.match->text == "needle");
    CHECK(found.match->snippet.empty());
    CHECK(found.match->snippetTruncated);
  }
  SUBCASE("nonseekable") {
    f.input->seekable = false;
    auto source = take(ByteDocumentSource::create(f.input, f.budget));
    const auto result = DocumentSearchSession::create(source, "needle");
    REQUIRE_FALSE(result);
    CHECK(result.error().error.code == SourceError::UNSUPPORTED);
  }
}

TEST_CASE("document search memory pressure distinguishes impossible own overlap and transient owners") {
  SUBCASE("query storage cannot fit") {
    Fixture f("x", 64, {.maxBytes = 64 * 1024});
    const auto used = f.budget->usage().bytes;
    const auto result = DocumentSearchSession::create(f.source, std::string(10000, 'x'));
    REQUIRE_FALSE(result);
    CHECK(result.error().error.code == SourceError::PAGE_TOO_LARGE);
    CHECK(result.error().error.budgetDemand);
    CHECK(f.budget->usage().bytes == used);
    CHECK(f.input->reads == 0);
  }
  SUBCASE("external pressure releases for a retry") {
    Fixture f("needle", 64, {.maxBytes = 128 * 1024});
    auto session = f.search("needle", {.workBytes = 64});
    auto pressure = take(f.budget->reserve({f.budget->options().maxBytes - f.budget->usage().bytes - 1024, 0, 0, 0}));
    const auto result = worker([&] {
      return session->advance();
    });
    REQUIRE_FALSE(result);
    CHECK(result.error().error.code == SourceError::WOULD_BLOCK);
    CHECK(result.error().error.budgetDemand);
    CHECK(result.error().error.budgetPeak);
    pressure = {};
    CHECK(take(worker([&] {
            return finish(*session);
          }))
            .state == DocumentSearchState::FOUND);
  }
}

TEST_CASE("document search anchors above four GiB do not truncate to a matching low offset") {
  Fixture f("needle", 2);
  auto session = f.search("needle");
  const auto result = take(worker([&] {
    REQUIRE(session->start(DocumentSearchDirection::NEXT, std::uint64_t{1} << 32));
    return finish(*session);
  }));
  CHECK(result.state == DocumentSearchState::END);
  CHECK(result.scannedByteOffset == 6);
}
