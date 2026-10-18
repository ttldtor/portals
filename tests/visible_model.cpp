// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <portals/ui.hpp>

#include <atomic>
#include <cstdlib>
#include <numeric>
#include <thread>

namespace {
std::atomic<std::ptrdiff_t> allocationsBeforeFailure{-1};
}

// Process-local failure injection is armed only around the public operation under test.
void *operator new(std::size_t size) {
  if (allocationsBeforeFailure.load() == 0) {
    throw std::bad_alloc();
  }

  if (allocationsBeforeFailure.load() > 0) {
    --allocationsBeforeFailure;
  }

  auto *memory = std::malloc(size ? size : 1);

  if (!memory) {
    throw std::bad_alloc();
  }

  return memory;
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete(void *memory) noexcept {
  std::free(memory);
}

void operator delete[](void *memory) noexcept {
  std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
  std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
  std::free(memory);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new[](size);
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void *memory, const std::nothrow_t &) noexcept {
  ::operator delete(memory);
}

void operator delete[](void *memory, const std::nothrow_t &) noexcept {
  ::operator delete[](memory);
}

using namespace portals;

namespace {
template <class T, class E> T take(std::expected<T, E> result) {
  REQUIRE(result);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

struct Page final : ModelSnapshot<int> {
  std::vector<ModelRow<int>> values;
  RangeState readiness;
  std::string failureMessage = "provider failed";
  mutable std::size_t reads{};
  mutable std::size_t largestRead{};

  explicit Page(std::uint64_t count = 1'000'000, RangeState state = RangeState::READY)
      : ModelSnapshot(7, count), readiness(state) {
    for (std::uint64_t i = 0; i < std::min<std::uint64_t>(count, 100); ++i) {
      values.push_back({{i + 100, 2}, static_cast<int>(i)});
    }
  }

  ModelRows<int> read(RowRange range) const override {
    ++reads;
    largestRead = std::max(largestRead, range.count);

    if (readiness == RangeState::FAILED) {
      return {readiness, {}, failureMessage};
    }

    if (readiness == RangeState::PENDING) {
      return {};
    }

    return {readiness, std::span(values).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }
};

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));
  std::shared_ptr<Page> page;
  ui::Ref<VirtualTable<int>> table;
  ui::VisibleModelSource<int> source = table.visibleModel();

  explicit Fixture(std::uint64_t count = 1'000'000, RangeState state = RangeState::READY,
                   VirtualTableOptions options = {})
      : page(std::make_shared<Page>(count, state)) {
    CHECK_FALSE(source.snapshot());

    auto model = take(scene->model<int>(page));
    const std::array columns{TableColumn<int>{
      .id = {1, 1}, .header = take(GraphemeText::from("value")), .width = 8, .format = [](const int &) {
        return take(GraphemeText::from("row"));
      }}};

    take(ui::mount(
      *scene, ui::virtualTable<int>(table, model, columns, options, {},
                                    [](const ModelFrame<int> &,
                                       const std::optional<ModelRequest<int>> &work) -> std::expected<void, TreeError> {
                                      if (work) {
                                        work->cancel();
                                      }

                                      return {};
                                    })));
  }

  void layout(CellSize size = {20, 31}, std::optional<CellRect> clip = {}, bool publish = true) {
    take(scene->prepare(*app, size));

    const auto bounds = take(CellRect::from({}, size));
    std::optional<LayoutSnapshot> snapshot;

    take(app->tree().frame([&] {
      take(layoutTree(app->tree(), app->root(), bounds, clip.value_or(bounds)));
      snapshot.emplace(take(LayoutSnapshot::capture(app->tree(), app->root())));
    }));
    take(scene->prepareViews(*app, *snapshot));

    // The runtime arranges the final render frame after view preparation, then publishes its stamp.
    take(app->tree().frame([&] {
      take(layoutTree(app->tree(), app->root(), bounds, clip.value_or(bounds)));
    }));

    if (publish) {
      take(scene->completedLayout(*app));
    }
  }
};

bool state(const VisibleModelError &error, VisibleModelReadState expected) {
  const auto *read = std::get_if<VisibleModelReadError>(&error);

  return read && read->state == expected;
}

bool access(const VisibleModelError &error, TreeError expected) {
  const auto *managed = std::get_if<ManagedError>(&error);

  return managed && std::holds_alternative<TreeError>(managed->cause) &&
         std::get<TreeError>(managed->cause) == expected;
}

static_assert(std::ranges::forward_range<VisibleModelSnapshot<int>>);
static_assert(std::same_as<std::ranges::range_reference_t<VisibleModelSnapshot<int>>, const int &>);
static_assert(!std::ranges::range<ui::VisibleModelSource<int>>);
} // namespace

TEST_CASE("deferred pre-mount source resolves original slot and final preparation only") {
  ui::Ref<VirtualTable<int>> before;
  CHECK(access(before.visibleModel().snapshot().error(), TreeError::CLOSED));

  Fixture fixture;
  CHECK(state(fixture.source.snapshot().error(), VisibleModelReadState::NOT_PREPARED));
  fixture.layout({20, 31}, {}, false);
  CHECK(state(fixture.source.snapshot().error(), VisibleModelReadState::NOT_PREPARED));
  take(fixture.scene->completedLayout(*fixture.app));

  const auto saved = take(fixture.source.snapshot());
  CHECK(saved.size() == 30);
  CHECK(saved.range() == RowRange{0, 30});
  CHECK(saved.rows().front().id == RowId{100, 2});
  CHECK(saved.dataRevision() == 7);
  CHECK(std::accumulate(saved.begin(), saved.end(), 0) == 435);

  const auto reads = fixture.page->reads;

  for (int i = 0; i < 20; ++i) {
    CHECK(take(fixture.source.snapshot()).size() == 30);
  }

  CHECK(fixture.page->reads == reads);
  CHECK(fixture.page->largestRead <= 34);
}

TEST_CASE("resize scroll and options require fresh preparation while retained values remain stable") {
  Fixture fixture;
  fixture.layout();

  auto saved = take(fixture.source.snapshot());
  const auto copy = saved;

  fixture.layout({20, 4});
  CHECK(take(fixture.source.snapshot()).size() == 3);
  take(setVirtualTableFirst(fixture.app->tree(), fixture.table.node(), 10));
  CHECK(state(fixture.source.snapshot().error(), VisibleModelReadState::NOT_PREPARED));
  fixture.layout({20, 4});

  const auto scrolled = take(fixture.source.snapshot());

  CHECK(scrolled.range() == RowRange{10, 3});
  CHECK(*scrolled.begin() == 10);
  CHECK(copy.size() == 30);
  CHECK(*copy.begin() == 0);

  const auto transferred = std::move(saved);

  CHECK(saved.empty());
  CHECK(saved.begin() == saved.end());
  CHECK(transferred.size() == 30);

  VirtualTableOptions options;
  options.row_height = 2;
  take(setVirtualTableOptions(fixture.app->tree(), fixture.table.node(), options));
  CHECK(state(fixture.source.snapshot().error(), VisibleModelReadState::NOT_PREPARED));
  fixture.layout({20, 4});
  CHECK(take(fixture.source.snapshot()).size() == 2);
}

TEST_CASE("final ancestor clip includes partial body rows and excludes header and overscan") {
  VirtualTableOptions options;
  options.row_height = 2;
  options.overscan_rows = 4;
  Fixture fixture(100, RangeState::READY, options);

  fixture.layout({20, 10}, take(CellRect::from({0, 4}, {20, 3})));
  CHECK(take(fixture.source.snapshot()).range() == RowRange{1, 2});
  fixture.layout({20, 10}, take(CellRect::from({0, 0}, {20, 1})));
  CHECK(take(fixture.source.snapshot()).empty());
  fixture.layout({20, 10}, take(CellRect::from({20, 0}, {1, 10})));
  CHECK(take(fixture.source.snapshot()).empty());
  fixture.layout({20, 10}, take(CellRect::from({0, 10}, {20, 1})));
  CHECK(take(fixture.source.snapshot()).empty());
}

TEST_CASE("empty tiny hidden and body clipped below the model are successful empty snapshots") {
  Fixture empty(0);
  empty.layout();
  CHECK(take(empty.source.snapshot()).empty());

  Fixture tiny(1);
  tiny.layout({20, 1});
  CHECK(take(tiny.source.snapshot()).empty());
  tiny.layout({0, 20});
  CHECK(take(tiny.source.snapshot()).empty());
  tiny.layout({20, 10}, take(CellRect::from({0, 8}, {20, 2})));
  CHECK(take(tiny.source.snapshot()).empty());
  take(tiny.app->tree().setVisible(tiny.table.node(), false));
  tiny.layout();
  CHECK(take(tiny.source.snapshot()).empty());
}

TEST_CASE("pending and failed provider data have typed errors with owning diagnostics") {
  Fixture pending(100, RangeState::PENDING);
  pending.layout();
  CHECK(state(pending.source.snapshot().error(), VisibleModelReadState::PENDING));

  Fixture failed(100, RangeState::FAILED);
  failed.layout();

  const auto error = failed.source.snapshot().error();

  CHECK(state(error, VisibleModelReadState::FAILED));
  take(failed.scene->remove(failed.table.node()));
  failed.page.reset();
  CHECK(std::get<VisibleModelReadError>(error).message == "provider failed");
}

TEST_CASE("source rejects foreign UI reads and recycled nodes while saved pins outlive scene") {
  Fixture fixture;
  fixture.layout();

  const auto saved = take(fixture.source.snapshot());
  auto source = fixture.source;
  bool wrongThread{}, copiedRows{};

  std::thread worker([source, saved, &wrongThread, &copiedRows] {
    wrongThread = access(source.snapshot().error(), TreeError::WRONG_THREAD);
    copiedRows = saved.size() == 30 && *saved.begin() == 0;
  });
  worker.join();
  CHECK(wrongThread);
  CHECK(copiedRows);

  const auto original = fixture.table.node().id();

  take(fixture.scene->remove(fixture.table.node()));
  take(fixture.scene->emplace<Widget>(fixture.scene->root()));
  CHECK(access(source.snapshot().error(), TreeError::STALE_NODE));
  CHECK(std::get<ManagedError>(source.snapshot().error()).node == original);
  fixture.scene.reset();
  fixture.page.reset();
  CHECK(access(source.snapshot().error(), TreeError::CLOSED));
  CHECK(saved.rows().front().id == RowId{100, 2});
  CHECK(*saved.begin() == 0);
}

TEST_CASE("failed diagnostic allocation returns NO_MEMORY without throwing") {
  Fixture failed(100, RangeState::FAILED);
  failed.page->failureMessage.assign(256, 'x');
  failed.layout();

  allocationsBeforeFailure = 0;
  auto result = failed.source.snapshot();
  allocationsBeforeFailure = -1;

  REQUIRE_FALSE(result);
  CHECK(access(result.error(), TreeError::NO_MEMORY));
  CHECK(state(failed.source.snapshot().error(), VisibleModelReadState::FAILED));
}
