// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <portals/text_history.hpp>

#include <cstdlib>
#include <new>
#include <random>
#include <type_traits>
#include <vector>

namespace allocationTest {
thread_local std::ptrdiff_t remaining{-1};

// GCC otherwise inlines the malloc-backed replacement delete and diagnoses a
// false allocation/deallocation mismatch at ordinary new expressions.
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

static_assert(std::is_nothrow_move_constructible_v<PreparedTextHistory>);
static_assert(std::is_nothrow_move_assignable_v<PreparedTextHistory>);
static_assert(TextHistoryOptions{}.maxOperations == 128);
static_assert(TextHistoryOptions{}.maxBytes == 1048576);

std::string utf8(std::u8string_view value) {
  return {reinterpret_cast<const char *>(value.data()), value.size()};
}

TextHistorySnapshot snapshot(std::string bytes) {
  const auto end = bytes.size();

  return {std::move(bytes), end, end};
}

void edit(TextHistory &history, const TextHistorySnapshot &before, const TextHistorySnapshot &after,
          TextHistoryKind kind = TextHistoryKind::PROGRAMMATIC) {
  auto prepared = history.prepareEdit(before, after, kind);

  REQUIRE(prepared);
  REQUIRE(history.commit(std::move(*prepared)));
}

TextHistorySnapshot restore(TextHistory &history, bool undo) {
  auto prepared = undo ? history.prepareUndo() : history.prepareRedo();
  auto text = GraphemeText::from("");

  REQUIRE(prepared);
  REQUIRE(text);
  REQUIRE(history.checkPrepared(*prepared));

  const auto cursor = prepared->cursor();
  const auto anchor = prepared->anchor();

  prepared->takeText(*text);

  REQUIRE(history.commit(std::move(*prepared)));

  return {std::string(text->bytes()), cursor, anchor};
}

void checkSnapshot(const TextHistorySnapshot &actual, const TextHistorySnapshot &expected) {
  CHECK(actual.bytes == expected.bytes);
  CHECK(actual.cursor == expected.cursor);
  CHECK(actual.anchor == expected.anchor);
}
} // namespace

TEST_CASE("typing and directional deletion group independently with explicit boundaries") {
  TextHistory history;

  edit(history, snapshot(""), snapshot("a"), TextHistoryKind::TYPING);
  edit(history, snapshot("a"), snapshot("ab"), TextHistoryKind::TYPING);
  CHECK(history.operationCount() == 1);
  checkSnapshot(restore(history, true), snapshot(""));
  checkSnapshot(restore(history, false), snapshot("ab"));

  edit(history, snapshot("ab"), snapshot("abc"), TextHistoryKind::TYPING);
  CHECK(history.operationCount() == 2);

  history.stopGrouping();
  edit(history, snapshot("abc"), snapshot("abcd"), TextHistoryKind::TYPING);
  edit(history, snapshot("abcd"), snapshot("abc"), TextHistoryKind::BACKSPACE);
  edit(history, snapshot("abc"), snapshot("ab"), TextHistoryKind::BACKSPACE);
  CHECK(history.operationCount() == 4);
  checkSnapshot(restore(history, true), snapshot("abcd"));

  TextHistory forward;

  edit(forward, {"abcd", 0, 0}, {"bcd", 0, 0}, TextHistoryKind::FORWARD_DELETE);
  edit(forward, {"bcd", 0, 0}, {"cd", 0, 0}, TextHistoryKind::FORWARD_DELETE);
  edit(forward, {"cd", 0, 0}, {"d", 0, 0}, TextHistoryKind::BACKSPACE);
  CHECK(forward.operationCount() == 2);
  checkSnapshot(restore(forward, true), {"cd", 0, 0});
  checkSnapshot(restore(forward, true), {"abcd", 0, 0});
}

TEST_CASE("selection cursor paste cut replacement and program edits close grouping") {
  for (auto kind :
       {TextHistoryKind::PASTE, TextHistoryKind::CUT, TextHistoryKind::REPLACEMENT, TextHistoryKind::PROGRAMMATIC}) {
    TextHistory history;

    edit(history, snapshot(""), snapshot("a"), kind);
    edit(history, snapshot("a"), snapshot("ab"), kind);
    CHECK(history.operationCount() == 2);
  }

  TextHistory history;

  edit(history, snapshot(""), snapshot("a"), TextHistoryKind::TYPING);
  edit(history, {"a", 0, 0}, {"ba", 1, 1}, TextHistoryKind::TYPING);
  edit(history, {"ba", 0, 2}, snapshot("c"), TextHistoryKind::TYPING);
  CHECK(history.operationCount() == 3);
  checkSnapshot(restore(history, true), {"ba", 0, 2});

  TextHistoryOptions options;

  options.groupTyping = false;
  options.groupDeletion = false;

  TextHistory separate(options);

  edit(separate, snapshot(""), snapshot("a"), TextHistoryKind::TYPING);
  edit(separate, snapshot("a"), snapshot("ab"), TextHistoryKind::TYPING);
  edit(separate, snapshot("ab"), snapshot("a"), TextHistoryKind::BACKSPACE);
  edit(separate, snapshot("a"), snapshot(""), TextHistoryKind::BACKSPACE);
  CHECK(separate.operationCount() == 4);
}

TEST_CASE("owning Unicode snapshots restore grapheme-safe cursor and anchor") {
  TextHistory history;
  auto before = snapshot(utf8(u8"é 👩‍💻 🇺🇳"));
  auto after = snapshot(before.bytes + utf8(u8"Ж"));

  before.cursor = 3;
  before.anchor = before.bytes.size();

  edit(history, before, after, TextHistoryKind::REPLACEMENT);

  const auto expectedBefore = before;
  const auto expectedAfter = after;

  before.bytes.assign("destroyed");
  after.bytes.clear();

  checkSnapshot(restore(history, true), expectedBefore);
  checkSnapshot(restore(history, false), expectedAfter);

  auto inside = expectedBefore;

  inside.cursor = 1;

  const auto invalidCursor = history.prepareEdit(inside, expectedAfter, TextHistoryKind::TYPING);

  REQUIRE_FALSE(invalidCursor);
  CHECK(invalidCursor.error() == TextHistoryError::NOT_BOUNDARY);

  inside = expectedBefore;
  inside.anchor = inside.bytes.size() + 1;

  const auto invalidAnchor = history.prepareResetBaseline(inside);
  const auto invalidUtf8 = history.prepareResetBaseline(snapshot(std::string(1, '\xff')));

  REQUIRE_FALSE(invalidAnchor);
  REQUIRE_FALSE(invalidUtf8);
  CHECK(invalidAnchor.error() == TextHistoryError::NOT_BOUNDARY);
  CHECK(invalidUtf8.error() == TextHistoryError::INVALID_TEXT);
  CHECK(history.operationCount() == 1);
}

TEST_CASE("no-op preserves redo while equal-text external baseline clears everything") {
  TextHistory history;

  edit(history, snapshot(""), snapshot("a"));
  edit(history, snapshot("a"), snapshot("b"));
  checkSnapshot(restore(history, true), snapshot("a"));

  auto noop = history.prepareEdit(snapshot("a"), {"a", 0, 0}, TextHistoryKind::TYPING);
  auto redo = history.prepareRedo();

  REQUIRE(noop);
  REQUIRE(redo);
  CHECK_FALSE(noop->changed());
  REQUIRE(history.commit(std::move(*noop)));
  REQUIRE(history.checkPrepared(*redo));
  CHECK(history.canRedo());

  auto baseline = history.prepareResetBaseline(snapshot("a"));

  REQUIRE(baseline);
  CHECK(baseline->changed());
  REQUIRE(history.commit(std::move(*baseline)));
  CHECK_FALSE(history.canUndo());
  CHECK_FALSE(history.canRedo());
  CHECK(history.retainedBytes() == 0);
  CHECK_FALSE(history.checkPrepared(*redo));
}

TEST_CASE("branching count eviction and immutable staging preserve live history") {
  TextHistoryOptions options;

  options.maxOperations = 2;

  TextHistory history(options);

  edit(history, snapshot("0"), snapshot("1"));
  edit(history, snapshot("1"), snapshot("2"));

  auto abandoned = history.prepareEdit(snapshot("2"), snapshot("3"), TextHistoryKind::PROGRAMMATIC);

  REQUIRE(abandoned);
  CHECK(history.operationCount() == 2);
  checkSnapshot(restore(history, true), snapshot("1"));
  CHECK(history.operationCount() == 2);
  CHECK(history.canRedo());
  CHECK_FALSE(history.commit(std::move(*abandoned)));

  edit(history, snapshot("1"), snapshot("branch"));
  CHECK_FALSE(history.canRedo());
  edit(history, snapshot("branch"), snapshot("last"));
  CHECK(history.operationCount() == 2);
  checkSnapshot(restore(history, true), snapshot("branch"));
  checkSnapshot(restore(history, true), snapshot("1"));
  CHECK_FALSE(history.canUndo());

  const auto empty = history.prepareUndo();

  REQUIRE_FALSE(empty);
  CHECK(empty.error() == TextHistoryError::EMPTY);
}

TEST_CASE("finite byte budgets reject single oversized edits and split oversized merges") {
  TextHistory reference;
  const auto large = snapshot(std::string(400, 'x'));

  edit(reference, snapshot(""), large);

  TextHistoryOptions options;

  options.maxBytes = reference.retainedBytes();

  TextHistory exact(options);

  edit(exact, snapshot(""), large);
  CHECK(exact.retainedBytes() == options.maxBytes);

  options.maxBytes -= 1;

  TextHistory rejected(options);
  const auto oversized = rejected.prepareEdit(snapshot(""), large, TextHistoryKind::PASTE);

  REQUIRE_FALSE(oversized);
  CHECK(oversized.error() == TextHistoryError::WOULD_BLOCK);
  CHECK(rejected.operationCount() == 0);

  const auto big = snapshot(std::string(600, 'x'));
  const auto empty = snapshot("");
  TextHistory firstReference;

  edit(firstReference, big, empty, TextHistoryKind::TYPING);
  options.maxBytes = firstReference.retainedBytes();

  TextHistory split(options);

  // The value layer accepts caller-provided categories. This unusual pair
  // makes the merged before/after larger than either separate entry.
  edit(split, big, empty, TextHistoryKind::TYPING);
  edit(split, empty, big, TextHistoryKind::TYPING);
  CHECK(split.operationCount() == 1);
  checkSnapshot(restore(split, true), empty);

  TextHistory bounded(options);

  edit(bounded, big, empty);
  checkSnapshot(restore(bounded, true), big);

  const auto oldBytes = bounded.retainedBytes();
  const auto rejectedBranch = bounded.prepareEdit(big, snapshot(std::string(5000, 'y')), TextHistoryKind::PASTE);

  REQUIRE_FALSE(rejectedBranch);
  CHECK(rejectedBranch.error() == TextHistoryError::WOULD_BLOCK);
  CHECK(bounded.canRedo());
  CHECK(bounded.retainedBytes() == oldBytes);
}

TEST_CASE("disabled and zero bounds accept oversized text without retention") {
  for (int mode = 0; mode != 3; ++mode) {
    TextHistoryOptions options;

    options.enabled = mode != 0;
    options.maxOperations = mode == 1 ? 0 : 128;
    options.maxBytes = mode == 2 ? 0 : 1;

    TextHistory history(options);

    edit(history, snapshot(""), snapshot(std::string(5000, 'x')));
    CHECK(history.operationCount() == 0);
    CHECK(history.retainedBytes() == 0);
    CHECK_FALSE(history.canUndo());
    CHECK_FALSE(history.canRedo());
  }
}

TEST_CASE("owner identity and explicit boundaries reject stale staging before publication") {
  TextHistory first;
  TextHistory second;
  auto prepared = first.prepareEdit(snapshot(""), snapshot("a"), TextHistoryKind::TYPING);

  REQUIRE(prepared);
  CHECK_FALSE(second.checkPrepared(*prepared));
  CHECK_FALSE(second.commit(std::move(*prepared)));
  REQUIRE(first.checkPrepared(*prepared));

  first.stopGrouping();

  CHECK_FALSE(first.checkPrepared(*prepared));
  CHECK_FALSE(first.commit(std::move(*prepared)));
  CHECK(first.operationCount() == 0);

  edit(first, snapshot(""), snapshot("a"));

  const auto mismatch = first.prepareEdit(snapshot("other"), snapshot("b"), TextHistoryKind::PROGRAMMATIC);

  REQUIRE_FALSE(mismatch);
  CHECK(mismatch.error() == TextHistoryError::STALE);
}

TEST_CASE("captured text limits and width policy apply before history changes") {
  TextLimits limits;

  limits.max_input_bytes = 3;

  TextHistory limited({}, {}, limits);
  const auto tooLong = limited.prepareResetBaseline(snapshot("abcd"));

  REQUIRE_FALSE(tooLong);
  CHECK(tooLong.error() == TextHistoryError::INVALID_TEXT);

  WidthPolicy policy;

  policy.ambiguous = AmbiguousWidth::WIDE;
  policy.revision = 17;

  TextHistory history({}, policy);
  auto prepared = history.prepareResetBaseline(snapshot(utf8(u8"¡")));
  auto text = GraphemeText::from("");

  REQUIRE(prepared);
  REQUIRE(text);

  prepared->takeText(*text);

  CHECK(text->policy() == policy);
  CHECK(text->measure() == 2);
  REQUIRE(history.commit(std::move(*prepared)));
}

TEST_CASE("global allocation failure sweeps preserve undo redo and allocation-free commit") {
  for (int action = 0; action != 5; ++action) {
    TextHistory history;
    const auto empty = snapshot("");
    const auto first = snapshot(std::string(80, 'a'));
    const auto second = snapshot(std::string(160, 'b'));
    const auto third = snapshot(std::string(240, 'c'));

    edit(history, empty, first, TextHistoryKind::TYPING);
    if (action != 4) {
      edit(history, first, second);
      checkSnapshot(restore(history, true), first);
    }

    const auto retained = history.retainedBytes();
    auto destination = GraphemeText::from(first.bytes);
    bool succeeded = false;
    int failures = 0;

    REQUIRE(destination);

    for (std::ptrdiff_t allowance = 0; allowance != 200; ++allowance) {
      allocationTest::remaining = allowance;

      auto prepared = action == 0   ? history.prepareEdit(first, third, TextHistoryKind::PROGRAMMATIC)
                      : action == 1 ? history.prepareUndo()
                      : action == 2 ? history.prepareRedo()
                      : action == 3 ? history.prepareResetBaseline(first)
                                    : history.prepareEdit(first, third, TextHistoryKind::TYPING);

      allocationTest::remaining = -1;

      if (!prepared) {
        CHECK(prepared.error() == TextHistoryError::NO_MEMORY);
        CHECK(history.retainedBytes() == retained);
        CHECK(history.operationCount() == (action == 4 ? 1 : 2));
        CHECK(history.canUndo());
        CHECK(history.canRedo() == (action != 4));
        ++failures;

        continue;
      }

      allocationTest::remaining = 0;

      const auto checked = history.checkPrepared(*prepared);

      prepared->takeText(*destination);

      const auto committed = history.commit(std::move(*prepared));

      allocationTest::remaining = -1;

      REQUIRE(checked);
      REQUIRE(committed);
      CHECK(destination->bytes() == (action == 1   ? empty.bytes
                                     : action == 2 ? second.bytes
                                     : action == 3 ? first.bytes
                                                   : third.bytes));
      succeeded = true;

      break;
    }

    CHECK(succeeded);
    CHECK(failures > 0);
  }
}

TEST_CASE("deterministic random edits match independent string oracle through branching and eviction") {
  constexpr std::size_t GROUP_LIMIT = 32;
  TextHistoryOptions options;

  options.maxOperations = GROUP_LIMIT;

  TextHistory history(options);
  std::mt19937 random(0x713489u);
  std::vector<TextHistorySnapshot> oracle{snapshot("")};
  std::size_t position = 0;

  for (int step = 0; step != 1200; ++step) {
    const auto action = random() % 6;

    if (action == 0 && position != 0) {
      --position;
      checkSnapshot(restore(history, true), oracle[position]);
    } else if (action == 1 && position + 1 < oracle.size()) {
      ++position;
      checkSnapshot(restore(history, false), oracle[position]);
    } else {
      const auto before = oracle[position];
      auto after = before;
      const auto offset = static_cast<std::size_t>(random()) % (after.bytes.size() + 1);

      if (action == 2 && !after.bytes.empty()) {
        const auto eraseAt = offset == after.bytes.size() ? offset - 1 : offset;

        after.bytes.erase(eraseAt, 1);
        after.cursor = eraseAt;
      } else {
        after.bytes.insert(offset, 1, static_cast<char>('a' + random() % 26));
        after.cursor = offset + 1;
      }

      after.anchor = random() % (after.bytes.size() + 1);

      edit(history, before, after);
      oracle.resize(position + 1);
      oracle.push_back(after);
      ++position;

      if (position > GROUP_LIMIT) {
        oracle.erase(oracle.begin());
        --position;
      }
    }

    CHECK(history.canUndo() == (position != 0));
    CHECK(history.canRedo() == (position + 1 < oracle.size()));
    CHECK(history.operationCount() == oracle.size() - 1);
  }

  while (position != 0) {
    --position;
    checkSnapshot(restore(history, true), oracle[position]);
  }

  while (position + 1 < oracle.size()) {
    ++position;
    checkSnapshot(restore(history, false), oracle[position]);
  }
}
