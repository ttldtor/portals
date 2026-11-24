// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <portals/text_document.hpp>

#include <cstdlib>
#include <new>
#include <type_traits>

namespace allocationTest {
thread_local std::ptrdiff_t remaining{-1};

#if defined(__GNUC__)
[[gnu::noinline]]
#endif
void release(void *value) noexcept {
  std::free(value);
}
} // namespace allocationTest

void *operator new(std::size_t size) {
  if (allocationTest::remaining == 0) {
    throw std::bad_alloc();
  }

  if (allocationTest::remaining > 0) {
    --allocationTest::remaining;
  }

  if (auto *value = std::malloc(size == 0 ? 1 : size)) {
    return value;
  }

  throw std::bad_alloc();
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete(void *value) noexcept {
  allocationTest::release(value);
}

void operator delete(void *value, std::size_t) noexcept {
  ::operator delete(value);
}

void operator delete[](void *value) noexcept {
  ::operator delete(value);
}

void operator delete[](void *value, std::size_t) noexcept {
  ::operator delete(value);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}

void *operator new[](std::size_t size, const std::nothrow_t &tag) noexcept {
  return ::operator new(size, tag);
}

void operator delete(void *value, const std::nothrow_t &) noexcept {
  ::operator delete(value);
}

void operator delete[](void *value, const std::nothrow_t &) noexcept {
  ::operator delete(value);
}

namespace {
using namespace portals;

std::string utf8(std::u8string_view text) {
  return {reinterpret_cast<const char *>(text.data()), text.size()};
}

std::unique_ptr<FiniteTextDocument> document(std::string_view text, FiniteTextDocumentOptions options = {}) {
  auto result = FiniteTextDocument::create(text, options);

  REQUIRE(result);

  return std::move(*result);
}

ModelBookmark mark(FiniteTextDocument &doc, std::size_t byteOffset,
                   PositionAffinity affinity = PositionAffinity::FORWARD) {
  auto result = doc.bookmark(byteOffset, affinity);

  REQUIRE(result);
  CHECK(result->position().kind == PositionKind::TEXT);
  CHECK(result->metadata().empty());

  return *result;
}

PositionReply resolve(FiniteTextDocument &doc, const ModelBookmark &bookmark) {
  PositionRequest request{doc.session(), doc.revision(), 1, bookmark};
  auto result = doc.resolve(request);

  REQUIRE(result);
  CHECK(positionReplyMatches(request, *result));

  return *result;
}

std::size_t offset(FiniteTextDocument &doc, const ModelBookmark &bookmark) {
  const auto reply = resolve(doc, bookmark);

  REQUIRE((reply.state == PositionResolution::EXACT || reply.state == PositionResolution::NEAREST));

  CHECK(doc.text().nextCursor(static_cast<std::size_t>(reply.position.offset)));

  return static_cast<std::size_t>(reply.position.offset);
}
} // namespace

TEST_CASE("insert affinity is preserved before at and after marks, including repeated clusters") {
  for (const auto affinity : {PositionAffinity::BACKWARD, PositionAffinity::FORWARD}) {
    for (const std::size_t insertion : {0U, 1U, 2U}) {
      auto doc = document("aaa");
      const auto bookmark = mark(*doc, 1, affinity);

      REQUIRE(doc->replace(insertion, insertion, "a"));

      const auto expected = insertion < 1 || (insertion == 1 && affinity == PositionAffinity::FORWARD) ? 2U : 1U;

      CHECK(offset(*doc, bookmark) == expected);
      REQUIRE(doc->undo());
      CHECK(offset(*doc, bookmark) == 1);
      REQUIRE(doc->redo());
      CHECK(offset(*doc, bookmark) == expected);
    }
  }
}

TEST_CASE("deletion marks recover their exact original position after Undo and branch mapping is safe") {
  auto doc = document("abcdef");
  const auto before = mark(*doc, 3, PositionAffinity::BACKWARD);
  const auto after = mark(*doc, 3, PositionAffinity::FORWARD);

  REQUIRE(doc->replace(1, 5, "XY"));
  CHECK(offset(*doc, before) == 1);
  CHECK(offset(*doc, after) == 3);
  CHECK(resolve(*doc, before).state == PositionResolution::NEAREST);

  const auto deletedState = mark(*doc, 2);

  REQUIRE(doc->undo());
  CHECK(offset(*doc, before) == 3);
  CHECK(resolve(*doc, before).state == PositionResolution::EXACT);
  CHECK(offset(*doc, after) == 3);
  REQUIRE(doc->replace(0, 0, "!"));
  CHECK(offset(*doc, before) == 4);
  CHECK(offset(*doc, deletedState) == 6);
  CHECK_FALSE(doc->canRedo());
}

TEST_CASE("combining marks emoji and resegmentation always resolve to actual boundaries") {
  const auto combining = utf8(u8"\u0301");
  auto doc = document("ab");
  const auto backward = mark(*doc, 1, PositionAffinity::BACKWARD);
  const auto forward = mark(*doc, 1, PositionAffinity::FORWARD);

  REQUIRE(doc->replace(1, 1, combining));
  CHECK(doc->text().graphemes().size() == 2);
  CHECK(offset(*doc, backward) == 0);
  CHECK(resolve(*doc, backward).state == PositionResolution::NEAREST);
  CHECK(offset(*doc, forward) == 1 + combining.size());
  CHECK(doc->selection().cursor == 1 + combining.size());
  CHECK_FALSE(doc->setSelection({1, 1}));
  CHECK_FALSE(doc->replace(1, 1, "x"));
  REQUIRE(doc->undo());
  CHECK(offset(*doc, backward) == 1);

  const auto emoji = utf8(u8"\U0001F469\u200D\U0001F4BB");

  REQUIRE(doc->replace(1, 1, emoji));
  CHECK(doc->text().graphemes().size() == 3);
  CHECK(offset(*doc, forward) == 1 + emoji.size());
  REQUIRE(doc->undo());
  REQUIRE(doc->redo());
  CHECK(offset(*doc, forward) == 1 + emoji.size());
  CHECK_FALSE(doc->bookmark(2));
}

TEST_CASE("RI resegmentation snaps around the mapped bytes rather than to the run edges") {
  auto doc = document(utf8(u8"\U0001F1E6\U0001F1E7\U0001F1E8\U0001F1E9\U0001F1EA\U0001F1EB"));
  const auto backward = mark(*doc, 8, PositionAffinity::BACKWARD);
  const auto forward = mark(*doc, 8, PositionAffinity::FORWARD);

  REQUIRE(doc->replace(0, 0, utf8(u8"\U0001F1FD")));
  CHECK(offset(*doc, backward) == 8);
  CHECK(offset(*doc, forward) == 16);
  CHECK(resolve(*doc, backward).state == PositionResolution::NEAREST);
  CHECK(resolve(*doc, forward).state == PositionResolution::NEAREST);
  REQUIRE(doc->undo());
  CHECK(offset(*doc, backward) == 8);
  CHECK(offset(*doc, forward) == 8);
  CHECK(resolve(*doc, backward).state == PositionResolution::EXACT);
  REQUIRE(doc->redo());
  CHECK(offset(*doc, backward) == 8);
  CHECK(offset(*doc, forward) == 16);
}

TEST_CASE("ZWJ insertion can remove both adjacent cluster boundaries") {
  auto doc = document(utf8(u8"\U0001F469\U0001F4BB"));
  const auto backward = mark(*doc, 4, PositionAffinity::BACKWARD);
  const auto forward = mark(*doc, 4, PositionAffinity::FORWARD);

  REQUIRE(doc->replace(4, 4, utf8(u8"\u200D")));
  CHECK(doc->text().graphemes().size() == 1);
  CHECK(offset(*doc, backward) == 0);
  CHECK(offset(*doc, forward) == 11);
  CHECK(resolve(*doc, backward).state == PositionResolution::NEAREST);
  CHECK(resolve(*doc, forward).state == PositionResolution::NEAREST);
  REQUIRE(doc->undo());
  CHECK(offset(*doc, backward) == 4);
  CHECK(offset(*doc, forward) == 4);
}

TEST_CASE("full provider validates request qualification and cancellation") {
  auto doc = document("abc");
  const auto bookmark = mark(*doc, 1);
  PositionRequest request{doc->session(), doc->revision(), 1, bookmark};

  const PositionResolver &resolver = *doc;
  const auto throughInterface = resolver.resolve(request, {});

  REQUIRE(throughInterface);
  CHECK(throughInterface->position.offset == 1);

  std::stop_source stop;

  stop.request_stop();
  CHECK(doc->resolve(request, stop.get_token()).error() == SourceError::CANCELLED);
  request.sequence = 0;
  CHECK_FALSE(doc->resolve(request));
  request.sequence = 1;
  ++request.revision;
  CHECK_FALSE(doc->resolve(request));
  --request.revision;
  ++request.session.epoch;
  CHECK_FALSE(doc->resolve(request));
}

TEST_CASE("typing groups use existing history and selection belongs to the session") {
  auto doc = document("x");
  const auto origin = mark(*doc, 1);

  REQUIRE(doc->setSelection({1, 0}));
  REQUIRE(doc->replace(1, 1, "a", TextHistoryKind::TYPING));
  REQUIRE(doc->replace(2, 2, "b", TextHistoryKind::TYPING));
  REQUIRE(doc->undo());
  CHECK(doc->text().bytes() == "xa");
  CHECK(doc->selection() == TextSelection{2, 2});
  REQUIRE(doc->undo());
  CHECK(doc->text().bytes() == "x");
  CHECK(doc->selection() == TextSelection{1, 0});
  CHECK(offset(*doc, origin) == 1);

  // No view pointers occur in this path: a new view borrows the same session.
  auto *replacementViewSession = doc.get();

  CHECK(replacementViewSession->selection() == TextSelection{1, 0});
  REQUIRE(doc->redo());
  CHECK(offset(*replacementViewSession, origin) == 2);
}

TEST_CASE("save SaveAs stale acknowledgements and history are independently qualified") {
  auto doc = document("abc");
  const auto resource = doc->resource();
  const auto bookmark = mark(*doc, 2);

  REQUIRE(doc->setSelection({2, 1}));

  auto first = doc->beginSave("a.txt");

  REQUIRE(first);
  REQUIRE(doc->replace(3, 3, "x"));
  REQUIRE(doc->acknowledgeSave(*first));
  CHECK(doc->dirty());
  CHECK(doc->saveTarget() == "a.txt");
  CHECK(doc->selection() == TextSelection{4, 4});

  auto saveAs = doc->beginSave("b.txt");

  REQUIRE(saveAs);
  REQUIRE(doc->acknowledgeSave(*saveAs));
  CHECK_FALSE(doc->dirty());
  CHECK(doc->resource() == resource);
  CHECK(doc->saveTarget() == "b.txt");
  CHECK(offset(*doc, bookmark) == 2);
  CHECK(doc->canUndo());
  CHECK_FALSE(doc->acknowledgeSave(*first));
  CHECK(doc->saveTarget() == "b.txt");
  REQUIRE(doc->undo());
  CHECK(doc->dirty());
  CHECK(doc->selection() == TextSelection{2, 1});
  REQUIRE(doc->redo());
  CHECK_FALSE(doc->dirty());

  auto unrelated = document("abcx");

  CHECK_FALSE(unrelated->acknowledgeSave(*saveAs));
  CHECK_FALSE(doc->beginSave(""));
}

TEST_CASE("external replacement is explicit conflict and accepted replacement advances lineage") {
  auto doc = document("old");
  const auto resource = doc->resource();
  const auto bookmark = mark(*doc, 1);
  auto ticket = doc->beginSave("same-path.txt");

  REQUIRE(ticket);
  REQUIRE(doc->replace(3, 3, "!"));
  doc->observeExternalReplacement();
  CHECK(doc->conflicted());
  CHECK(doc->dirty());
  CHECK(doc->text().bytes() == "old!");
  CHECK(doc->canUndo());
  CHECK(doc->replace(0, 0, "?").error() == TextDocumentError::CONFLICT);
  CHECK(doc->undo().error() == TextDocumentError::CONFLICT);
  CHECK(doc->acknowledgeSave(*ticket).error() == TextDocumentError::CONFLICT);
  REQUIRE(doc->acceptExternalReplacement("new"));
  CHECK_FALSE(doc->conflicted());
  CHECK_FALSE(doc->dirty());
  CHECK_FALSE(doc->canUndo());
  CHECK(doc->resource().identity == resource.identity);
  CHECK(doc->resource().lineage == resource.lineage + 1);
  CHECK(resolve(*doc, bookmark).state == PositionResolution::INCOMPATIBLE);
  CHECK(doc->acknowledgeSave(*ticket).error() == TextDocumentError::STALE);
  REQUIRE(doc->replace(3, 3, "!"));
  REQUIRE(doc->undo());
  CHECK(doc->text().bytes() == "new");
  CHECK_FALSE(doc->dirty());
}

TEST_CASE("generic map has bounded metadata independent of huge provider extent") {
  auto identity = ModelResource::create();

  REQUIRE(identity);

  const auto extent = std::uint64_t{1} << 50;
  auto result = TextChangeMap::create(*identity, extent, {2});

  REQUIRE(result);

  auto &map = **result;
  const auto bookmark = map.bookmark(extent - 1);

  REQUIRE(bookmark);

  allocationTest::remaining = 0;
  const auto edit = map.edit({0, 0, 5}, 2);
  const auto reply = map.map(*bookmark);
  allocationTest::remaining = -1;

  REQUIRE(edit);
  REQUIRE(reply);
  CHECK(reply->state == PositionResolution::EXACT);
  CHECK(reply->position.offset == extent + 4);
  REQUIRE(map.edit({1, 2, 0}, 3));
  REQUIRE(map.edit({0, 0, 1}, 4));
  const auto unavailable = map.map(*bookmark);

  REQUIRE(unavailable);
  CHECK(unavailable->state == PositionResolution::UNAVAILABLE);
  CHECK_FALSE(map.edit({extent + 100, 0, 0}, 5));
}

TEST_CASE("metadata expiry and disabled history never fabricate a restored position") {
  FiniteTextDocumentOptions options;

  options.anchors.maxChanges = 1;
  options.history.enabled = false;

  auto doc = document("abc", options);
  const auto bookmark = mark(*doc, 1);

  REQUIRE(doc->replace(0, 0, "x"));
  REQUIRE(doc->replace(0, 0, "y"));
  CHECK_FALSE(doc->canUndo());
  CHECK(resolve(*doc, bookmark).state == PositionResolution::UNAVAILABLE);
}

TEST_CASE("new state admission failures preserve text marks selection history savepoint and conflict") {
  for (int operation = 0; operation != 4; ++operation) {
    bool succeeded{};

    for (std::ptrdiff_t failure = 0; failure != 96 && !succeeded; ++failure) {
      auto doc = document("initial text");

      REQUIRE(doc->setSelection({7, 2}));

      const auto bookmark = mark(*doc, 7);
      const auto identity = doc->resource();
      const auto revision = doc->revision();

      if (operation == 3) {
        doc->observeExternalReplacement();
      }

      allocationTest::remaining = failure;
      std::expected<void, TextDocumentError> result;

      if (operation == 0) {
        result = doc->replace(0, 0, "new text");
      } else if (operation == 1) {
        auto ticket = doc->beginSave("a deliberately longer physical destination.txt");

        if (!ticket) {
          result = std::unexpected(ticket.error());
        }
      } else if (operation == 2) {
        auto map = TextChangeMap::create(identity, 10);

        if (!map) {
          result = std::unexpected(TextDocumentError::NO_MEMORY);
        }
      } else {
        result = doc->acceptExternalReplacement("explicitly accepted new baseline");
      }

      allocationTest::remaining = -1;
      succeeded = result.has_value();

      if (!succeeded) {
        CHECK(result.error() == TextDocumentError::NO_MEMORY);
        CHECK(doc->text().bytes() == "initial text");
        CHECK(doc->selection() == TextSelection{7, 2});
        CHECK(doc->resource() == identity);
        CHECK(doc->revision() == revision);
        CHECK_FALSE(doc->canUndo());
        CHECK(doc->saveTarget().empty());
        CHECK(doc->conflicted() == (operation == 3));
        CHECK(offset(*doc, bookmark) == 7);
      }
    }

    CHECK(succeeded);
  }
}

TEST_CASE("tracked Undo and Redo admission failures cannot publish partial text or metadata") {
  for (const bool redo : {false, true}) {
    bool succeeded{};

    for (std::ptrdiff_t failure = 0; failure != 96 && !succeeded; ++failure) {
      auto doc = document("original");
      const auto bookmark = mark(*doc, 3);

      REQUIRE(doc->replace(0, 0, "!"));

      if (redo) {
        REQUIRE(doc->undo());
      }

      const auto bytes = std::string(doc->text().bytes());
      const auto selection = doc->selection();
      const auto revision = doc->revision();
      const auto markOffset = offset(*doc, bookmark);
      const auto canUndo = doc->canUndo();
      const auto canRedo = doc->canRedo();
      const auto dirty = doc->dirty();

      allocationTest::remaining = failure;
      const auto result = redo ? doc->redo() : doc->undo();
      allocationTest::remaining = -1;
      succeeded = result.has_value();

      if (!succeeded) {
        CHECK(result.error() == TextDocumentError::NO_MEMORY);
        CHECK(doc->text().bytes() == bytes);
        CHECK(doc->selection() == selection);
        CHECK(doc->revision() == revision);
        CHECK(doc->canUndo() == canUndo);
        CHECK(doc->canRedo() == canRedo);
        CHECK(doc->dirty() == dirty);
        CHECK(offset(*doc, bookmark) == markOffset);
      }
    }

    CHECK(succeeded);
  }
}

TEST_CASE("cold finite admission reports allocation failure and rejects invalid finite input") {
  bool succeeded{};

  for (std::ptrdiff_t failure = 0; failure != 96 && !succeeded; ++failure) {
    allocationTest::remaining = failure;
    auto result = FiniteTextDocument::create("a finite initial document");
    allocationTest::remaining = -1;
    succeeded = result.has_value();

    if (!succeeded) {
      CHECK(result.error() == TextDocumentError::NO_MEMORY);
    }
  }

  CHECK(succeeded);
  CHECK(FiniteTextDocument::create("\xff").error() == TextDocumentError::INVALID_TEXT);

  FiniteTextDocumentOptions options;

  options.limits.max_input_bytes = 2;
  CHECK(FiniteTextDocument::create("long", options).error() == TextDocumentError::INVALID_TEXT);
  options.limits.max_input_bytes = 100;
  options.anchors.maxChanges = 0;
  CHECK(FiniteTextDocument::create("text", options).error() == TextDocumentError::INVALID_ARGUMENT);
}
