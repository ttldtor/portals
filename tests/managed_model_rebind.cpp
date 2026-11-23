// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/ui.hpp>

#include <array>
#include <map>
#include <thread>

namespace {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> result) {
  REQUIRE(result);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

class Rows final : public ModelSnapshot<int> {
public:
  explicit Rows(int first, RangeState state = RangeState::READY) : ModelSnapshot(1, 4), state_(state) {
    for (std::size_t index = 0; index < rows_.size(); ++index) {
      rows_[index] = {{index + 1, 1}, first + static_cast<int>(index)};
    }
  }

  ModelRows<int> read(RowRange range) const override {
    if (state_ == RangeState::PENDING) {
      return {};
    }

    if (state_ == RangeState::FAILED) {
      return {RangeState::FAILED, {}, "new model failed"};
    }

    return {RangeState::READY, std::span(rows_).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }

private:
  std::array<ModelRow<int>, 4> rows_{};
  RangeState state_;
};

class IndexedRows final : public ModelSnapshot<int> {
public:
  explicit IndexedRows(int base) : ModelSnapshot(1, 128), rows_(0) {
    for (std::uint64_t index = 0; index < rowCount(); ++index) {
      rows_.push_back({{index + 1, 1}, base + static_cast<int>(index)});
    }
  }

  ModelRows<int> read(RowRange range) const override {
    return {RangeState::READY, std::span(rows_).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }

  RowLookup locateRow(RowId id) const override {
    return id.generation == 1 && id.value && id.value <= rowCount() ? RowLookup{RowLookupState::FOUND, id.value - 1}
                                                                    : RowLookup{RowLookupState::NOT_FOUND, 0};
  }

private:
  std::vector<ModelRow<int>> rows_;
};

class SwitchRows final : public ModelSnapshot<int> {
public:
  SwitchRows(std::uint64_t revision, int base, bool pending = false, bool indexed = true)
      : ModelSnapshot(revision, 128), rows_(128), pending_(pending), indexed_(indexed) {
    for (std::size_t index = 0; index < rows_.size(); ++index) {
      rows_[index] = {{index + 1, 1}, base + static_cast<int>(index)};
    }
  }

  ModelRows<int> read(RowRange range) const override {
    ++reads;
    return pending_ ? ModelRows<int>{}
                    : ModelRows<int>{RangeState::READY,
                                     std::span(rows_).subspan(static_cast<std::size_t>(range.first), range.count),
                                     {}};
  }

  RowLookup locateRow(RowId id) const override {
    if (id.value == 2) {
      ++lookupTwo;
    }
    if (onLocate) {
      auto action = std::move(onLocate);
      action();
    }
    if (!indexed_ || id.value == unsupportedId) {
      return {};
    }
    return id.generation == 1 && id.value && id.value <= rowCount() ? RowLookup{RowLookupState::FOUND, id.value - 1}
                                                                    : RowLookup{RowLookupState::NOT_FOUND, 0};
  }

  mutable std::function<void()> onLocate;
  mutable std::size_t reads{};
  mutable std::size_t lookupTwo{};
  std::uint64_t unsupportedId{};

private:
  std::vector<ModelRow<int>> rows_;
  bool pending_;
  bool indexed_;
};

class OrderedRows final : public ModelSnapshot<int> {
public:
  OrderedRows(std::uint64_t revision, std::initializer_list<std::uint64_t> ids)
      : ModelSnapshot(revision, ids.size()), rows_(0) {
    for (auto id : ids) {
      index_.emplace(id, rows_.size());
      rows_.push_back({{id, 1}, static_cast<int>(id * 10)});
    }
  }

  ModelRows<int> read(RowRange range) const override {
    return {RangeState::READY, std::span(rows_).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }

  RowLookup locateRow(RowId id) const override {
    const auto entry = index_.find(id.value);
    return id.generation == 1 && entry != index_.end() ? RowLookup{RowLookupState::FOUND, entry->second}
                                                       : RowLookup{RowLookupState::NOT_FOUND, 0};
  }

private:
  std::vector<ModelRow<int>> rows_;
  std::map<std::uint64_t, std::uint64_t> index_;
};

GraphemeText rowText(int value) {
  return take(GraphemeText::from(std::to_string(value)));
}

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));
  ManagedModel<int> a;
  ManagedModel<int> b;
  ui::Ref<VirtualList<int>> list;
  ui::Ref<VirtualTable<int>> table;
  ui::Ref<DataStateView> state;
  Node<Widget> body;
  std::optional<int> firstListValue;
  std::array<std::optional<ModelRequest<int>>, 2> requests;
  std::array<std::size_t, 2> callbacks{};

  explicit Fixture(RangeState initial = RangeState::READY, bool automaticState = false) {
    a = take(scene->model<int>(std::make_shared<Rows>(10, initial)));
    b = take(scene->model<int>(std::make_shared<Rows>(100)));

    const std::array columns{TableColumn<int>{.id = {1, 1}, .header = rowText(0), .width = 8, .format = rowText}};
    auto listRecipe = ui::virtualList<int>(
      list, a,
      [this](const ModelRow<int> &row) {
        if (!firstListValue) {
          firstListValue = row.value;
        }

        return rowText(row.value);
      },
      {}, DEFAULT_VIEWPORT_POLICY, provider(0));
    auto tableRecipe = ui::virtualTable<int>(table, a, columns, {}, DEFAULT_VIEWPORT_POLICY, provider(1));

    if (automaticState) {
      body = take(ui::mount(
        *scene, ui::row(ui::dataStateView(std::move(listRecipe)).reference(state), std::move(tableRecipe)).height(8)));
    } else {
      body = take(ui::mount(*scene, ui::row(std::move(listRecipe), std::move(tableRecipe)).height(8)));
    }
  }

  ManagedViewHandler<int> provider(std::size_t index) {
    return [this, index](const ModelFrame<int> &,
                         const std::optional<ModelRequest<int>> &request) -> std::expected<void, TreeError> {
      ++callbacks[index];
      if (request) {
        requests[index] = request;
      }

      return {};
    };
  }

  void prepare() {
    firstListValue.reset();
    take(app->scheduler().dispatch());
    take(scene->afterDispatch(*app));
    take(scene->prepare(*app, {40, 8}));

    const auto bounds = take(CellRect::from({}, {40, 8}));
    std::optional<LayoutSnapshot> layout;

    take(app->tree().frame([&] {
      take(layoutTree(app->tree(), app->root(), bounds, bounds));
      layout.emplace(take(LayoutSnapshot::capture(app->tree(), app->root())));
    }));
    take(scene->prepareViews(*app, *layout));
    take(app->tree().frame([&] {
      take(layoutTree(app->tree(), app->root(), bounds, bounds));
    }));
    take(scene->completedLayout(*app));
  }

  int tableValue() {
    const auto geometry = take(app->tree().inspect(table.node())).geometry.surface.size();
    DataRangeStatus status;
    RowRange range;
    take(app->tree().read(table.node(), [&](const auto &view) {
      status = view.dataStatus();
      range = view.visibleRange();
    }));
    INFO("table surface=", geometry.width, "x", geometry.height, " prepared=", status.prepared,
         " rows=", status.rowCount, " visible=", range.first, ":", range.count);
    const auto visible = take(table.visibleModel().snapshot());

    REQUIRE_FALSE(visible.empty());

    return *visible.begin();
  }
};

TEST_CASE("rebind keeps node identity and replaces both views at equal data revisions and row ids") {
  Fixture f;
  f.prepare();
  const auto listNode = f.list.node();
  const auto tableNode = f.table.node();
  const auto saved = take(f.table.visibleModel().snapshot());
  REQUIRE_FALSE(saved.empty());

  CHECK(f.firstListValue == 10);
  CHECK(f.tableValue() == 10);
  take(f.list.setModel(f.b));
  take(f.table.setModel(f.b));
  CHECK_FALSE(f.table.visibleModel().snapshot());
  f.prepare();

  CHECK(f.list.node() == listNode);
  CHECK(f.table.node() == tableNode);
  CHECK(f.firstListValue == 100);
  CHECK(f.tableValue() == 100);
  CHECK(*saved.begin() == 10);
  take(f.list.setModel(f.a));
  take(f.table.setModel(f.a));
  f.prepare();
  CHECK(f.firstListValue == 10);
  CHECK(f.tableValue() == 10);
}

TEST_CASE("invalid models and foreign scene models preserve the prepared binding") {
  Fixture f;
  f.prepare();

  CHECK_FALSE(f.list.setModel(ManagedModel<int>{}));
  CHECK_FALSE(f.table.setModel(ManagedModel<int>{}));
  auto otherApp = take(Application::create());
  auto other = take(ManagedScene::create(*otherApp));
  auto foreign = take(other->model<int>(std::make_shared<Rows>(900)));

  CHECK_FALSE(f.list.setModel(foreign));
  CHECK_FALSE(f.table.setModel(foreign));
  CHECK(f.tableValue() == 10);
  f.prepare();
  CHECK_FALSE(f.firstListValue);
  CHECK(f.tableValue() == 10);
}

TEST_CASE("default model replacement restores independent list and table reading and selection") {
  Fixture f;
  const auto a = take(f.scene->model<int>(std::make_shared<IndexedRows>(1000)));
  const auto b = take(f.scene->model<int>(std::make_shared<IndexedRows>(2000)));

  take(f.list.setModel(a));
  take(f.table.setModel(a));
  f.prepare();
  take(setVirtualListFirst(f.app->tree(), f.list.node(), 30));
  take(setVirtualTableFirst(f.app->tree(), f.table.node(), 40));
  take(setVirtualListSelection(f.app->tree(), f.list.node(), RowId{32, 1}));
  take(setVirtualTableSelection(f.app->tree(), f.table.node(), RowId{42, 1}));
  f.prepare();
  take(f.list.setModel(b));
  take(f.table.setModel(b));
  f.prepare();
  take(f.list.setModel(a));
  take(f.table.setModel(a));
  f.prepare();

  CHECK(f.firstListValue == 1030);
  CHECK(f.tableValue() == 1040);
  take(f.app->tree().read(f.list.node(), [](const VirtualList<int> &view) {
    CHECK(view.selection() == RowId{32, 1});
  }));
  take(f.app->tree().read(f.table.node(), [](const VirtualTable<int> &view) {
    CHECK(view.selection() == RowId{42, 1});
  }));
}

TEST_CASE("pending old receipts cannot publish into the rebound consumer") {
  Fixture f(RangeState::PENDING);
  f.prepare();
  REQUIRE(f.requests[0]);
  REQUIRE(f.requests[1]);
  const auto old = f.requests;

  take(f.list.setModel(f.b));
  take(f.table.setModel(f.b));

  for (const auto &request : old) {
    CHECK(request->stopRequested());
    const auto late = request->complete(std::make_shared<Rows>(900));

    REQUIRE_FALSE(late);
    CHECK(late.error() == SchedulerError::CLOSED);
  }

  f.prepare();
  CHECK(f.firstListValue == 100);
  CHECK(f.tableValue() == 100);
}

TEST_CASE("a completion queued before rebinding cannot replace the new model data") {
  Fixture f(RangeState::PENDING);
  f.prepare();
  REQUIRE(f.requests[0]);
  take(f.requests[0]->complete(std::make_shared<Rows>(900)));
  take(f.list.setModel(f.b));
  take(f.table.setModel(f.b));
  f.prepare();

  CHECK(f.firstListValue == 100);
  CHECK(f.tableValue() == 100);
}

TEST_CASE("handler-only replacement preserves the same model consumer and pending receipt") {
  Fixture f(RangeState::PENDING);
  f.prepare();
  REQUIRE(f.requests[0]);
  const auto pending = f.requests[0];
  std::size_t replacements{};
  ManagedViewHandler<int> replacement = [&](const auto &, const auto &work) -> std::expected<void, TreeError> {
    ++replacements;
    CHECK_FALSE(work.has_value());

    return {};
  };

  take(f.list.setModel(f.a, replacement));
  CHECK_FALSE(pending->stopRequested());
  f.prepare();
  CHECK(replacements == 1);
  CHECK_FALSE(pending->stopRequested());
}

TEST_CASE("cross-model callbacks use model ownership unless retention is explicit") {
  Fixture f;
  f.prepare();
  auto before = f.callbacks;

  take(f.list.setModel(f.b));
  take(f.table.setModel(f.b));
  f.prepare();
  CHECK(f.callbacks == before);
  std::size_t replacements{};
  ManagedViewHandler<int> replacement = [&](const auto &, const auto &) -> std::expected<void, TreeError> {
    ++replacements;

    return {};
  };

  before = f.callbacks;
  take(f.list.setModel(f.a, replacement));
  take(f.table.setModel(f.a, replacement));
  f.prepare();
  CHECK(replacements == 2);
  CHECK(f.callbacks == before);
  take(f.list.setModel(f.b, ManagedViewHandler<int>{}));
  take(f.table.setModel(f.b, ManagedViewHandler<int>{}));
  f.prepare();
  CHECK(replacements == 2);
  CHECK(f.firstListValue == 100);
  CHECK(f.tableValue() == 100);
  take(f.list.setModel(f.a, replacement));
  f.prepare();
  const auto retained = replacements;
  take(f.list.setModel(f.b, ModelRebindOptions{.retainPreparedOverride = true}));
  f.prepare();
  CHECK(replacements > retained);
}

TEST_CASE("explicit reset returns one independent view to the resource initial policy") {
  Fixture f;
  auto a = take(f.scene->model<int>(std::make_shared<IndexedRows>(1000)));
  CHECK_FALSE(take(f.list.restoreStatus()));
  take(f.list.setModel(a));
  f.prepare();
  take(setVirtualListFirst(f.app->tree(), f.list.node(), 30));
  take(setVirtualListSelection(f.app->tree(), f.list.node(), RowId{32, 1}));
  f.prepare();
  take(f.list.setModel(f.b));
  f.prepare();
  take(f.list.setModel(a, ModelRebindOptions{.restore = ModelRestorePolicy::RESET}));
  CHECK_FALSE(take(f.list.restoreStatus()));
  f.prepare();
  CHECK(f.firstListValue == 1000);
  take(f.app->tree().read(f.list.node(), [](const auto &view) {
    CHECK_FALSE(view.selection());
    CHECK(view.scrollState().rasterOffset() == 0);
  }));
}

TEST_CASE("history admission capacity fails before replacing the current model") {
  Fixture f;
  f.prepare();
  constexpr std::size_t EMPTY_LEDGER_BYTES = 4096;
  REQUIRE(SourceBudget::create({.maxBytes = EMPTY_LEDGER_BYTES}));
  const auto result =
    f.list.setModel(f.b, ModelRebindOptions{.checkpoints = ModelCheckpointOptions{.maxBytes = EMPTY_LEDGER_BYTES}});
  REQUIRE_FALSE(result);
  CHECK(std::get<TreeError>(result.error().cause) == TreeError::WOULD_BLOCK);
  take(f.app->tree().read(f.list.node(), [](const auto &view) {
    CHECK(view.dataStatus().prepared);
  }));
  CHECK(f.tableValue() == 10);
}

TEST_CASE("shared same-model keep replace and clear preserve a capacity-one pending consumer") {
  Fixture f;
  auto limited =
    take(f.scene->model<int>(std::make_shared<Rows>(300, RangeState::PENDING), {.max_consumers = 1}, f.provider(0)));
  take(f.list.setModel(limited));
  auto shared = take(f.scene->navigation(limited));
  take(f.scene->bindNavigation(f.list.node(), shared));
  f.prepare();
  REQUIRE(f.requests[0]);
  const auto retained = *f.requests[0];
  take(shared.setModel(limited));
  std::size_t changed{};
  ManagedViewHandler<int> replacement = [&](const auto &, const auto &work) -> std::expected<void, TreeError> {
    ++changed;
    CHECK_FALSE(work);
    return {};
  };
  take(f.list.setModel(limited, replacement));
  f.prepare();
  CHECK(changed == 1);
  CHECK_FALSE(retained.stopRequested());
  const auto modelCalls = f.callbacks[0];
  take(f.list.setModel(limited, ManagedViewHandler<int>{}));
  f.prepare();
  CHECK(changed == 1);
  CHECK(f.callbacks[0] > modelCalls);
  CHECK_FALSE(retained.stopRequested());
}

TEST_CASE("terminal restoration is view-local and retries only newer publication evidence") {
  Fixture f(RangeState::READY, true);
  auto a = take(f.scene->model<int>(std::make_shared<SwitchRows>(1, 1000)));
  take(f.list.setModel(a));
  f.prepare();
  take(setVirtualListFirst(f.app->tree(), f.list.node(), 30));
  take(setVirtualListSelection(f.app->tree(), f.list.node(), RowId{32, 1}));
  f.prepare();
  take(f.list.setModel(f.b));
  auto pending = std::make_shared<SwitchRows>(2, 1000, false, false);
  take(take(a.publisher()).publish(pending));
  f.prepare();
  take(f.list.setModel(a));
  f.prepare();
  CHECK(take(f.list.restoreStatus()) == PositionResolution::UNSUPPORTED);
  take(f.app->tree().read(f.state.node(), [](const auto &view) {
    CHECK(view.presentation().phase == DataPhase::FAILED);
  }));
  CHECK_FALSE(f.firstListValue);
  const auto reads = pending->reads;
  f.prepare();
  CHECK_FALSE(f.firstListValue);
  CHECK(pending->reads == reads);
  // A fresh independent view uses this healthy provider normally. Failure belongs
  // to the saved intent, never to the model shared by other controls.
  take(f.table.setModel(a));
  f.prepare();
  CHECK(f.tableValue() == 1000);
  CHECK_FALSE(take(f.table.restoreStatus()));
  CHECK(take(f.list.restoreStatus()) == PositionResolution::UNSUPPORTED);
  take(take(a.publisher()).publish(std::make_shared<SwitchRows>(3, 1000)));
  f.prepare();
  CHECK(f.firstListValue == 1030);
  CHECK(take(f.list.restoreStatus()) == PositionResolution::EXACT);
  take(f.app->tree().read(f.list.node(), [](const auto &view) {
    CHECK(view.selection() == RowId{32, 1});
  }));
}

TEST_CASE("indexed pending rows request the restored bounded interval and never row zero") {
  Fixture f;
  auto a = take(f.scene->model<int>(std::make_shared<SwitchRows>(1, 1000), {}, f.provider(0)));
  take(f.list.setModel(a));
  f.prepare();
  take(setVirtualListFirst(f.app->tree(), f.list.node(), 30));
  f.prepare();
  take(f.list.setModel(f.b));
  take(take(a.publisher()).publish(std::make_shared<SwitchRows>(2, 1000, true)));
  f.prepare();
  take(f.list.setModel(a));
  f.prepare();
  CHECK_FALSE(f.firstListValue);
  REQUIRE(f.requests[0]);
  CHECK(f.requests[0]->range().first > 0);
  take(f.app->tree().read(f.list.node(), [](const auto &view) {
    CHECK(view.visibleRange().first == 30);
    CHECK(view.rangeState() == RangeState::PENDING);
  }));
  take(f.requests[0]->complete(std::make_shared<SwitchRows>(2, 1000)));
  f.prepare();
  CHECK(f.firstListValue == 1030);
}

TEST_CASE("same-model explicit reset clears terminal restoration using its capacity-one consumer") {
  Fixture f;
  auto a = take(f.scene->model<int>(std::make_shared<SwitchRows>(1, 1000), {.max_consumers = 1}));
  take(f.list.setModel(a));
  f.prepare();
  take(setVirtualListFirst(f.app->tree(), f.list.node(), 30));
  f.prepare();
  take(f.list.setModel(f.b));
  take(take(a.publisher()).publish(std::make_shared<SwitchRows>(2, 1000, false, false)));
  f.prepare();
  take(f.list.setModel(a));
  f.prepare();
  CHECK(take(f.list.restoreStatus()) == PositionResolution::UNSUPPORTED);
  take(f.list.setModel(a));
  CHECK(take(f.list.restoreStatus()) == PositionResolution::UNSUPPORTED);
  take(f.list.setModel(a, ModelRebindOptions{.restore = ModelRestorePolicy::RESET}));
  CHECK_FALSE(take(f.list.restoreStatus()));
  f.prepare();
  CHECK(f.firstListValue == 1000);
  take(f.app->tree().read(f.list.node(), [](const auto &view) {
    CHECK(view.scrollState().rasterOffset() == 0);
    CHECK_FALSE(view.selection());
  }));
}

TEST_CASE("remembered identities survive reordering and nearest requires declared deletion proof") {
  Fixture f;
  auto a =
    take(f.scene->model<int>(std::make_shared<OrderedRows>(1, std::initializer_list<std::uint64_t>{1, 2, 3, 4, 5})));
  take(f.list.setModel(a));
  f.prepare();
  take(setVirtualListFirst(f.app->tree(), f.list.node(), 1));
  take(setVirtualListSelection(f.app->tree(), f.list.node(), RowId{2, 1}));
  f.prepare();
  take(f.list.setModel(f.b));
  take(
    take(a.publisher()).publish(std::make_shared<OrderedRows>(2, std::initializer_list<std::uint64_t>{1, 3, 4, 5, 2})));
  f.prepare();
  take(f.list.setModel(a));
  f.prepare();
  CHECK(take(f.list.restoreStatus()) == PositionResolution::EXACT);
  CHECK(f.firstListValue == 20);
  take(f.app->tree().read(f.list.node(), [](const auto &view) {
    CHECK(view.scrollState().rasterOffset() == 4);
    CHECK(view.selection() == RowId{2, 1});
  }));
  take(f.list.setModel(f.b));
  take(take(a.publisher()).publish(std::make_shared<OrderedRows>(3, std::initializer_list<std::uint64_t>{1, 3, 4, 5})));
  f.prepare();
  const std::array changes{PositionChange::splice(4, 1, 0)};
  take(a.setChangeMap(take(PositionChangeMap::create(2, 3, 5, changes))));
  take(f.list.setModel(a));
  f.prepare();
  CHECK(take(f.list.restoreStatus()) == PositionResolution::NEAREST);
  CHECK(f.firstListValue == 50);
  take(f.app->tree().read(f.list.node(), [](const auto &view) {
    CHECK(view.selection() == RowId{5, 1});
  }));
}

TEST_CASE("shared model admission restores per-view reading and publishes coherent latest channels") {
  Fixture f;
  auto a = take(f.scene->model<int>(std::make_shared<IndexedRows>(1000)));
  auto b = take(f.scene->model<int>(std::make_shared<IndexedRows>(2000)));
  take(f.list.setModel(a));
  take(f.table.setModel(a));
  auto shared = take(f.scene->navigation(a));
  take(f.scene->bindNavigation(f.list.node(), shared));
  take(f.scene->bindNavigation(f.table.node(), shared));
  take(shared.navigate({32, 1}));
  const std::array selected{RowId{31, 1}, RowId{32, 1}};
  take(shared.setSelection(selected));
  f.prepare();
  take(setVirtualListFirst(f.app->tree(), f.list.node(), 30));
  take(setVirtualTableFirst(f.app->tree(), f.table.node(), 40));
  f.prepare();
  std::size_t observations{};
  take(f.scene->observe(take(shared.current()), [&](const NavigationPosition &) {
    const auto current = take(take(shared.current()).snapshot())->value;
    const auto selection = take(take(shared.selection()).snapshot())->value;
    const auto reading = take(take(shared.readingAnchor()).snapshot())->value;
    CHECK(current.resource == selection.resource());
    CHECK(current.resource == reading.resource);
    take(f.app->tree().read(f.list.node(), [&](const auto &view) {
      CHECK(view.selection() == current.row);
    }));
    take(f.app->tree().read(f.table.node(), [&](const auto &view) {
      CHECK(view.selection() == current.row);
    }));
    ++observations;
  }));
  take(f.list.setModel(b));
  f.prepare();
  CHECK(f.firstListValue == 2000);
  CHECK(f.tableValue() == 2000);
  take(shared.setModel(a));
  f.prepare();
  CHECK(f.firstListValue == 1030);
  CHECK(f.tableValue() == 1040);
  CHECK(take(take(shared.current()).snapshot())->value.row == RowId{32, 1});
  CHECK(std::ranges::equal(take(take(shared.selection()).snapshot())->value.rows(), selected));
  CHECK(observations >= 2);
}

TEST_CASE("second participant consumer failure preserves the entire installed group") {
  Fixture f;
  auto a = take(f.scene->model<int>(std::make_shared<IndexedRows>(1000)));
  auto limited = take(f.scene->model<int>(std::make_shared<IndexedRows>(2000), {.max_consumers = 1}));
  take(f.list.setModel(a));
  take(f.table.setModel(a));
  auto shared = take(f.scene->navigation(a));
  take(f.scene->bindNavigation(f.list.node(), shared));
  take(f.scene->bindNavigation(f.table.node(), shared));
  take(shared.navigate({3, 1}));
  f.prepare();
  const auto retained = take(f.table.visibleModel().snapshot());
  const auto current = take(take(shared.current()).snapshot());
  const auto selection = take(take(shared.selection()).snapshot());
  const auto reading = take(take(shared.readingAnchor()).snapshot());
  auto rejected = shared.setModel(limited);
  REQUIRE_FALSE(rejected);
  CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::NODE_LIMIT);
  CHECK(take(take(shared.current()).snapshot()) == current);
  CHECK(take(take(shared.selection()).snapshot()) == selection);
  CHECK(take(take(shared.readingAnchor()).snapshot()) == reading);
  CHECK(*retained.begin() == 1000);
  take(f.app->tree().read(f.table.node(), [](const auto &view) {
    CHECK(view.dataStatus().prepared);
    CHECK(view.selection() == RowId{3, 1});
  }));
  CHECK_FALSE(f.table.visibleModel().snapshot());
  f.prepare();
  CHECK(f.tableValue() == 1000);
}

TEST_CASE("resolver reentry invalidates a group stage after changing a Property channel") {
  Fixture f;
  auto rows = std::make_shared<SwitchRows>(1, 1000);
  auto a = take(f.scene->model<int>(rows));
  auto b = take(f.scene->model<int>(std::make_shared<IndexedRows>(2000)));
  take(f.list.setModel(a));
  take(f.table.setModel(a));
  auto shared = take(f.scene->navigation(a));
  take(f.scene->bindNavigation(f.list.node(), shared));
  take(f.scene->bindNavigation(f.table.node(), shared));
  take(shared.navigate({32, 1}));
  f.prepare();
  take(shared.setModel(b));
  f.prepare();
  const auto resource = take(take(shared.current()).snapshot())->value.resource;
  const auto retained = take(f.table.visibleModel().snapshot());
  bool called{};
  rows->onLocate = [&] {
    called = true;
    const std::array changed{RowId{7, 1}};
    take(shared.setSelection(changed));
  };
  const auto rejected = shared.setModel(a);
  CHECK(called);
  REQUIRE_FALSE(rejected);
  CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::WOULD_BLOCK);
  CHECK(take(take(shared.current()).snapshot())->value.resource == resource);
  CHECK(take(take(shared.selection()).snapshot())->value.rows().front() == RowId{7, 1});
  CHECK(*retained.begin() == 2000);
  take(f.app->tree().read(f.table.node(), [](const auto &view) {
    CHECK(view.dataStatus().prepared);
    CHECK_FALSE(view.selection());
  }));
  CHECK_FALSE(f.table.visibleModel().snapshot());
  f.prepare();
  CHECK(f.tableValue() == 2000);
}

TEST_CASE("postcommit cancellation may rebind a newer group without old continuation writes") {
  Fixture f;
  auto a = take(f.scene->model<int>(std::make_shared<IndexedRows>(1000)));
  auto b = take(f.scene->model<int>(std::make_shared<IndexedRows>(2000)));
  auto c = take(f.scene->model<int>(std::make_shared<IndexedRows>(3000)));
  take(f.list.setModel(a));
  take(f.table.setModel(a));
  auto shared = take(f.scene->navigation(a));
  take(f.scene->bindNavigation(f.list.node(), shared));
  take(f.scene->bindNavigation(f.table.node(), shared));
  take(shared.navigate({3, 1}));
  f.prepare();
  take(shared.beginResolve(1));
  bool called{};
  std::stop_callback callback(take(shared.stopToken()), [&] {
    called = true;
    take(shared.setModel(c));
  });
  take(shared.setModel(b));
  CHECK(called);
  f.prepare();
  CHECK(f.firstListValue == 3000);
  CHECK(f.tableValue() == 3000);
  const auto current = take(take(shared.current()).snapshot())->value;
  CHECK(current.resource == take(take(shared.selection()).snapshot())->value.resource());
  CHECK(current.resource == take(take(shared.readingAnchor()).snapshot())->value.resource);
}

TEST_CASE("the same model with one consumer capacity is a checked no-op") {
  Fixture f;
  auto limited = take(f.scene->model<int>(std::make_shared<Rows>(300), {.max_consumers = 1}));

  take(f.list.setModel(limited));
  f.prepare();
  CHECK(f.firstListValue == 300);
  take(f.app->tree().consumeFrameRequest());
  take(f.list.setModel(limited));
  CHECK(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  f.prepare();
  CHECK_FALSE(f.firstListValue);
  take(f.app->tree().read(f.list.node(), [](const auto &view) {
    CHECK(view.dataStatus().prepared);
    CHECK(view.dataStatus().rowCount == 4);
  }));

  // The second view cannot acquire the occupied slot; its original data stays usable.
  CHECK_FALSE(f.table.setModel(limited));
  CHECK(f.tableValue() == 10);
}

TEST_CASE("hidden views accept a new model without exposing their previous prepared range") {
  Fixture f;
  f.prepare();
  take(f.app->tree().setVisible(f.list.node(), false));
  take(f.app->tree().setVisible(f.table.node(), false));
  take(f.list.setModel(f.b));
  take(f.table.setModel(f.b));
  f.prepare();
  CHECK_FALSE(f.firstListValue);
  take(f.app->tree().setVisible(f.list.node(), true));
  take(f.app->tree().setVisible(f.table.node(), true));
  f.prepare();
  CHECK(f.firstListValue == 100);
  CHECK(f.tableValue() == 100);
}

TEST_CASE("hidden remembered view defers payload preparation until reveal and does not keep redrawing") {
  Fixture f;
  auto rows = std::make_shared<SwitchRows>(1, 1000);
  auto a = take(f.scene->model<int>(rows));
  take(f.list.setModel(a));
  f.prepare();
  take(setVirtualListFirst(f.app->tree(), f.list.node(), 30));
  f.prepare();
  take(f.app->tree().setVisible(f.list.node(), false));
  take(f.list.setModel(f.b));
  f.prepare();
  const auto reads = rows->reads;
  take(f.list.setModel(a));
  f.prepare();
  CHECK(rows->reads == reads);
  CHECK_FALSE(f.firstListValue);
  take(f.app->tree().consumeFrameRequest());
  const auto idleLayout = take(LayoutSnapshot::capture(f.app->tree(), f.app->root()));
  take(f.app->scheduler().dispatch());
  take(f.scene->afterDispatch(*f.app));
  take(f.scene->prepare(*f.app, {40, 8}));
  take(f.scene->prepareViews(*f.app, idleLayout));
  take(f.scene->completedLayout(*f.app));
  CHECK(rows->reads == reads);
  CHECK(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  take(f.app->tree().setVisible(f.list.node(), true));
  f.prepare();
  CHECK(f.firstListValue == 1030);
  CHECK(take(f.list.restoreStatus()) == PositionResolution::EXACT);
}

TEST_CASE("automatic data state observes the new source at the same revision") {
  Fixture f(RangeState::READY, true);
  f.prepare();
  auto failed = take(f.scene->model<int>(std::make_shared<Rows>(100, RangeState::FAILED)));

  take(f.list.setModel(failed));
  f.prepare();
  take(f.app->tree().read(f.state.node(), [](const DataStateView &view) {
    CHECK(view.presentation().phase == DataPhase::FAILED);
  }));
  take(f.list.setModel(f.b));
  f.prepare();
  take(f.app->tree().read(f.state.node(), [](const DataStateView &view) {
    CHECK(view.presentation().phase == DataPhase::READY);
  }));
}

TEST_CASE("model replacement rejects a foreign thread and diagnostic reentry") {
  Fixture f;
  f.prepare();
  bool rejected{};
  std::thread worker([&] {
    const auto result = f.list.setModel(f.b);
    rejected = !result && std::get<TreeError>(result.error().cause) == TreeError::WRONG_THREAD;
  });

  worker.join();
  CHECK(rejected);
  take(f.scene->onError([&](const ManagedDiagnostic &) {
    const auto blocked = f.table.setModel(f.b);

    REQUIRE_FALSE(blocked);
    CHECK(std::get<TreeError>(blocked.error().cause) == TreeError::WOULD_BLOCK);
  }));
  CHECK_FALSE(f.list.model(ManagedModel<int>{}));
  CHECK(f.tableValue() == 10);
}

TEST_CASE("repeated loaded swaps release old consumers while a sibling keeps its original model") {
  Fixture f;

  for (unsigned turn = 0; turn < 40; ++turn) {
    const bool useB = turn % 2 == 0;

    take(f.list.setModel(useB ? f.b : f.a));
    f.prepare();
    CHECK(f.firstListValue == (useB ? 100 : 10));
    CHECK(f.tableValue() == 10);
  }

  CHECK(take(f.scene->diagnostics()).errors.empty());
}

TEST_CASE("retiring a text collection owner cannot erase the migrated provider callback") {
  class EmptySegments final : public ModelSnapshot<TextSegment> {
  public:
    EmptySegments() : ModelSnapshot(1, 0) {
    }

    ModelRows<TextSegment> read(RowRange) const override {
      return {RangeState::READY, {}, {}};
    }
  };

  Fixture f;
  take(f.scene->remove(f.body));
  ui::Ref<LineEdit> input;
  ui::Ref<VirtualList<TextSegment>> segments;
  take(ui::mount(*f.scene, ui::vbox(ui::lineEdit(input, "one two"),
                                    ui::virtualList(ui::words(input.draftText())).reference(segments))));
  f.prepare();
  auto replacement = take(f.scene->model<TextSegment>(std::make_shared<EmptySegments>()));
  std::size_t callbacks{};
  ManagedViewHandler<TextSegment> provider = [&](const auto &, const auto &) -> std::expected<void, TreeError> {
    ++callbacks;

    return {};
  };

  take(segments.setModel(replacement, provider));
  f.prepare();
  const auto before = callbacks;
  take(f.scene->remove(input.node()));
  f.prepare();
  CHECK(callbacks > before);
  take(f.app->tree().read(segments.node(), [](const auto &view) {
    CHECK(view.dataStatus().prepared);
    CHECK(view.dataStatus().rowCount == 0);
  }));
}

TEST_CASE("old callback capture destruction cannot reenter a sibling model replacement") {
  struct Capture {
    std::function<void()> action;

    ~Capture() {
      action();
    }
  };

  bool attempted{};
  bool blocked{};
  Fixture f;
  auto capture = std::make_shared<Capture>();
  capture->action = [table = f.table, model = f.b, &attempted, &blocked] {
    attempted = true;
    const auto result = table.setModel(model);
    blocked = !result && std::get<TreeError>(result.error().cause) == TreeError::WOULD_BLOCK;
  };
  ManagedViewHandler<int> handler = [capture](const auto &, const auto &) -> std::expected<void, TreeError> {
    return {};
  };

  take(f.list.setModel(f.a, std::move(handler)));
  capture.reset();
  take(f.list.setModel(f.b, ManagedViewHandler<int>{}));
  CHECK(attempted);
  CHECK(blocked);
  f.prepare();
  CHECK(f.firstListValue == 100);
  CHECK(f.tableValue() == 10);
}

TEST_CASE("callback-only shared replacement preserves a reading channel independently of empty current") {
  Fixture f;
  auto a = take(f.scene->model<int>(std::make_shared<IndexedRows>(1000), {.max_consumers = 1}));
  take(f.list.setModel(a));
  auto shared = take(f.scene->navigation(a));
  take(f.scene->bindNavigation(f.list.node(), shared));
  const auto resource = take(take(shared.current()).snapshot())->value.resource;
  auto mark = take(ModelBookmark::create(resource, {PositionKind::ITEM, {31, 1}, 30, 1, PositionAffinity::FORWARD}));
  take(shared.setReadingAnchor(mark));
  f.prepare();
  const auto current = take(take(shared.current()).snapshot());
  const auto reading = take(take(shared.readingAnchor()).snapshot());
  const auto selection = take(take(shared.selection()).snapshot());
  REQUIRE(reading->value.resolution == PositionResolution::EXACT);
  REQUIRE(current->value.resolution == PositionResolution::EMPTY);
  ManagedViewHandler<int> replacement = [](const auto &, const auto &) -> std::expected<void, TreeError> {
    return {};
  };
  take(f.list.setModel(a, replacement));
  CHECK(take(take(shared.current()).snapshot()) == current);
  CHECK(take(take(shared.readingAnchor()).snapshot()) == reading);
  CHECK(take(take(shared.selection()).snapshot()) == selection);
  take(f.list.setModel(a, ManagedViewHandler<int>{}));
  CHECK(take(take(shared.current()).snapshot()) == current);
  CHECK(take(take(shared.readingAnchor()).snapshot()) == reading);
  CHECK(take(take(shared.selection()).snapshot()) == selection);
  take(shared.navigate({5, 1}));
  const auto request = take(shared.beginResolve(1));
  const auto stop = take(shared.stopToken());
  take(f.list.setModel(a, replacement));
  CHECK_FALSE(stop.stop_requested());
  const PositionReply reply{
    request.bookmark.resource(), request.session, request.revision, request.sequence, PositionResolution::EXACT,
    request.bookmark.position()};
  CHECK(take(shared.applyReply(reply)));
}

TEST_CASE("shared restoration preserves separate exact current and nearest reading outcomes") {
  Fixture f;
  auto a =
    take(f.scene->model<int>(std::make_shared<OrderedRows>(1, std::initializer_list<std::uint64_t>{1, 2, 3, 4, 5})));
  auto shared = take(f.scene->navigation(a));
  take(shared.navigate({1, 1}));
  const auto resource = take(take(shared.current()).snapshot())->value.resource;
  take(shared.setReadingAnchor(
    take(ModelBookmark::create(resource, {PositionKind::ITEM, {2, 1}, 1, 1, PositionAffinity::FORWARD}))));
  take(shared.setModel(f.b));
  take(take(a.publisher()).publish(std::make_shared<OrderedRows>(2, std::initializer_list<std::uint64_t>{1, 3, 4, 5})));
  const std::array changes{PositionChange::splice(1, 1, 0)};
  take(a.setChangeMap(take(PositionChangeMap::create(1, 2, 5, changes))));
  f.prepare();
  take(shared.setModel(a));
  const auto current = take(take(shared.current()).snapshot())->value;
  const auto reading = take(take(shared.readingAnchor()).snapshot())->value;
  CHECK(current.row == RowId{1, 1});
  CHECK(current.resolution == PositionResolution::EXACT);
  CHECK(reading.row == RowId{3, 1});
  CHECK(reading.resolution == PositionResolution::NEAREST);
}

TEST_CASE("navigation rejects a different row type even with no participating views") {
  class DoubleRows final : public ModelSnapshot<double> {
  public:
    DoubleRows() : ModelSnapshot(1, 1) {
    }

    ModelRows<double> read(RowRange range) const override {
      return {RangeState::READY, std::span(rows_).subspan(static_cast<std::size_t>(range.first), range.count), {}};
    }

  private:
    std::array<ModelRow<double>, 1> rows_{{{{1, 1}, 1.5}}};
  };

  Fixture f;
  auto shared = take(f.scene->navigation(f.a));
  auto wrong = take(f.scene->model<double>(std::make_shared<DoubleRows>()));
  const auto current = take(take(shared.current()).snapshot());
  const auto selection = take(take(shared.selection()).snapshot());
  const auto reading = take(take(shared.readingAnchor()).snapshot());
  auto result = shared.setModel(wrong);
  REQUIRE_FALSE(result);
  CHECK(std::get<TreeError>(result.error().cause) == TreeError::INVALID_ARGUMENT);
  CHECK(take(take(shared.current()).snapshot()) == current);
  CHECK(take(take(shared.selection()).snapshot()) == selection);
  CHECK(take(take(shared.readingAnchor()).snapshot()) == reading);
}

TEST_CASE("explicitly attested resource restores across model registrations and delivery sessions") {
  Fixture f;
  auto a = take(f.scene->model<int>(std::make_shared<IndexedRows>(1000)));
  auto reopened = take(f.scene->model<int>(std::make_shared<IndexedRows>(3000)));
  const auto resource = take(ModelResource::create());
  const auto firstSession = take(ModelResource::create()).identity;
  const auto reopenedSession = take(ModelResource::create()).identity;
  take(f.list.setModel(a));
  auto shared = take(f.scene->navigation(a, resource, firstSession));
  auto attested = take(f.scene->navigation(reopened, resource, reopenedSession));
  take(f.scene->bindNavigation(f.list.node(), shared, {.readingAnchor = true}));
  take(shared.navigate({32, 1}));
  const std::array selected{RowId{32, 1}};
  take(shared.setSelection(selected));
  take(shared.setReadingAnchor(
    take(ModelBookmark::create(resource, {PositionKind::ITEM, {31, 1}, 30, 1, PositionAffinity::FORWARD}))));
  f.prepare();
  take(shared.setModel(f.b));
  f.prepare();
  take(shared.setModel(reopened));
  f.prepare();
  CHECK(take(take(shared.current()).snapshot())->value.row == RowId{32, 1});
  CHECK(take(take(shared.readingAnchor()).snapshot())->value.row == RowId{31, 1});
  CHECK(std::ranges::equal(take(take(shared.selection()).snapshot())->value.rows(), selected));
  CHECK(f.firstListValue == 3030);
  CHECK_FALSE(take(take(attested.current()).snapshot())->value.row);
  auto unrelated = take(f.scene->model<int>(std::make_shared<IndexedRows>(4000)));
  take(shared.setModel(unrelated));
  CHECK_FALSE(take(take(shared.current()).snapshot())->value.row);
}

TEST_CASE("same identity with incompatible lineage remains an observed terminal restore") {
  Fixture f;
  auto a = take(f.scene->model<int>(std::make_shared<IndexedRows>(1000)));
  auto changed = take(f.scene->model<int>(std::make_shared<IndexedRows>(3000)));
  const auto resource = take(ModelResource::create());
  auto incompatible = resource;
  ++incompatible.lineage;
  take(f.list.setModel(a));
  auto shared = take(f.scene->navigation(a, resource, take(ModelResource::create()).identity));
  auto attested = take(f.scene->navigation(changed, incompatible, take(ModelResource::create()).identity));
  take(f.scene->bindNavigation(f.list.node(), shared));
  take(shared.navigate({32, 1}));
  const std::array selected{RowId{32, 1}};
  take(shared.setSelection(selected));
  f.prepare();
  take(shared.setModel(f.b));
  f.prepare();
  take(shared.setModel(changed));
  f.prepare();
  CHECK(take(take(shared.current()).snapshot())->value.resolution == PositionResolution::INCOMPATIBLE);
  CHECK(take(f.list.restoreStatus()) == PositionResolution::INCOMPATIBLE);
  const auto unresolved = take(take(shared.selection()).snapshot())->value;
  CHECK(unresolved.resource() == resource);
  CHECK(unresolved.resolution() == PositionResolution::INCOMPATIBLE);
  CHECK(std::ranges::equal(unresolved.rows(), selected));
  CHECK_FALSE(f.firstListValue);
  take(f.list.setModel(changed, ModelRebindOptions{.restore = ModelRestorePolicy::RESET}));
  f.prepare();
  CHECK_FALSE(take(f.list.restoreStatus()));
  CHECK(f.firstListValue == 3000);
  (void)attested;
}

TEST_CASE("explicit resource metadata survives a gap with no live registration witness") {
  struct Source final : detail::ManagedModelSource {
    std::expected<void, TreeError> pin() override {
      return {};
    }

    std::expected<std::optional<TreeError>, TreeError> dataError() const override {
      return std::nullopt;
    }

    bool bound() const noexcept override {
      return false;
    }

    std::expected<bool, TreeError> synchronize() override {
      return false;
    }

    void retire() noexcept override {
      retired = true;
    }
  };

  auto lifetime = std::make_shared<int>(0);
  auto history = take(detail::ModelViewHistory::create(std::weak_ptr<const void>(lifetime), {}));
  const auto resource = take(ModelResource::create());
  auto source = std::make_shared<Source>();
  source->navigationResource = resource;
  source->navigationExplicit = true;
  detail::ModelViewCheckpoint value;
  value.current =
    take(ModelBookmark::create(resource, {PositionKind::ITEM, {32, 1}, 31, 1, PositionAffinity::FORWARD}));
  value.currentResolution = PositionResolution::EXACT;
  history->commit(take(history->stage(source, 0, value)));
  std::weak_ptr<Source> witness = source;
  source->retire();
  source.reset();
  REQUIRE(witness.expired());
  history->prune();
  auto reopened = std::make_shared<Source>();
  reopened->navigationResource = resource;
  reopened->navigationExplicit = true;
  auto restored = history->find(reopened, 0);
  REQUIRE(restored);
  REQUIRE(restored->value.current);
  CHECK(restored->value.current->position().row == RowId{32, 1});
  CHECK(restored->model.expired());

  auto ephemeral = std::make_shared<Source>();
  const auto automatic = take(ModelResource::create());
  ephemeral->navigationResource = automatic;
  history->commit(take(history->stage(ephemeral, 0, {})));
  ephemeral->retire();
  ephemeral.reset();
  history->prune();
  auto automaticProbe = std::make_shared<Source>();
  automaticProbe->navigationResource = automatic;
  CHECK_FALSE(history->find(automaticProbe, 0));
}

TEST_CASE("confirmed deletion removes a non-current selected identity without choosing its neighbor") {
  Fixture f;
  auto a =
    take(f.scene->model<int>(std::make_shared<OrderedRows>(1, std::initializer_list<std::uint64_t>{1, 2, 3, 4, 5})));
  auto shared = take(f.scene->navigation(a));
  take(shared.navigate({1, 1}));
  const std::array selected{RowId{2, 1}, RowId{4, 1}};
  take(shared.setSelection(selected));
  take(shared.setModel(f.b));
  take(take(a.publisher()).publish(std::make_shared<OrderedRows>(2, std::initializer_list<std::uint64_t>{1, 3, 4, 5})));
  const std::array changes{PositionChange::splice(1, 1, 0)};
  take(a.setChangeMap(take(PositionChangeMap::create(1, 2, 5, changes))));
  f.prepare();
  take(shared.setModel(a));
  const auto restored = take(take(shared.selection()).snapshot())->value;
  const std::array expected{RowId{4, 1}};
  CHECK(std::ranges::equal(restored.rows(), expected));
  CHECK(restored.resolution() == PositionResolution::EXACT);
  CHECK(take(take(shared.current()).snapshot())->value.row == RowId{1, 1});
}

TEST_CASE("pending canonical selection retries newer publication and preserves newer manual channels") {
  Fixture f;
  auto a = take(f.scene->model<int>(std::make_shared<SwitchRows>(1, 1000)));
  take(f.list.setModel(a));
  auto shared = take(f.scene->navigation(a));
  take(f.scene->bindNavigation(f.list.node(), shared));
  take(shared.navigate({1, 1}));
  const std::array selected{RowId{2, 1}};
  take(shared.setSelection(selected));
  f.prepare();
  take(setVirtualListFirst(f.app->tree(), f.list.node(), 30));
  f.prepare();
  take(shared.setModel(f.b));
  auto partial = std::make_shared<SwitchRows>(2, 1000);
  partial->unsupportedId = 2;
  take(take(a.publisher()).publish(partial));
  f.prepare();
  take(shared.setModel(a));
  f.prepare();
  CHECK(take(take(shared.selection()).snapshot())->value.resolution() == PositionResolution::UNSUPPORTED);
  const auto lookups = partial->lookupTwo;
  f.prepare();
  CHECK(partial->lookupTwo == lookups);

  SUBCASE("a newer publication resolves the canonical channel and local paused reading") {
    take(take(a.publisher()).publish(std::make_shared<SwitchRows>(3, 1000)));
    f.prepare();
    const auto restored = take(take(shared.selection()).snapshot())->value;
    CHECK(restored.resolution() == PositionResolution::EXACT);
    CHECK(std::ranges::equal(restored.rows(), selected));
    CHECK(f.firstListValue == 1030);
    CHECK(take(f.list.restoreStatus()) == PositionResolution::EXACT);
  }

  SUBCASE("new manual current reading and selection supersede only their earlier intents") {
    take(shared.navigate({4, 1}));
    const auto resource = take(take(shared.current()).snapshot())->value.resource;
    take(shared.setReadingAnchor(
      take(ModelBookmark::create(resource, {PositionKind::ITEM, {35, 1}, 34, 2, PositionAffinity::FORWARD}))));
    const std::array manual{RowId{4, 1}};
    take(shared.setSelection(manual));
    const auto current = take(take(shared.current()).snapshot());
    const auto reading = take(take(shared.readingAnchor()).snapshot());
    const auto selection = take(take(shared.selection()).snapshot());
    f.prepare();
    CHECK(partial->lookupTwo == lookups);
    auto ready = std::make_shared<SwitchRows>(3, 1000);
    take(take(a.publisher()).publish(ready));
    f.prepare();
    CHECK(take(take(shared.current()).snapshot()) == current);
    CHECK(take(take(shared.readingAnchor()).snapshot()) == reading);
    CHECK(take(take(shared.selection()).snapshot()) == selection);
    CHECK(ready->lookupTwo == 0);
    const auto bookmark = take(shared.bookmark());
    const auto readingBookmark = take(shared.readingBookmark());
    REQUIRE(bookmark);
    REQUIRE(readingBookmark);
    CHECK(bookmark->position().row == RowId{4, 1});
    CHECK(readingBookmark->position().row == RowId{35, 1});
  }
}

TEST_CASE("a conflicting canonical resource qualification is rejected before replacing history") {
  Fixture f;
  const auto resource = take(ModelResource::create());
  auto shared = take(f.scene->navigation(f.a, resource, take(ModelResource::create()).identity));
  const auto before = take(take(shared.current()).snapshot());
  const auto conflict = f.scene->navigation(f.a, take(ModelResource::create()), take(ModelResource::create()).identity);
  REQUIRE_FALSE(conflict);
  CHECK(std::get<TreeError>(conflict.error().cause) == TreeError::INVALID_ARGUMENT);
  CHECK(take(take(shared.current()).snapshot()) == before);
  auto separate = take(f.scene->navigation(f.a));
  CHECK(take(take(separate.current()).snapshot())->value.resource == resource);
}

TEST_CASE("publication backpressure is observed after a newer cancellation rebind without rollback") {
  auto app = take(Application::create({.scheduler = {.data_capacity = 1}}));
  auto scene = take(ManagedScene::create(*app));
  auto a = take(scene->model<int>(std::make_shared<IndexedRows>(1000)));
  auto b = take(scene->model<int>(std::make_shared<IndexedRows>(2000)));
  auto c = take(scene->model<int>(std::make_shared<IndexedRows>(3000)));
  auto shared = take(scene->navigation(a));
  take(shared.navigate({1, 1}));
  take(scene->observe(take(shared.current()), [](const auto &) {
  }));
  take(scene->observe(take(shared.selection()), [](const auto &) {
  }));
  take(shared.beginResolve(1));
  bool rebound{};
  std::stop_callback callback(take(shared.stopToken()), [&] {
    take(shared.setModel(c));
    rebound = true;
  });
  take(shared.setModel(b));
  CHECK(rebound);
  auto cContext = take(scene->navigation(c));
  const auto cResource = take(take(cContext.current()).snapshot())->value.resource;
  CHECK(take(take(shared.current()).snapshot())->value.resource == cResource);
  CHECK(take(take(shared.selection()).snapshot())->value.resource() == cResource);
  const auto diagnostics = take(scene->diagnostics());
  REQUIRE(diagnostics.errors.size() == 2);
  for (const auto &diagnostic : diagnostics.errors) {
    const auto *application = std::get_if<ApplicationError>(&diagnostic.error.cause);
    REQUIRE(application);
    CHECK(application->phase == ApplicationPhase::DISPATCH);
    const auto *failure = std::get_if<SchedulerError>(&application->cause);
    REQUIRE(failure);
    CHECK(*failure == SchedulerError::WOULD_BLOCK);
  }
}

TEST_CASE("unversioned models rebind with an unsupported checkpoint and healthy independent views") {
  Fixture f(RangeState::READY, true);
  auto zero =
    take(f.scene->model<int>(std::make_shared<OrderedRows>(0, std::initializer_list<std::uint64_t>{1, 2, 3, 4, 5})));
  take(f.list.setModel(zero));
  f.prepare();
  take(setVirtualListFirst(f.app->tree(), f.list.node(), 2));
  take(setVirtualListSelection(f.app->tree(), f.list.node(), RowId{3, 1}));
  f.prepare();
  CHECK(f.firstListValue == 30);
  const auto leaving = f.list.setModel(f.b);
  if (!leaving) {
    INFO("leaving revision0 cause=", static_cast<int>(std::get<TreeError>(leaving.error().cause)));
  }
  REQUIRE(leaving);
  f.prepare();
  CHECK(f.firstListValue == 100);
  take(f.list.setModel(zero));
  f.prepare();
  CHECK(take(f.list.restoreStatus()) == PositionResolution::UNSUPPORTED);
  CHECK_FALSE(f.firstListValue);
  take(f.app->tree().read(f.state.node(), [](const auto &view) {
    CHECK(view.presentation().phase == DataPhase::FAILED);
  }));
  take(f.table.setModel(zero));
  f.prepare();
  CHECK(f.tableValue() == 10);
  CHECK_FALSE(take(f.table.restoreStatus()));
  take(take(zero.publisher())
         .publish(std::make_shared<OrderedRows>(1, std::initializer_list<std::uint64_t>{1, 2, 3, 4, 5})));
  f.prepare();
  CHECK(take(f.list.restoreStatus()) == PositionResolution::UNSUPPORTED);
  CHECK_FALSE(f.firstListValue);
  CHECK(f.tableValue() == 10);
  take(f.list.setModel(zero, ModelRebindOptions{.restore = ModelRestorePolicy::RESET}));
  f.prepare();
  CHECK(f.firstListValue == 10);
  CHECK_FALSE(take(f.list.restoreStatus()));
  CHECK(take(f.scene->diagnostics()).errors.empty());
}

TEST_CASE("explicit reset leaves and returns to an unversioned capacity-one model") {
  Fixture f;
  auto zero = take(f.scene->model<int>(
    std::make_shared<OrderedRows>(0, std::initializer_list<std::uint64_t>{1, 2, 3, 4, 5}), {.max_consumers = 1}));
  take(f.list.setModel(zero));
  f.prepare();
  take(setVirtualListFirst(f.app->tree(), f.list.node(), 2));
  f.prepare();
  take(f.list.setModel(f.b, ModelRebindOptions{.restore = ModelRestorePolicy::RESET}));
  f.prepare();
  CHECK(f.firstListValue == 100);
  take(f.list.setModel(zero, ModelRebindOptions{.restore = ModelRestorePolicy::RESET}));
  f.prepare();
  CHECK(f.firstListValue == 10);
  take(setVirtualListFirst(f.app->tree(), f.list.node(), 2));
  f.prepare();
  take(f.list.setModel(zero, ModelRebindOptions{.restore = ModelRestorePolicy::RESET}));
  f.prepare();
  CHECK(f.firstListValue == 10);
  CHECK_FALSE(take(f.list.restoreStatus()));
  take(f.app->tree().read(f.list.node(), [](const auto &view) {
    CHECK(view.dataStatus().prepared);
    CHECK(view.dataStatus().dataRevision == 0);
    CHECK_FALSE(view.selection());
  }));
}

TEST_CASE("shared unversioned models keep native keyboard navigation usable without bookmark continuity") {
  Fixture f;
  auto zero =
    take(f.scene->model<int>(std::make_shared<OrderedRows>(0, std::initializer_list<std::uint64_t>{1, 2, 3, 4, 5})));
  auto shared = take(f.scene->navigation(f.a));
  take(f.scene->bindNavigation(f.list.node(), shared));
  take(f.scene->bindNavigation(f.table.node(), shared));
  f.prepare();
  take(shared.setModel(zero, ModelRebindOptions{.restore = ModelRestorePolicy::RESET}));
  f.prepare();
  CHECK(f.firstListValue == 10);
  CHECK(f.tableValue() == 10);
  const auto unsupported = shared.navigate({2, 1});
  REQUIRE_FALSE(unsupported);
  CHECK(unsupported.error() == TreeError::WOULD_BLOCK);
  CHECK(take(take(shared.current()).snapshot())->value.resolution == PositionResolution::UNSUPPORTED);
  take(setVirtualListSelection(f.app->tree(), f.list.node(), RowId{1, 1}));
  CHECK(take(f.app->router().invoke(f.list.node(), {Action::MOVE_DOWN})).status == DispatchStatus::HANDLED);
  f.prepare();
  take(f.app->tree().read(f.list.node(), [](const auto &view) {
    CHECK(view.selection() == RowId{2, 1});
  }));
  f.prepare();
  take(f.app->tree().read(f.list.node(), [](const auto &view) {
    CHECK(view.selection() == RowId{2, 1});
  }));
  take(shared.setModel(f.a, ModelRebindOptions{.restore = ModelRestorePolicy::RESET}));
  f.prepare();
  CHECK(f.firstListValue == 10);
  CHECK(f.tableValue() == 10);
  CHECK(take(f.scene->diagnostics()).errors.empty());
}
} // namespace
