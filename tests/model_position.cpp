// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <portals/model_position_finite.hpp>

#include <array>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace position_allocation_test {
thread_local int remaining{-1};
}

void *operator new(std::size_t bytes) {
  if (position_allocation_test::remaining == 0) {
    position_allocation_test::remaining = -1;

    throw std::bad_alloc();
  }

  if (position_allocation_test::remaining > 0) {
    --position_allocation_test::remaining;
  }

  if (auto result = std::malloc(bytes ? bytes : 1)) {
    return result;
  }

  throw std::bad_alloc();
}

void *operator new[](std::size_t bytes) {
  return ::operator new(bytes);
}

void operator delete(void *value) noexcept {
  std::free(value);
}

void operator delete(void *value, std::size_t) noexcept {
  std::free(value);
}

void operator delete[](void *value) noexcept {
  std::free(value);
}

void operator delete[](void *value, std::size_t) noexcept {
  std::free(value);
}

namespace {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> value) {
  REQUIRE(value.has_value());

  return std::move(*value);
}

constexpr ModelResource RESOURCE{{11, 1}, 1};
constexpr SourceStamp SESSION{12, 1};

class Cities final : public ModelSnapshot<int> {
public:
  Cities(std::uint64_t revision, std::initializer_list<RowId> ids) : ModelSnapshot(revision, ids.size()) {
    for (auto id : ids) {
      index_.emplace(std::pair{id.value, id.generation}, rows_.size());
      rows_.push_back({id, static_cast<int>(id.value)});
    }
  }

  ModelRows<int> read(RowRange range) const override {
    ++reads;

    if (onRead) {
      onRead();
    }

    if (failRead) {
      throw std::runtime_error("read failed");
    }

    if (pending) {
      return {};
    }

    return {RangeState::READY, std::span(rows_).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }

  RowLookup locateRow(RowId id) const override {
    ++lookups;

    if (onLookup) {
      onLookup();
    }

    if (failLookup) {
      throw std::runtime_error("lookup failed");
    }

    if (!indexed) {
      return {};
    }

    auto found = index_.find({id.value, id.generation});

    return found == index_.end() ? RowLookup{RowLookupState::NOT_FOUND, 0}
                                 : RowLookup{RowLookupState::FOUND, found->second};
  }

  bool indexed{true};
  bool pending{};
  bool failRead{};
  bool failLookup{};
  std::function<void()> onRead;
  std::function<void()> onLookup;
  mutable int reads{};
  mutable int lookups{};

private:
  std::vector<ModelRow<int>> rows_;
  std::map<std::pair<std::uint64_t, std::uint64_t>, std::uint64_t> index_;
};

ModelBookmark item(RowId id, std::uint64_t offset = 2, std::uint64_t revision = 1,
                   PositionAffinity affinity = PositionAffinity::FORWARD) {
  return take(ModelBookmark::create(RESOURCE, {PositionKind::ITEM, id, offset, revision, affinity}));
}

PositionRequest request(ModelBookmark bookmark, std::uint64_t revision = 2) {
  return {SESSION, revision, 7, std::move(bookmark)};
}

PositionChangeMap changes(std::initializer_list<PositionChange> operations, std::uint64_t before = 4) {
  return take(PositionChangeMap::create(1, 2, before, std::span(operations.begin(), operations.size())));
}

auto resolver(const std::shared_ptr<Cities> &snapshot, PositionChangeMap map = {}) {
  return take(FinitePositionResolver<int>::create(RESOURCE, SESSION, snapshot, std::move(map)));
}
} // namespace

TEST_CASE("bookmarks own bounded metadata without retaining a model") {
  std::array<std::byte, 3> data{std::byte{1}, std::byte{2}, std::byte{3}};
  auto bookmark = take(ModelBookmark::create(RESOURCE, {PositionKind::ITEM, {3, 1}, 2, 1}, data));
  auto copied = bookmark;

  data[0] = std::byte{9};

  CHECK(copied.metadata()[0] == std::byte{1});
  CHECK(copied.resource() == RESOURCE);
  CHECK(copied.formatVersion() == ModelBookmark::FORMAT_VERSION);
  CHECK(bookmark.storageBytes() >= sizeof(ModelBookmark) + data.size());

  BookmarkOptions tiny;
  tiny.maxBytes = sizeof(ModelBookmark) + 2;

  CHECK(ModelBookmark::create(RESOURCE, {PositionKind::ITEM, {3, 1}, 2, 1}, data, tiny).error() ==
        SourceError::WOULD_BLOCK);
  CHECK(ModelBookmark::create({}, {PositionKind::ITEM, {3, 1}, 2, 1}).error() == SourceError::INVALID_ARGUMENT);

  auto moved = std::move(copied);

  CHECK(copied.metadata().empty());
  CHECK(copied.position().revision == 0);
  CHECK(moved.metadata()[0] == std::byte{1});
}

TEST_CASE("exact identity survives insertions and reorder without scanning rows") {
  auto snapshot = std::make_shared<Cities>(2, std::initializer_list<RowId>{{4, 1}, {3, 1}, {1, 1}, {2, 1}});
  auto navigation = resolver(snapshot);
  auto reply = take(navigation.resolve(request(item({3, 1}))));

  CHECK(reply.state == PositionResolution::EXACT);
  CHECK(reply.position.row == RowId{3, 1});
  CHECK(reply.position.offset == 1);
  CHECK(reply.position.revision == 2);
  CHECK(snapshot->lookups == 1);
  CHECK(snapshot->reads == 1);
  CHECK(positionReplyMatches(request(item({3, 1})), reply));
}

TEST_CASE("removed current chooses successor and final current chooses predecessor") {
  auto middle = std::make_shared<Cities>(2, std::initializer_list<RowId>{{1, 1}, {2, 1}, {4, 1}});
  auto navigation = resolver(middle, changes({PositionChange::splice(2, 1, 0)}));
  auto reply = take(navigation.resolve(request(item({3, 1}))));

  CHECK(reply.state == PositionResolution::NEAREST);
  CHECK(reply.position.row == RowId{4, 1});

  auto end = std::make_shared<Cities>(2, std::initializer_list<RowId>{{1, 1}, {2, 1}, {3, 1}});
  auto finalNavigation = resolver(end, changes({PositionChange::splice(3, 1, 0)}));
  auto finalReply = take(finalNavigation.resolve(request(item({4, 1}, 3))));

  CHECK(finalReply.state == PositionResolution::NEAREST);
  CHECK(finalReply.position.row == RowId{3, 1});
}

TEST_CASE("range removal and reverse affinity retain the declared place") {
  auto snapshot = std::make_shared<Cities>(2, std::initializer_list<RowId>{{1, 1}, {2, 1}, {5, 1}});
  auto navigation = resolver(snapshot, changes({PositionChange::splice(2, 2, 0)}, 5));
  auto forward = take(navigation.resolve(request(item({3, 1}))));
  auto backward = take(navigation.resolve(request(item({3, 1}, 2, 1, PositionAffinity::BACKWARD))));

  CHECK(forward.position.row == RowId{5, 1});
  CHECK(backward.position.row == RowId{2, 1});

  auto empty = std::make_shared<Cities>(2, std::initializer_list<RowId>{});
  auto emptyNavigation = resolver(empty, changes({PositionChange::splice(0, 4, 0)}));
  auto emptyReply = take(emptyNavigation.resolve(request(item({3, 1}))));

  CHECK(emptyReply.state == PositionResolution::EMPTY);
  CHECK(emptyReply.position.row == RowId{});
}

TEST_CASE("proven sequential changes map position before deleting its item") {
  auto snapshot = std::make_shared<Cities>(2, std::initializer_list<RowId>{{8, 1}, {1, 1}, {4, 1}});
  auto navigation = resolver(snapshot, changes({PositionChange::splice(0, 0, 1), PositionChange::splice(2, 2, 0)}));
  auto reply = take(navigation.resolve(request(item({3, 1}))));

  CHECK(reply.state == PositionResolution::NEAREST);
  CHECK(reply.position.row == RowId{4, 1});
  CHECK(reply.position.offset == 2);
}

TEST_CASE("move maps a deleted item in its new order") {
  auto snapshot = std::make_shared<Cities>(2, std::initializer_list<RowId>{{1, 1}, {4, 1}, {2, 1}});
  auto navigation = resolver(
    snapshot, changes({PositionChange::move(2, 1, 0), PositionChange::splice(0, 1, 0), PositionChange::move(1, 1, 2)}));
  auto reply = take(navigation.resolve(request(item({3, 1}))));

  CHECK(reply.state == PositionResolution::NEAREST);
  CHECK(reply.position.row == RowId{1, 1});
}

TEST_CASE("missing identity is not clamped without proven changes") {
  auto snapshot = std::make_shared<Cities>(2, std::initializer_list<RowId>{{1, 1}, {2, 1}, {4, 1}});
  auto reply = take(resolver(snapshot).resolve(request(item({3, 1}))));

  CHECK(reply.state == PositionResolution::INCOMPATIBLE);

  snapshot->indexed = false;

  auto unsupported = take(resolver(snapshot).resolve(request(item({3, 1}))));

  CHECK(unsupported.state == PositionResolution::UNSUPPORTED);
  CHECK(snapshot->reads == 0);
}

TEST_CASE("source lineage and full generation prevent false exact matches") {
  auto snapshot = std::make_shared<Cities>(2, std::initializer_list<RowId>{{1, 1}, {2, 1}, {3, 2}, {4, 1}});
  auto navigation = resolver(snapshot);
  auto reply = take(navigation.resolve(request(item({3, 1}))));

  CHECK(reply.state == PositionResolution::INCOMPATIBLE);

  auto foreign = take(ModelBookmark::create({RESOURCE.identity, 2}, {PositionKind::ITEM, {3, 2}, 2, 1}));
  auto incompatible = take(navigation.resolve(request(foreign)));

  CHECK(incompatible.state == PositionResolution::INCOMPATIBLE);
  CHECK(snapshot->lookups == 1);

  auto wrongSession = request(item({3, 2}));
  wrongSession.session.epoch++;

  CHECK(navigation.resolve(wrongSession).error() == SourceError::STALE_SOURCE);
}

TEST_CASE("pending and errors do not turn into navigation to zero") {
  auto snapshot = std::make_shared<Cities>(2, std::initializer_list<RowId>{{1, 1}, {2, 1}, {3, 1}, {4, 1}});
  auto navigation = resolver(snapshot);

  snapshot->pending = true;

  CHECK(take(navigation.resolve(request(item({3, 1})))).state == PositionResolution::PENDING);

  snapshot->pending = false;
  snapshot->failRead = true;

  CHECK(navigation.resolve(request(item({3, 1}))).error() == SourceError::CALLBACK_FAILED);

  snapshot->failRead = false;
  snapshot->failLookup = true;

  CHECK(navigation.resolve(request(item({3, 1}))).error() == SourceError::CALLBACK_FAILED);
}

TEST_CASE("finite resolver neither owns its snapshot nor accepts stale completions") {
  auto snapshot = std::make_shared<Cities>(2, std::initializer_list<RowId>{{1, 1}});
  auto navigation = resolver(snapshot);
  auto command = request(item({1, 1}, 0));
  auto reply = take(navigation.resolve(command));

  CHECK(positionReplyMatches(command, reply));

  ++command.sequence;

  CHECK_FALSE(positionReplyMatches(command, reply));

  auto bookmark = item({1, 1}, 0);
  snapshot.reset();

  CHECK(navigation.resolve(request(bookmark)).error() == SourceError::CLOSED);
  CHECK(bookmark.position().row == RowId{1, 1});
}

TEST_CASE("change maps reject impossible edits and obey operation budget") {
  std::array invalid{PositionChange::splice(3, 2, 0)};

  CHECK(PositionChangeMap::create(1, 2, 4, invalid).error() == SourceError::INVALID_ARGUMENT);
  CHECK(PositionChangeMap::create(2, 1, 4, {}).error() == SourceError::INVALID_ARGUMENT);

  std::array valid{PositionChange::splice(0, 0, 1), PositionChange::splice(0, 1, 0)};
  PositionMapOptions options;
  options.maxChanges = 1;

  CHECK(PositionChangeMap::create(1, 2, 4, valid, options).error() == SourceError::WOULD_BLOCK);
}

TEST_CASE("gap affinity is mapped separately from item selection") {
  auto snapshot = std::make_shared<Cities>(2, std::initializer_list<RowId>{{1, 1}, {8, 1}, {2, 1}, {3, 1}, {4, 1}});
  auto navigation = resolver(snapshot, changes({PositionChange::splice(1, 0, 1)}));
  auto left = take(ModelBookmark::create(RESOURCE, {PositionKind::GAP, {}, 1, 1, PositionAffinity::BACKWARD}));
  auto right = take(ModelBookmark::create(RESOURCE, {PositionKind::GAP, {}, 1, 1, PositionAffinity::FORWARD}));
  auto leftReply = take(navigation.resolve(request(left)));
  auto rightReply = take(navigation.resolve(request(right)));

  CHECK(leftReply.state == PositionResolution::EXACT);
  CHECK(leftReply.position.offset == 1);
  CHECK(rightReply.position.offset == 2);
  CHECK(rightReply.position.row == RowId{});
  CHECK(snapshot->reads == 0);
}

TEST_CASE("finite resolution rejects stale revision and cancellation before provider access") {
  auto snapshot = std::make_shared<Cities>(2, std::initializer_list<RowId>{{1, 1}});
  auto navigation = resolver(snapshot);
  auto command = request(item({1, 1}, 0));
  std::stop_source stop;

  stop.request_stop();

  CHECK(navigation.resolve(command, stop.get_token()).error() == SourceError::CANCELLED);
  CHECK(navigation.resolve(request(item({1, 1}, 0), 3)).error() == SourceError::REVISION_CHANGED);
  CHECK(snapshot->lookups == 0);
  CHECK(snapshot->reads == 0);
}

TEST_CASE("text marks remain a distinct unsupported capability of finite rows") {
  auto snapshot = std::make_shared<Cities>(2, std::initializer_list<RowId>{{1, 1}});
  auto bookmark = take(ModelBookmark::create(RESOURCE, {PositionKind::TEXT, {}, 1000000, 1}));
  auto reply = take(resolver(snapshot).resolve(request(bookmark)));

  CHECK(reply.state == PositionResolution::UNSUPPORTED);
  CHECK(snapshot->lookups == 0);
  CHECK(snapshot->reads == 0);
}

TEST_CASE("replacement and equivalent erase then insert preserve the same surviving neighbor") {
  auto snapshot = std::make_shared<Cities>(2, std::initializer_list<RowId>{{1, 1}, {2, 1}, {8, 1}, {4, 1}});
  auto combined = resolver(snapshot, changes({PositionChange::splice(2, 1, 1)}));
  auto split = resolver(snapshot, changes({PositionChange::splice(2, 1, 0), PositionChange::splice(2, 0, 1)}));

  for (auto affinity : {PositionAffinity::FORWARD, PositionAffinity::BACKWARD}) {
    auto bookmark = item({3, 1}, 2, 1, affinity);
    auto first = take(combined.resolve(request(bookmark)));
    auto second = take(split.resolve(request(bookmark)));

    CHECK(first.state == PositionResolution::NEAREST);
    CHECK(first.position.row == second.position.row);
    CHECK(first.position.offset == second.position.offset);
    CHECK(first.position.row == (affinity == PositionAffinity::FORWARD ? RowId{4, 1} : RowId{2, 1}));
  }
}

TEST_CASE("empty target without complete history is incompatible rather than an inferred deletion") {
  auto snapshot = std::make_shared<Cities>(2, std::initializer_list<RowId>{});
  auto reply = take(resolver(snapshot).resolve(request(item({3, 1}))));

  CHECK(reply.state == PositionResolution::INCOMPATIBLE);
  CHECK(snapshot->reads == 0);
}

TEST_CASE("capture issues owning bookmarks and resource factory reuses unique source namespaces") {
  auto first = take(ModelResource::create());
  auto second = take(ModelResource::create());

  CHECK(first.lineage == 1);
  CHECK(first.identity != second.identity);

  auto snapshot = std::make_shared<Cities>(2, std::initializer_list<RowId>{{1, 1}, {2, 1}});
  auto navigation = take(FinitePositionResolver<int>::create(first, first.identity, snapshot));
  auto bookmark = take(navigation.capture(1));

  CHECK(bookmark.resource() == first);
  CHECK(bookmark.position().row == RowId{2, 1});
  CHECK(bookmark.position().offset == 1);
  CHECK(bookmark.position().revision == 2);
  CHECK(snapshot->reads == 1);
  CHECK(snapshot->lookups == 0);

  snapshot->pending = true;

  CHECK(navigation.capture(1).error() == SourceError::WOULD_BLOCK);

  snapshot.reset();

  CHECK(navigation.capture(1).error() == SourceError::CLOSED);
  CHECK(bookmark.position().row == RowId{2, 1});
}

TEST_CASE("metadata and change-map allocation failures preserve reusable values") {
  std::array<std::byte, 3> metadata{};
  auto original = item({3, 1});

  position_allocation_test::remaining = 0;

  auto failed = ModelBookmark::create(RESOURCE, original.position(), metadata);

  CHECK(failed.error() == SourceError::NO_MEMORY);
  CHECK(original.position().row == RowId{3, 1});

  std::array operation{PositionChange::splice(2, 1, 0)};

  for (int allocation = 0; allocation != 2; ++allocation) {
    position_allocation_test::remaining = allocation;

    auto failedMap = PositionChangeMap::create(1, 2, 4, operation);

    CHECK(failedMap.error() == SourceError::NO_MEMORY);
  }

  auto map = take(PositionChangeMap::create(1, 2, 4, operation));

  position_allocation_test::remaining = 0;

  auto copied = map;
  auto copiedBookmark = original;

  const auto remaining = std::exchange(position_allocation_test::remaining, -1);

  CHECK(remaining == 0);
  CHECK(copied.toRevision() == 2);
  CHECK(copiedBookmark.position().row == RowId{3, 1});
}

TEST_CASE("provider reentry can release the resolver owner without invalidating in-flight values") {
  auto snapshot = std::make_shared<Cities>(2, std::initializer_list<RowId>{{1, 1}, {2, 1}, {4, 1}});
  auto owner =
    std::make_unique<FinitePositionResolver<int>>(resolver(snapshot, changes({PositionChange::splice(2, 1, 0)})));

  snapshot->onLookup = [&] {
    owner.reset();
  };

  auto reply = take(owner->resolve(request(item({3, 1}))));

  CHECK_FALSE(owner);
  CHECK(reply.state == PositionResolution::NEAREST);
  CHECK(reply.position.row == RowId{4, 1});

  owner = std::make_unique<FinitePositionResolver<int>>(resolver(snapshot));
  snapshot->onLookup = {};
  snapshot->onRead = [&] {
    owner.reset();
  };

  auto bookmark = take(owner->capture(1));

  CHECK_FALSE(owner);
  CHECK(bookmark.resource() == RESOURCE);
  CHECK(bookmark.position().row == RowId{2, 1});
}

TEST_CASE("map arithmetic and bookmark limits reject oversized inputs before allocation") {
  std::array overflow{PositionChange::splice(std::numeric_limits<std::uint64_t>::max(), 0, 1)};

  CHECK(PositionChangeMap::create(1, 2, std::numeric_limits<std::uint64_t>::max(), overflow).error() ==
        SourceError::INVALID_ARGUMENT);

  std::array<std::byte, 4096> tooLarge{};

  CHECK(ModelBookmark::create(RESOURCE, {PositionKind::ITEM, {3, 1}, 2, 1}, tooLarge).error() ==
        SourceError::WOULD_BLOCK);
}
