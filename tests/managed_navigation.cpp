// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "support/examples/replay/terminal.hpp"
#include <portals/managed.hpp>
#include <portals/managed_navigation.hpp>

#include <map>
#include <source_location>
#include <thread>

namespace {
using namespace portals;
constexpr ModelResource RESOURCE{{91, 1}, 1};
constexpr SourceStamp SESSION{92, 1};
constexpr CellSize VIEWPORT{40, 12};

template <class T, class E>
T take(std::expected<T, E> value, std::source_location where = std::source_location::current()) {
  INFO("API call at ", where.file_name(), ":", where.line());
  if (!value) {
    if constexpr (std::same_as<E, ManagedError>) {
      INFO("ManagedPhase=", static_cast<int>(value.error().phase), ", cause index=", value.error().cause.index());
      if (const auto *cause = std::get_if<TreeError>(&value.error().cause)) {
        INFO("TreeError=", static_cast<int>(*cause));
        REQUIRE(value.has_value());
      }
    } else if constexpr (std::is_enum_v<E>) {
      INFO("Typed error=", static_cast<int>(value.error()));
      REQUIRE(value.has_value());
    }
  }
  REQUIRE(value.has_value());

  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

class Rows final : public ModelSnapshot<int> {
public:
  Rows(std::uint64_t revision, std::initializer_list<std::uint64_t> values) : ModelSnapshot(revision, values.size()) {
    for (const auto value : values) {
      index_.emplace(value, rows_.size());
      rows_.push_back({{value, 1}, static_cast<int>(value * revision)});
    }
  }

  ModelRows<int> read(RowRange range) const override {
    return {RangeState::READY, std::span(rows_).subspan(range.first, range.count), {}};
  }

  RowLookup locateRow(RowId id) const override {
    const auto found = index_.find(id.value);

    return found == index_.end() || id.generation != 1 ? RowLookup{RowLookupState::NOT_FOUND, 0}
                                                       : RowLookup{RowLookupState::FOUND, found->second};
  }

private:
  std::vector<ModelRow<int>> rows_;
  std::map<std::uint64_t, std::uint64_t> index_;
};

struct Fixture {
  example::ReplayTerminal terminal{{}, 5000, VIEWPORT};
  std::unique_ptr<Application> app = take(Application::create({}, terminal.clock()));
  std::unique_ptr<ManagedScene> scene;
  Node<Column> content;
  ManagedModel<int> model;

  explicit Fixture(ManagedOptions options = {})
      : scene(take(ManagedScene::create(*app, options))), content(take(scene->emplace<Column>(scene->root()))),
        model(take(scene->model<int>(std::make_shared<Rows>(1, std::initializer_list<std::uint64_t>{1, 2, 3, 4})))) {
  }

  void prepare() {
    take(scene->prepare(*app, VIEWPORT));
    std::optional<LayoutSnapshot> layout;

    take(app->tree().frame([&] {
      const auto bounds = take(CellRect::from({0, 0}, VIEWPORT));

      take(layoutTree(app->tree(), take(app->tree().root()), bounds, bounds));
      layout = take(LayoutSnapshot::capture(app->tree(), scene->root()));
    }));
    take(scene->prepareViews(*app, *layout));
  }

  Node<VirtualList<int>> list(ManagedModel<int> source) {
    return take(scene->virtualList<int>(content, source, [](const ModelRow<int> &row) {
      return take(GraphemeText::from(std::to_string(row.value)));
    }));
  }
};

std::optional<RowId> current(const ManagedNavigation &navigation) {
  const auto value = take(take(navigation.current()).snapshot());

  return value->value.row;
}

std::optional<RowId> selected(Fixture &fixture, Node<VirtualList<int>> node) {
  std::optional<RowId> value;

  take(fixture.app->tree().read(node, [&](const VirtualList<int> &view) {
    value = view.selection();
  }));

  return value;
}
} // namespace

TEST_CASE("navigation is owned by scene and remains usable without views") {
  Fixture fixture;
  auto shared = take(fixture.scene->navigation(fixture.model, RESOURCE, SESSION));
  auto separate = take(fixture.scene->navigation(fixture.model, RESOURCE, SESSION));

  CHECK_FALSE(current(shared));
  take(shared.navigate({3, 1}));
  CHECK(current(shared) == RowId{3, 1});
  CHECK_FALSE(current(separate));

  auto view = fixture.list(fixture.model);
  take(fixture.scene->bindNavigation(view, shared));
  fixture.prepare();
  CHECK(selected(fixture, view) == RowId{3, 1});
  take(fixture.scene->remove(view));
  take(shared.navigate({2, 1}));
  CHECK(current(shared) == RowId{2, 1});

  take(fixture.scene->close());
  CHECK(shared.navigate({1, 1}).error() == TreeError::CLOSED);
}

TEST_CASE("current selection and reading anchor are separate and same current is silent") {
  Fixture fixture;
  auto navigation = take(fixture.scene->navigation(fixture.model, RESOURCE, SESSION));
  auto source = take(navigation.current());
  std::size_t notifications{};

  take(fixture.scene->observe(source, [&](const NavigationPosition &) {
    ++notifications;
  }));
  fixture.prepare();
  take(navigation.navigate({3, 1}));
  fixture.prepare();
  const auto revision = take(source.snapshot())->revision;
  const auto count = notifications;

  take(navigation.navigate({3, 1}));
  fixture.prepare();
  CHECK(take(source.snapshot())->revision == revision);
  CHECK(notifications == count);
  take(navigation.setSelection(std::array<RowId, 1>{{{2, 1}}}));
  auto bookmark = take(ModelBookmark::create(RESOURCE, {PositionKind::ITEM, {4, 1}, 3, 1}));
  take(navigation.setReadingAnchor(bookmark));
  fixture.prepare();
  CHECK(current(navigation) == RowId{3, 1});
  CHECK(take(take(navigation.selection()).snapshot())->value.rows().front() == RowId{2, 1});
  CHECK(take(take(navigation.readingAnchor()).snapshot())->value.row == RowId{4, 1});
  CHECK(notifications == count);
}

TEST_CASE("three views share actual input and projection absence does not delete current") {
  Fixture fixture;
  auto navigation = take(fixture.scene->navigation(fixture.model, RESOURCE, SESSION));
  auto projection =
    take(fixture.scene->model<int>(std::make_shared<Rows>(1, std::initializer_list<std::uint64_t>{4, 2})));
  auto first = fixture.list(fixture.model);
  auto second = fixture.list(fixture.model);
  auto filtered = fixture.list(projection);

  take(fixture.scene->bindNavigation(first, navigation));
  take(fixture.scene->bindNavigation(second, navigation));
  CHECK_FALSE(fixture.scene->bindNavigation(filtered, navigation));
  take(fixture.scene->bindNavigation(filtered, navigation,
                                     NavigationBindingOptions{.projection = NavigationProjection::PRESERVES_ROW_IDS}));
  fixture.prepare();
  take(fixture.app->router().invoke(first, {Action::MOVE_END}));
  fixture.prepare();
  CHECK(current(navigation) == RowId{4, 1});
  CHECK(selected(fixture, second) == RowId{4, 1});
  CHECK(selected(fixture, filtered) == RowId{4, 1});

  take(navigation.navigate({3, 1}));
  fixture.prepare();
  CHECK_FALSE(selected(fixture, filtered));
  CHECK(current(navigation) == RowId{3, 1});
  take(scrollVirtualList(fixture.app->tree(), first, 1));
  fixture.prepare();
  CHECK(current(navigation) == RowId{3, 1});
}

TEST_CASE("deletion uses declared proof and otherwise preserves unresolved bookmark") {
  Fixture fixture;
  auto navigation = take(fixture.scene->navigation(fixture.model, RESOURCE, SESSION));

  take(navigation.navigate({3, 1}));
  const auto changes = take(PositionChangeMap::create(1, 2, 4, std::array{PositionChange::splice(2, 1, 0)}));
  take(navigation.setChangeMap(changes));
  take(
    take(fixture.model.publisher()).publish(std::make_shared<Rows>(2, std::initializer_list<std::uint64_t>{1, 2, 4})));
  take(fixture.app->scheduler().dispatch());
  fixture.prepare();
  CHECK(current(navigation) == RowId{4, 1});
  CHECK(take(take(navigation.current()).snapshot())->value.resolution == PositionResolution::NEAREST);

  take(take(fixture.model.publisher()).publish(std::make_shared<Rows>(3, std::initializer_list<std::uint64_t>{1, 2})));
  take(fixture.app->scheduler().dispatch());
  fixture.prepare();
  CHECK(current(navigation) == RowId{4, 1});
  CHECK(take(take(navigation.current()).snapshot())->value.resolution == PositionResolution::INCOMPATIBLE);
}

TEST_CASE("generic resolution rejects stale reply after newer navigation") {
  Fixture fixture;
  auto navigation = take(fixture.scene->navigation(RESOURCE, SESSION));
  auto bookmark = take(ModelBookmark::create(RESOURCE, {PositionKind::ITEM, {2, 1}, 1, 1}));

  take(navigation.setCurrent(bookmark));
  auto request = take(navigation.beginResolve(2));
  take(navigation.clearCurrent());
  PositionReply reply{
    RESOURCE, SESSION, 2, request.sequence, PositionResolution::EXACT, {PositionKind::ITEM, {2, 1}, 1, 2}};
  CHECK_FALSE(take(navigation.applyReply(reply)));
  CHECK_FALSE(current(navigation));
}

TEST_CASE("wrong thread foreign scene and callback removal are checked") {
  Fixture fixture;
  Fixture foreign;
  auto navigation = take(fixture.scene->navigation(fixture.model, RESOURCE, SESSION));
  auto view = fixture.list(fixture.model);
  auto other = foreign.list(foreign.model);

  CHECK_FALSE(foreign.scene->bindNavigation(other, navigation));
  std::optional<TreeError> failure;
  std::thread worker([&] {
    auto result = navigation.navigate({1, 1});
    failure = result.error();
  });
  worker.join();
  CHECK(failure == TreeError::WRONG_THREAD);
  take(fixture.scene->bindNavigation(view, navigation));
  take(fixture.scene->observe(take(navigation.current()), [&](const NavigationPosition &value) {
    if (value.row) {
      take(fixture.scene->remove(view));
    }
  }));
  take(navigation.navigate({2, 1}));
  fixture.prepare();
  CHECK(current(navigation) == RowId{2, 1});
}

TEST_CASE("same record revision refreshes bookmark without navigation delivery") {
  Fixture fixture;
  auto navigation = take(fixture.scene->navigation(fixture.model));
  auto independent = take(fixture.scene->navigation(fixture.model));
  auto channel = take(navigation.current());

  take(navigation.navigate({3, 1}));
  take(independent.navigate({1, 1}));
  CHECK(take(navigation.bookmark())->resource() == take(independent.bookmark())->resource());
  fixture.prepare();
  const auto published = take(channel.snapshot())->revision;

  take(take(fixture.model.publisher())
         .publish(std::make_shared<Rows>(2, std::initializer_list<std::uint64_t>{4, 3, 2, 1})));
  take(fixture.app->scheduler().dispatch());
  fixture.prepare();
  CHECK(take(channel.snapshot())->revision == published);
  CHECK(take(navigation.bookmark())->position().revision == 2);
  CHECK(take(navigation.bookmark())->position().offset == 1);
  const auto proof = take(PositionChangeMap::create(2, 3, 4, std::array{PositionChange::splice(1, 1, 0)}));

  take(navigation.setChangeMap(proof));
  take(
    take(fixture.model.publisher()).publish(std::make_shared<Rows>(3, std::initializer_list<std::uint64_t>{4, 2, 1})));
  take(fixture.app->scheduler().dispatch());
  fixture.prepare();
  CHECK(current(navigation) == RowId{2, 1});
  CHECK(current(independent) == RowId{1, 1});
}

TEST_CASE("reading synchronization maps stable anchors across different projection orders") {
  Fixture fixture;
  auto navigation = take(fixture.scene->navigation(fixture.model));
  auto projection =
    take(fixture.scene->model<int>(std::make_shared<Rows>(1, std::initializer_list<std::uint64_t>{4, 3, 2, 1})));
  auto first = fixture.list(fixture.model);
  auto second = fixture.list(projection);
  auto third = fixture.list(fixture.model);
  const SizePolicy oneRow{.width = {.preferred = 0, .mode = SizeMode::FLEX},
                          .height = {.minimum = 1, .preferred = 1, .maximum = 1, .mode = SizeMode::FIXED}};

  take(setSizePolicy(fixture.app->tree(), first, oneRow));
  take(setSizePolicy(fixture.app->tree(), second, oneRow));
  take(setSizePolicy(fixture.app->tree(), third, oneRow));
  take(fixture.scene->bindNavigation(first, navigation, {.readingAnchor = true}));
  take(fixture.scene->bindNavigation(second, navigation,
                                     {.projection = NavigationProjection::PRESERVES_ROW_IDS, .readingAnchor = true}));
  take(fixture.scene->bindNavigation(third, navigation, {.readingAnchor = true}));
  take(navigation.navigate({1, 1}));
  fixture.prepare();
  take(scrollVirtualList(fixture.app->tree(), first, 1));
  fixture.prepare();
  CHECK(current(navigation) == RowId{1, 1});
  CHECK(take(take(navigation.readingAnchor()).snapshot())->value.row == RowId{2, 1});
  std::optional<RowId> secondAnchor;
  std::uint64_t secondOffset{};
  std::optional<RowId> thirdAnchor;

  take(fixture.app->tree().read(second, [&](const VirtualList<int> &view) {
    secondAnchor = view.anchorRow();
    secondOffset = view.scrollState().rasterOffset();
  }));
  take(fixture.app->tree().read(third, [&](const VirtualList<int> &view) {
    thirdAnchor = view.anchorRow();
  }));
  CHECK(secondAnchor == RowId{2, 1});
  CHECK(secondOffset == 2);
  CHECK(thirdAnchor == RowId{2, 1});
}

TEST_CASE("table input publishes stable current and remains independent from focus") {
  Fixture fixture;
  auto navigation = take(fixture.scene->navigation(fixture.model));
  auto list = fixture.list(fixture.model);
  const TableColumn<int> columns[]{{{1, 1}, take(GraphemeText::from("Value")), 8, [](const int &value) {
                                      return take(GraphemeText::from(std::to_string(value)));
                                    }}};
  auto table = take(fixture.scene->virtualTable<int>(fixture.content, fixture.model, columns));

  take(fixture.scene->bindNavigation(table, navigation));
  take(fixture.scene->bindNavigation(list, navigation));
  fixture.prepare();
  take(fixture.app->router().invoke(table, {Action::MOVE_END}));
  fixture.prepare();
  fixture.prepare();
  CHECK(current(navigation) == RowId{4, 1});
  CHECK(selected(fixture, list) == RowId{4, 1});
}

TEST_CASE("resolution cancellation closes pending work and is safe under callback reentry") {
  Fixture fixture;
  auto navigation = take(fixture.scene->navigation(RESOURCE, SESSION));
  auto first = take(ModelBookmark::create(RESOURCE, {PositionKind::ITEM, {1, 1}, 0, 1}));
  auto second = take(ModelBookmark::create(RESOURCE, {PositionKind::ITEM, {2, 1}, 1, 1}));

  take(navigation.setCurrent(first));
  const auto old = take(navigation.beginResolve(2));
  auto token = take(navigation.stopToken());
  std::stop_callback reenter(token, [&] {
    take(navigation.setCurrent(second));
  });

  take(navigation.clearCurrent());
  CHECK(token.stop_requested());
  CHECK(current(navigation) == RowId{2, 1});
  CHECK_FALSE(take(navigation.applyReply(
    {RESOURCE, SESSION, 2, old.sequence, PositionResolution::EXACT, {PositionKind::ITEM, {1, 1}, 0, 2}})));
  take(navigation.beginResolve(3));
  auto pending = take(navigation.stopToken());

  take(fixture.scene->close());
  CHECK(pending.stop_requested());
  CHECK(navigation.current().error() == TreeError::CLOSED);
}

TEST_CASE("pending view input cannot roll back a newer shared current") {
  class PendingRows final : public ModelSnapshot<int> {
  public:
    PendingRows() : ModelSnapshot(1, 4) {
    }

    ModelRows<int> read(RowRange) const override {
      return {RangeState::PENDING, {}, {}};
    }

    RowLookup locateRow(RowId id) const override {
      return id.value >= 1 && id.value <= 4 && id.generation == 1 ? RowLookup{RowLookupState::FOUND, id.value - 1}
                                                                  : RowLookup{RowLookupState::NOT_FOUND, 0};
    }
  };

  Fixture fixture;
  auto model = take(fixture.scene->model<int>(std::make_shared<PendingRows>()));
  auto navigation = take(fixture.scene->navigation(model));
  std::optional<ModelRequest<int>> work;
  auto list = take(fixture.scene->virtualList<int>(
    fixture.content, model,
    [](const ModelRow<int> &row) {
      return take(GraphemeText::from(std::to_string(row.value)));
    },
    {}, {},
    [&](const ModelFrame<int> &, const std::optional<ModelRequest<int>> &request) -> std::expected<void, TreeError> {
      if (request) {
        work = *request;
      }

      return {};
    }));

  take(fixture.scene->bindNavigation(list, navigation));
  fixture.prepare();
  REQUIRE(work.has_value());
  take(fixture.app->router().invoke(list, {Action::MOVE_END}));
  fixture.prepare();
  CHECK_FALSE(current(navigation));
  take(navigation.navigate({2, 1}));
  fixture.prepare();
  REQUIRE(work.has_value());
  take(work->complete(std::make_shared<Rows>(1, std::initializer_list<std::uint64_t>{1, 2, 3, 4})));
  take(fixture.app->scheduler().dispatch());
  fixture.prepare();
  CHECK(current(navigation) == RowId{2, 1});
  CHECK(selected(fixture, list) == RowId{2, 1});
}

TEST_CASE("hiding a view cancels positional input without publishing its old selection") {
  Fixture fixture;
  auto navigation = take(fixture.scene->navigation(fixture.model));
  auto list = fixture.list(fixture.model);

  take(fixture.scene->bindNavigation(list, navigation));
  fixture.prepare();
  take(setVirtualListSelection(fixture.app->tree(), list, RowId{1, 1}));
  take(fixture.app->router().invoke(list, {Action::MOVE_END}));
  take(fixture.app->tree().setVisible(list, false));
  fixture.prepare();
  CHECK_FALSE(current(navigation));
  take(fixture.app->tree().setVisible(list, true));
  fixture.prepare();
  CHECK_FALSE(current(navigation));
}

TEST_CASE("rejected navigation factory preserves remaining scene state capacity") {
  Fixture fixture(ManagedOptions{.maxStates = 2});
  auto first = take(fixture.scene->state(7));
  auto rejected = fixture.scene->navigation(fixture.model);

  REQUIRE_FALSE(rejected);
  CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::NODE_LIMIT);
  auto second = take(fixture.scene->state(8));

  CHECK(take(first.snapshot())->value == 7);
  CHECK(take(second.snapshot())->value == 8);
  CHECK_FALSE(fixture.scene->state(9));
}

TEST_CASE("observer close is checked during delivery and succeeds after callback") {
  Fixture fixture;
  auto navigation = take(fixture.scene->navigation(fixture.model));
  auto saved = take(navigation.current());
  std::optional<TreeError> closeError;

  take(fixture.scene->observe(saved, [&](const NavigationPosition &value) {
    if (value.row) {
      const auto attempted = fixture.scene->close();

      if (!attempted) {
        closeError = std::get<TreeError>(attempted.error().cause);
      }
    }
  }));
  fixture.prepare();
  take(navigation.navigate({2, 1}));
  take(fixture.scene->prepare(*fixture.app, VIEWPORT));
  CHECK(closeError == TreeError::WOULD_BLOCK);
  CHECK(current(navigation) == RowId{2, 1});
  take(fixture.scene->close());
  CHECK(navigation.current().error() == TreeError::CLOSED);
  CHECK(take(saved.snapshot())->value.row == RowId{2, 1});
}

TEST_CASE("failed reconciliation retries the same model revision without current notification") {
  class RetryRows final : public ModelSnapshot<int> {
  public:
    RetryRows()
        : ModelSnapshot(2, 4), rows_(std::make_shared<Rows>(2, std::initializer_list<std::uint64_t>{4, 3, 2, 1})) {
    }

    ModelRows<int> read(RowRange range) const override {
      return rows_->read(range);
    }

    RowLookup locateRow(RowId id) const override {
      if (++attempts_ == 1) {
        throw std::bad_alloc();
      }

      return rows_->locateRow(id);
    }

    std::size_t attempts() const noexcept {
      return attempts_;
    }

  private:
    std::shared_ptr<const ModelSnapshot<int>> rows_;
    mutable std::size_t attempts_{};
  };

  Fixture fixture;
  auto navigation = take(fixture.scene->navigation(fixture.model));
  auto channel = take(navigation.current());

  take(navigation.navigate({3, 1}));
  fixture.prepare();
  const auto published = take(channel.snapshot())->revision;
  auto rows = std::make_shared<RetryRows>();

  take(take(fixture.model.publisher()).publish(rows));
  take(fixture.app->scheduler().dispatch());
  const auto failed = fixture.scene->prepare(*fixture.app, VIEWPORT);

  REQUIRE_FALSE(failed);
  CHECK(failed.error() == TreeError::NO_MEMORY);
  CHECK(take(navigation.bookmark())->position().revision == 1);
  take(fixture.scene->prepare(*fixture.app, VIEWPORT));
  CHECK(rows->attempts() == 2);
  CHECK(take(navigation.bookmark())->position().revision == 2);
  CHECK(take(navigation.bookmark())->position().offset == 1);
  CHECK(take(channel.snapshot())->revision == published);
}

TEST_CASE("reading reconciliation retry preserves an already committed nearest current") {
  class RetryReadingRows final : public ModelSnapshot<int> {
  public:
    RetryReadingRows()
        : ModelSnapshot(2, 3), rows_(std::make_shared<Rows>(2, std::initializer_list<std::uint64_t>{1, 2, 4})) {
    }

    ModelRows<int> read(RowRange range) const override {
      return rows_->read(range);
    }

    RowLookup locateRow(RowId id) const override {
      if (id == RowId{4, 1} && ++readingAttempts_ == 1) {
        throw std::bad_alloc();
      }

      return rows_->locateRow(id);
    }

    std::size_t readingAttempts() const noexcept {
      return readingAttempts_;
    }

  private:
    std::shared_ptr<const ModelSnapshot<int>> rows_;
    mutable std::size_t readingAttempts_{};
  };

  Fixture fixture;
  auto navigation = take(fixture.scene->navigation(fixture.model));
  auto channel = take(navigation.current());
  std::size_t nearestNotifications{};

  take(fixture.scene->observe(channel, [&](const NavigationPosition &value) {
    if (value.resolution == PositionResolution::NEAREST) {
      ++nearestNotifications;
    }
  }));
  take(navigation.navigate({3, 1}));
  const auto resource = take(navigation.bookmark())->resource();
  auto reading = take(ModelBookmark::create(resource, {PositionKind::ITEM, {4, 1}, 3, 1}));

  take(navigation.setReadingAnchor(reading));
  fixture.prepare();
  const auto changes = take(PositionChangeMap::create(1, 2, 4, std::array{PositionChange::splice(2, 1, 0)}));
  auto rows = std::make_shared<RetryReadingRows>();

  take(navigation.setChangeMap(changes));
  take(take(fixture.model.publisher()).publish(rows));
  take(fixture.app->scheduler().dispatch());
  const auto failed = fixture.scene->prepare(*fixture.app, VIEWPORT);

  REQUIRE_FALSE(failed);
  CHECK(failed.error() == TreeError::NO_MEMORY);
  CHECK(current(navigation) == RowId{4, 1});
  const auto committed = take(channel.snapshot());

  CHECK(committed->value.resolution == PositionResolution::NEAREST);
  CHECK(take(navigation.bookmark())->position().revision == 2);
  CHECK(take(navigation.readingBookmark())->position().revision == 1);
  take(fixture.scene->prepare(*fixture.app, VIEWPORT));
  CHECK(rows->readingAttempts() == 2);
  CHECK(take(navigation.readingBookmark())->position().revision == 2);
  CHECK(take(channel.snapshot())->revision == committed->revision);
  CHECK(take(channel.snapshot())->value.resolution == PositionResolution::NEAREST);
  CHECK(nearestNotifications == 1);
  fixture.prepare();
  CHECK(take(channel.snapshot())->revision == committed->revision);
  CHECK(nearestNotifications == 1);
}

TEST_CASE("exact metadata reconciliation does not require a ready row payload") {
  class PendingMetadataRows final : public ModelSnapshot<int> {
  public:
    PendingMetadataRows() : ModelSnapshot(2, 4) {
    }

    ModelRows<int> read(RowRange) const override {
      ++payloadReads_;

      return {RangeState::PENDING, {}, {}};
    }

    RowLookup locateRow(RowId id) const override {
      return id.value >= 1 && id.value <= 4 && id.generation == 1 ? RowLookup{RowLookupState::FOUND, 4 - id.value}
                                                                  : RowLookup{RowLookupState::NOT_FOUND, 0};
    }

    std::size_t payloadReads() const noexcept {
      return payloadReads_;
    }

  private:
    mutable std::size_t payloadReads_{};
  };

  Fixture fixture;
  auto navigation = take(fixture.scene->navigation(fixture.model));
  auto channel = take(navigation.current());

  take(navigation.navigate({3, 1}));
  fixture.prepare();
  const auto published = take(channel.snapshot())->revision;
  auto rows = std::make_shared<PendingMetadataRows>();

  take(take(fixture.model.publisher()).publish(rows));
  take(fixture.app->scheduler().dispatch());
  take(fixture.scene->prepare(*fixture.app, VIEWPORT));
  CHECK(take(navigation.bookmark())->position().revision == 2);
  CHECK(take(navigation.bookmark())->position().offset == 1);
  CHECK(take(channel.snapshot())->revision == published);
  CHECK(take(channel.snapshot())->value.resolution == PositionResolution::EXACT);
  CHECK(rows->payloadReads() == 0);
}
