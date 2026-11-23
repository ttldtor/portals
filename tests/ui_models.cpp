// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/ui.hpp>

namespace {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> value) {
  REQUIRE(value.has_value());
  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

struct Item {
  std::string_view name;
  int quantity;
};

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> scene;

  explicit Fixture(ManagedOptions options = {}) : scene(take(ManagedScene::create(*app, options))) {
  }

  void prepare() {
    take(scene->prepare(*app, {40, 12}));
    const auto bounds = take(CellRect::from({}, {40, 12}));
    std::optional<LayoutSnapshot> layout;
    take(app->tree().frame([&] {
      take(layoutTree(app->tree(), app->root(), bounds, bounds));
      layout = take(LayoutSnapshot::capture(app->tree(), app->root()));
    }));
    take(scene->prepareViews(*app, *layout));
  }
};

class CountingRows final : public ModelSnapshot<int> {
public:
  CountingRows() : ModelSnapshot(1, 1000000) {
  }

  ModelRows<int> read(RowRange) const override {
    ++reads;
    return {};
  }

  mutable std::size_t reads{};
};

TEST_CASE("cold provider declarations and repeated views do not read or duplicate sources") {
  Fixture fixture({.maxModels = 1});
  auto provider = std::make_shared<CountingRows>();
  auto model = ui::model<int>(provider);
  auto copy = model;
  CHECK(provider->reads == 0);
  CHECK_FALSE(model.managed());
  take(ui::mount(*fixture.scene, ui::row(ui::virtualList(model), ui::virtualList(copy))));
  CHECK(provider->reads == 0);
  CHECK(model.managed());
  CHECK(copy.managed());
  auto another = fixture.scene->model<int>(provider);
  REQUIRE_FALSE(another);
  CHECK(std::get<TreeError>(another.error().cause) == TreeError::NODE_LIMIT);
}

TEST_CASE("typed cold navigation binds projected views and independent contexts stay separate") {
  Fixture fixture;
  auto model = ui::model(std::array{Item{"A", 1}, Item{"B", 2}, Item{"C", 3}});
  auto shared = ui::navigation(model, {.readingAnchor = true});
  auto separate = ui::navigation(model);
  ui::Ref<VirtualList<Item>> list;
  ui::Ref<VirtualTable<Item>> table;
  ui::Ref<VirtualList<Item>> other;
  take(ui::mount(*fixture.scene, ui::column(ui::virtualList(shared, &Item::name).reference(list),
                                            ui::virtualTable(shared, tableColumn("Name", &Item::name, 12),
                                                             tableColumn("Quantity", &Item::quantity, 8))
                                              .reference(table),
                                            ui::virtualList(separate, [](const Item &item) {
                                              return item.quantity;
                                            }).reference(other))));
  take(take(shared.managed()).navigate({2, 1}));
  fixture.prepare();
  fixture.prepare();
  take(fixture.app->tree().read(list.node(), [](const auto &view) {
    CHECK(view.selection() == RowId{2, 1});
  }));
  take(fixture.app->tree().read(table.node(), [](const auto &view) {
    CHECK(view.selection() == RowId{2, 1});
  }));
  CHECK_FALSE(take(take(take(separate.managed()).current()).snapshot())->value.row);
  take(fixture.scene->close());
  CHECK_FALSE(shared.managed());
  CHECK_FALSE(model.managed());
}

TEST_CASE("cold schema failure preserves TextError and permits a clean retry") {
  Fixture fixture({.maxStates = 3, .maxModels = 1});
  auto model = ui::model(std::array{Item{"A", 1}, Item{"B", 2}});
  auto navigation = ui::navigation(model);
  const auto failed = ui::mount(
    *fixture.scene, ui::row(ui::virtualList(navigation, &Item::name),
                            ui::virtualTable(navigation, tableColumn(std::string_view("\xff", 1), &Item::name, 8))));
  REQUIRE_FALSE(failed);
  CHECK(failed.error().text.has_value());
  CHECK_FALSE(model.managed());
  CHECK_FALSE(navigation.managed());
  take(ui::mount(*fixture.scene, ui::virtualTable(navigation, tableColumn("Name", &Item::name, 8))));
  CHECK(model.managed());
  CHECK(navigation.managed());
}

TEST_CASE("cold target admission retains old shared model and releases a failed new registration") {
  Fixture fixture({.maxModels = 2});
  auto a = ui::model(std::array{1, 2, 3});
  auto b = ui::model(std::array{10, 20, 30}, CollectionLimits{}, ModelOptions{.max_consumers = 1});
  auto c = ui::model(std::array{100, 200, 300});
  auto navigation = ui::navigation(a);
  take(ui::mount(*fixture.scene, ui::row(ui::virtualList(navigation), ui::virtualList(navigation))));
  auto current = take(take(navigation.managed()).current());
  const auto before = take(current.snapshot());
  for (int attempt = 0; attempt < 3; ++attempt) {
    CHECK_FALSE(navigation.setModel(b));
    CHECK_FALSE(b.managed());
    CHECK(take(current.snapshot()) == before);
  }
  take(navigation.setModel(c));
  CHECK(c.managed());
}

TEST_CASE("cold models reject foreign scenes and invalid capacity through checked setup") {
  Fixture first;
  Fixture second;
  auto model = ui::model(std::array{1, 2});
  take(ui::mount(*first.scene, ui::virtualList(model)));
  const auto foreign = ui::mount(*second.scene, ui::virtualList(model));
  REQUIRE_FALSE(foreign);
  CHECK(std::get<TreeError>(foreign.error().cause) == TreeError::WRONG_SESSION);
  auto invalid = ui::model(std::array{1, 2}, CollectionLimits{.maxRows = 1});
  const auto excessive = ui::mount(*second.scene, ui::virtualList(invalid));
  REQUIRE_FALSE(excessive);
  CHECK(std::get<TreeError>(excessive.error().cause) == TreeError::NODE_LIMIT);
}

struct LifetimeHookState {
  std::function<void()> onCopy;
  std::function<void()> onDestroy;
};

struct LifetimeHook {
  explicit LifetimeHook(std::shared_ptr<LifetimeHookState> value) : state(std::move(value)) {
  }

  LifetimeHook(const LifetimeHook &other) : state(other.state) {
    if (state && state->onCopy) {
      auto action = std::move(state->onCopy);
      action();
    }
  }

  LifetimeHook(LifetimeHook &&) noexcept = default;

  ~LifetimeHook() {
    if (state && state->onDestroy) {
      auto action = std::move(state->onDestroy);
      action();
    }
  }

  std::expected<void, TreeError> operator()(const ModelFrame<int> &, const std::optional<ModelRequest<int>> &) const {
    return {};
  }

  std::shared_ptr<LifetimeHookState> state;
};

TEST_CASE("cold preparation copy may destroy its scene before model registration") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto state = std::make_shared<LifetimeHookState>();
  ManagedViewHandler<int> handler = LifetimeHook(state);
  auto a = ui::model(std::array{1, 2, 3});
  auto b = ui::model<int>(std::make_shared<CountingRows>(), {}, handler);
  auto navigation = ui::navigation(a);
  take(ui::mount(*scene, ui::virtualList(navigation)));
  state->onCopy = [&] {
    scene.reset();
  };
  const auto result = navigation.setModel(b);
  REQUIRE_FALSE(result);
  CHECK_FALSE(scene);
  CHECK(std::get<TreeError>(result.error().cause) == TreeError::CLOSED);
  CHECK_FALSE(navigation.managed());
  CHECK_FALSE(b.managed());
}

TEST_CASE("cold target rollback detaches its source before preparation destruction") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto state = std::make_shared<LifetimeHookState>();
  ManagedViewHandler<int> handler = LifetimeHook(state);
  auto a = ui::model(std::array{1, 2, 3});
  auto b = ui::model<int>(std::make_shared<CountingRows>(), {.max_consumers = 1}, handler);
  auto navigation = ui::navigation(a);
  take(ui::mount(*scene, ui::row(ui::virtualList(navigation), ui::virtualList(navigation))));
  state->onDestroy = [&] {
    scene.reset();
  };
  const auto result = navigation.setModel(b);
  REQUIRE_FALSE(result);
  CHECK_FALSE(scene);
  CHECK(std::get<TreeError>(result.error().cause) == TreeError::NODE_LIMIT);
  CHECK_FALSE(navigation.managed());
  CHECK_FALSE(b.managed());
}

template <class T> auto snapshot(Fixture &fixture, const ui::Model<T> &model) {
  take(fixture.scene->prepare(*fixture.app, {40, 12}));
  return take(portals::detail::ManagedModelAccess::snapshot(take(model.managed()), fixture.scene->root().id()));
}

TEST_CASE("owned sequence operations preserve duplicate-valued identities and never reuse erased IDs") {
  Fixture fixture;
  auto model = ui::model(std::array{10, 10, 30});
  take(ui::mount(*fixture.scene, ui::virtualList(model)));
  const auto original = snapshot(fixture, model);
  const auto initial = original->read({0, 3}).rows;
  const auto a = initial[0].id;
  const auto b = initial[1].id;
  REQUIRE(a != b);
  take(model.insert(0, 10));
  auto changed = snapshot(fixture, model);
  auto rows = changed->read({0, 4}).rows;
  const auto inserted = rows[0].id;
  CHECK(inserted != a);
  CHECK(inserted != b);
  CHECK(rows[1].id == a);
  CHECK(rows[2].id == b);
  take(model.move(2, 1, 0));
  changed = snapshot(fixture, model);
  CHECK(changed->read({0, 1}).rows[0].id == b);
  take(model.erase(1));
  take(model.insert(1, 10));
  changed = snapshot(fixture, model);
  CHECK(changed->read({1, 1}).rows[0].id != inserted);
  take(model.clear());
  changed = snapshot(fixture, model);
  CHECK(changed->rowCount() == 0);
  const auto emptyRevision = changed->revision();
  take(model.clear());
  CHECK(snapshot(fixture, model)->revision() == emptyRevision);
  take(model.insert(0, 10));
  changed = snapshot(fixture, model);
  CHECK(changed->read({0, 1}).rows[0].id != a);
  CHECK(changed->read({0, 1}).rows[0].id != b);
  CHECK(original->read({0, 3}).rows[1].id == b);
  CHECK(original->read({0, 3}).rows[1].value == 10);
}

TEST_CASE("current row reads the existing navigation channel without mounting an alias") {
  Fixture fixture;
  auto model = ui::model(std::array{10, 20});
  auto navigation = ui::navigation(model);
  const auto cold = navigation.currentRow();
  REQUIRE_FALSE(cold);
  CHECK(std::get<TreeError>(cold.error().cause) == TreeError::CLOSED);
  take(ui::mount(*fixture.scene, ui::virtualList(navigation)));
  CHECK_FALSE(take(navigation.currentRow()));
  const auto before = snapshot(fixture, model);
  const auto id = before->read({1, 1}).rows[0].id;
  auto context = take(navigation.managed());
  take(context.navigate(id));
  CHECK(take(navigation.currentRow()) == id);
  take(model.update(*take(navigation.currentRow()), 25));
  CHECK(snapshot(fixture, model)->read({1, 1}).rows[0].value == 25);
  std::expected<std::optional<RowId>, ManagedError> wrongThread;
  std::thread worker([&] { wrongThread = navigation.currentRow(); });
  worker.join();
  REQUIRE_FALSE(wrongThread);
  CHECK(std::get<TreeError>(wrongThread.error().cause) == TreeError::WRONG_THREAD);
  take(context.clearCurrent());
  CHECK_FALSE(take(navigation.currentRow()));
  take(fixture.scene->close());
  const auto closed = navigation.currentRow();
  REQUIRE_FALSE(closed);
  CHECK(std::get<TreeError>(closed.error().cause) == TreeError::CLOSED);
}

TEST_CASE("owned row update keeps its identity and previous snapshot") {
  Fixture fixture;
  auto model = ui::model(std::array{10, 20});
  take(ui::mount(*fixture.scene, ui::virtualList(model)));
  const auto before = snapshot(fixture, model);
  const auto id = before->read({1, 1}).rows[0].id;
  take(model.update(id, 25));
  const auto after = snapshot(fixture, model);
  CHECK(after->revision() == before->revision() + 1);
  CHECK(after->read({1, 1}).rows[0].id == id);
  CHECK(after->read({1, 1}).rows[0].value == 25);
  CHECK(before->read({1, 1}).rows[0].value == 20);
}

TEST_CASE("owned row update validates exact keys and collects the same failure once") {
  Fixture fixture;
  auto model = ui::model(std::array{10, 20});
  CHECK_FALSE(model.update({1, 1}, 30));
  take(ui::mount(*fixture.scene, ui::virtualList(model)));
  const auto before = snapshot(fixture, model);
  const auto id = before->read({0, 1}).rows[0].id;
  take(model.update(id, 10));
  CHECK(snapshot(fixture, model) == before);

  for (const auto key : std::array{RowId{}, RowId{999, 1}, RowId{id.value, id.generation + 1}}) {
    const auto rejected = model.update(key, 30);
    REQUIRE_FALSE(rejected);
    CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::INVALID_ARGUMENT);
    CHECK(snapshot(fixture, model) == before);
  }

  CHECK_FALSE(model.row({999, 1}, 30));
  const auto diagnostics = take(fixture.scene->diagnostics());
  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(std::get<TreeError>(diagnostics.errors[0].error.cause) == TreeError::INVALID_ARGUMENT);
  take(model.erase(0));
  take(model.insert(0, 10));
  const auto reused = snapshot(fixture, model);
  CHECK(reused->read({0, 1}).rows[0].id != id);
  CHECK_FALSE(model.update(id, 30));
  CHECK(snapshot(fixture, model) == reused);
  CHECK(model.row(reused->read({0, 1}).rows[0].id, 40));
  take(fixture.scene->close());
  const auto closed = model.update(id, 30);
  REQUIRE_FALSE(closed);
  CHECK(std::get<TreeError>(closed.error().cause) == TreeError::CLOSED);
}

TEST_CASE("owned update preserves thread admission and read-only provider capabilities") {
  Fixture fixture;
  auto model = ui::model(std::array{1});
  auto provider = std::make_shared<CountingRows>();
  auto readOnly = ui::model<int>(provider);
  take(ui::mount(*fixture.scene, ui::row(ui::virtualList(model), ui::virtualList(readOnly))));
  const auto before = snapshot(fixture, model);
  const auto id = before->read({0, 1}).rows[0].id;
  std::expected<void, ManagedError> rejected;
  std::thread worker([&] {
    rejected = model.update(id, 2);
  });
  worker.join();
  REQUIRE_FALSE(rejected);
  CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::WRONG_THREAD);
  CHECK(snapshot(fixture, model) == before);
  const auto unavailable = readOnly.update(id, 2);
  REQUIRE_FALSE(unavailable);
  CHECK(std::get<TreeError>(unavailable.error().cause) == TreeError::INVALID_ARGUMENT);
  CHECK(provider->reads == 0);
}

TEST_CASE("later view delivery failure keeps an accepted owned row replacement") {
  Fixture fixture;
  auto model = ui::model(std::array{1});
  bool rejectDelivery{};
  ManagedViewHandler<int> prepared = [&](const auto &, const auto &) -> std::expected<void, TreeError> {
    return rejectDelivery ? std::unexpected(TreeError::CALLBACK_FAILED) : std::expected<void, TreeError>{};
  };
  ui::Ref<VirtualList<int>> list;
  take(ui::mount(*fixture.scene, ui::virtualList(model).reference(list)));
  take(fixture.scene->setModel(list.node(), take(model.managed()), std::optional(prepared)));
  const auto before = snapshot(fixture, model);
  const auto id = before->read({0, 1}).rows[0].id;
  rejectDelivery = true;
  take(model.update(id, 2));
  const auto accepted = snapshot(fixture, model);
  CHECK(accepted->revision() == before->revision() + 1);
  CHECK(accepted->read({0, 1}).rows[0].value == 2);
  const auto bounds = take(CellRect::from({}, {40, 12}));
  std::optional<LayoutSnapshot> layout;
  take(fixture.app->tree().frame([&] {
    take(layoutTree(fixture.app->tree(), fixture.app->root(), bounds, bounds));
    layout = take(LayoutSnapshot::capture(fixture.app->tree(), fixture.app->root()));
  }));
  const auto failedDelivery = fixture.scene->prepareViews(*fixture.app, *layout);
  REQUIRE_FALSE(failedDelivery);
  CHECK(failedDelivery.error() == TreeError::CALLBACK_FAILED);
  const auto retained =
    take(portals::detail::ManagedModelAccess::snapshot(take(model.managed()), fixture.scene->root().id()));
  CHECK(retained == accepted);
  CHECK(before->read({0, 1}).rows[0].value == 1);
}

TEST_CASE("opaque owned rows publish a replacement without an equality requirement") {
  Fixture fixture;
  auto model = ui::model(std::array{Item{"A", 1}});
  take(ui::mount(*fixture.scene, ui::virtualList(model, &Item::name)));
  const auto before = snapshot(fixture, model);
  const auto id = before->read({0, 1}).rows[0].id;
  take(model.update(id, Item{"A", 1}));
  const auto after = snapshot(fixture, model);
  CHECK(after->revision() == before->revision() + 1);
  CHECK(after->read({0, 1}).rows[0].id == id);
}

TEST_CASE("value revisions preserve navigation channels and structural proof retention") {
  Fixture fixture;
  auto model =
    ui::model(std::array{10, 20, 30}, CollectionLimits{}, ModelOptions{}, PositionMapOptions{.maxChanges = 2});
  auto navigation = ui::navigation(model, {.readingAnchor = true});
  ui::Ref<VirtualList<int>> list;
  take(ui::mount(*fixture.scene, ui::virtualList(navigation).reference(list)));
  const auto before = snapshot(fixture, model);
  const auto rows = before->read({0, 3}).rows;
  auto context = take(navigation.managed());
  const auto resource = take(take(context.current()).snapshot())->value.resource;
  const auto deleted = take(ModelBookmark::create(resource, {PositionKind::ITEM, rows[1].id, 1, before->revision()}));
  take(context.navigate(rows[0].id));
  take(context.setSelection(std::array{rows[0].id, rows[2].id}));
  take(context.setReadingAnchor(
    take(ModelBookmark::create(resource, {PositionKind::ITEM, rows[2].id, 2, before->revision()}))));
  fixture.prepare();

  for (int value = 11; value < 16; ++value) {
    take(model.update(rows[0].id, value));
  }

  const auto identity = snapshot(fixture, model);
  const auto originalAnchor =
    take(ModelBookmark::create(resource, {PositionKind::ITEM, rows[2].id, 2, before->revision()}));
  const auto identityResolver = take(FinitePositionResolver<int>::create(resource, resource.identity, identity));
  const auto resolvedAnchor = take(identityResolver.resolve({resource.identity, identity->revision(), 1, originalAnchor}));
  CHECK(resolvedAnchor.state == PositionResolution::EXACT);
  CHECK(resolvedAnchor.position.row == rows[2].id);
  const auto identityProof = take(identity->positionHistory()->changesFrom(before->revision()));
  CHECK(identityProof.state == PositionResolution::EXACT);
  CHECK(identityProof.proofFloor == before->revision());
  const auto gap = take(identityProof.changes.map({PositionKind::GAP, {}, 1, before->revision()}));
  CHECK(gap.offset == 1);
  CHECK_FALSE(gap.removed);
  fixture.prepare();
  CHECK(take(take(context.current()).snapshot())->value.row == rows[0].id);
  const auto selection = take(take(context.selection()).snapshot())->value;
  REQUIRE(selection.rows().size() == 2);
  CHECK(selection.rows()[0] == rows[0].id);
  CHECK(selection.rows()[1] == rows[2].id);
  REQUIRE(take(context.readingBookmark()));
  CHECK(take(context.readingBookmark())->position().row == rows[2].id);
  CHECK(selection.resolution() == PositionResolution::EXACT);

  take(model.erase(1));
  const auto afterErase = snapshot(fixture, model);
  const auto gapRevision = afterErase->revision();

  for (int value = 16; value < 21; ++value) {
    take(model.update(rows[0].id, value));
  }

  auto current = snapshot(fixture, model);
  auto resolver = take(FinitePositionResolver<int>::create(resource, resource.identity, current));
  CHECK(take(resolver.resolve({resource.identity, current->revision(), 1, deleted})).state ==
        PositionResolution::NEAREST);
  take(model.move(0, 1, 1));
  take(model.move(0, 1, 1));
  current = snapshot(fixture, model);
  CHECK(take(current->positionHistory()->changesFrom(before->revision())).state == PositionResolution::UNAVAILABLE);
  const auto retained = take(current->positionHistory()->changesFrom(gapRevision));
  CHECK(retained.state == PositionResolution::EXACT);
  CHECK(retained.proofFloor == gapRevision);
  const auto mapped = take(retained.changes.map({PositionKind::GAP, {}, 1, gapRevision}));
  CHECK_FALSE(mapped.removed);
}

struct EqualityValue {
  int number;
  std::shared_ptr<LifetimeHookState> state;

  bool operator==(const EqualityValue &other) const {
    if (state->onCopy) {
      auto action = std::move(state->onCopy);
      action();
    }

    return number == other.number;
  }
};

TEST_CASE("owned update equality runs under the mutation lifetime and reentry boundary") {
  Fixture fixture;
  auto state = std::make_shared<LifetimeHookState>();
  auto model = ui::model(std::array{EqualityValue{1, state}});
  take(ui::mount(*fixture.scene, ui::virtualList(model, &EqualityValue::number)));
  const auto before = snapshot(fixture, model);
  const auto id = before->read({0, 1}).rows[0].id;
  const EqualityValue replacement{2, state};

  SUBCASE("equality failure rejects before publication") {
    state->onCopy = [] {
      throw std::runtime_error("comparison failed");
    };
    const auto rejected = model.update(id, replacement);
    REQUIRE_FALSE(rejected);
    CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::CALLBACK_FAILED);
    CHECK(snapshot(fixture, model) == before);
  }

  SUBCASE("nested mutation is rejected and the outer update succeeds") {
    state->onCopy = [&] {
      const auto nested = model.update(id, replacement);
      REQUIRE_FALSE(nested);
      CHECK(std::get<TreeError>(nested.error().cause) == TreeError::WOULD_BLOCK);
    };
    take(model.update(id, replacement));
    CHECK(snapshot(fixture, model)->revision() == before->revision() + 1);
  }

  SUBCASE("scene close also wins over an equality no-op") {
    state->onCopy = [&] {
      take(fixture.scene->close());
    };
    const auto rejected = model.update(id, EqualityValue{1, state});
    REQUIRE_FALSE(rejected);
    CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::CLOSED);
    CHECK(before->read({0, 1}).rows[0].value.number == 1);
  }
}

TEST_CASE("owned sequence admission and no-ops preserve its installed revision") {
  Fixture fixture;
  auto model = ui::model(std::array{1, 2}, {.maxRows = 2});
  take(ui::mount(*fixture.scene, ui::virtualList(model)));
  const auto before = snapshot(fixture, model);
  const auto full = model.insert(2, 3);
  REQUIRE_FALSE(full);
  CHECK(std::get<TreeError>(full.error().cause) == TreeError::NODE_LIMIT);
  CHECK(snapshot(fixture, model) == before);
  CHECK_FALSE(model.erase(1, 2));
  CHECK_FALSE(model.move(0, 3, 0));
  take(model.move(0, 1, 0));
  take(model.erase(2, 0));
  CHECK(snapshot(fixture, model) == before);
  CHECK_FALSE(take(model.managed()).publisher());
  take(fixture.scene->close());
  const auto closed = model.clear();
  REQUIRE_FALSE(closed);
  CHECK(std::get<TreeError>(closed.error().cause) == TreeError::CLOSED);
}

TEST_CASE("rolling proof retention keeps the source usable and distinguishes old missing IDs") {
  Fixture fixture;
  auto model = ui::model(std::array{10, 20, 30});
  auto navigation = ui::navigation(model);
  take(ui::mount(*fixture.scene, ui::virtualList(navigation)));
  const auto first = snapshot(fixture, model);
  const auto resource = take(take(take(navigation.managed()).current()).snapshot())->value.resource;
  const auto ids = first->read({0, 3}).rows;
  auto alive = take(ModelBookmark::create(resource, {PositionKind::ITEM, ids[0].id, 0, first->revision()}));
  auto deleted = take(ModelBookmark::create(resource, {PositionKind::ITEM, ids[1].id, 1, first->revision()}));
  take(model.erase(1));
  for (std::size_t edit = 0; edit < PositionMapOptions{}.maxChanges + 1; ++edit) {
    take(model.move(0, 1, 1));
  }
  auto current = snapshot(fixture, model);
  auto resolver = take(FinitePositionResolver<int>::create(resource, resource.identity, current));
  CHECK(take(resolver.resolve({resource.identity, current->revision(), 1, alive})).state == PositionResolution::EXACT);
  CHECK(take(resolver.resolve({resource.identity, current->revision(), 2, deleted})).state ==
        PositionResolution::UNAVAILABLE);
  const auto history = take(current->positionHistory()->changesFrom(first->revision()));
  CHECK(history.state == PositionResolution::UNAVAILABLE);
  CHECK(history.proofFloor > first->revision());
  const auto present = current->read({0, 2}).rows;
  const auto recent =
    take(ModelBookmark::create(resource, {PositionKind::ITEM, present[1].id, 1, current->revision()}));
  const auto neighbor = present[0].id;
  take(model.erase(1));
  current = snapshot(fixture, model);
  resolver = take(FinitePositionResolver<int>::create(resource, resource.identity, current));
  const auto resolved = take(resolver.resolve({resource.identity, current->revision(), 3, recent}));
  CHECK(resolved.state == PositionResolution::NEAREST);
  CHECK(resolved.position.row == neighbor);
}

TEST_CASE("owned inactive edits restore selection deletion with one qualified batch proof") {
  Fixture fixture;
  auto a = ui::model(std::array{10, 20, 30, 40});
  auto b = ui::model(std::array{100, 200});
  auto navigation = ui::navigation(a);
  ui::Ref<VirtualList<int>> list;
  take(ui::mount(*fixture.scene, ui::virtualList(navigation).reference(list)));
  const auto first = snapshot(fixture, a);
  const auto original = first->read({0, 4}).rows;
  auto context = take(navigation.managed());
  take(context.navigate(original[1].id));
  const std::array selected{original[0].id, original[1].id, original[2].id};
  take(context.setSelection(selected));
  fixture.prepare();
  take(navigation.setModel(b));
  take(a.insert(0, 9));
  take(a.erase(3));
  take(navigation.setModel(a));
  fixture.prepare();
  const auto restored = take(take(context.selection()).snapshot())->value;
  CHECK(restored.resolution() == PositionResolution::EXACT);
  REQUIRE(restored.rows().size() == 2);
  CHECK(restored.rows()[0] == selected[0]);
  CHECK(restored.rows()[1] == selected[1]);
  CHECK(take(list.restoreStatus()) == PositionResolution::EXACT);
}

TEST_CASE("expired deletion proof remains observable until a descriptor RESET restores the active view") {
  Fixture fixture;
  auto a = ui::model(std::array{1, 2, 3}, CollectionLimits{}, ModelOptions{}, PositionMapOptions{.maxChanges = 2});
  auto b = ui::model(std::array{10, 20});
  auto navigation = ui::navigation(a);
  ui::Ref<VirtualList<int>> list;
  take(ui::mount(*fixture.scene, ui::virtualList(navigation).reference(list)));
  const auto first = snapshot(fixture, a);
  take(take(navigation.managed()).navigate(first->read({1, 1}).rows[0].id));
  fixture.prepare();
  take(navigation.setModel(b));
  take(a.erase(1));
  take(a.move(0, 1, 1));
  take(a.move(0, 1, 1));
  take(navigation.setModel(a));
  fixture.prepare();
  CHECK(take(list.restoreStatus()) == PositionResolution::UNAVAILABLE);
  auto context = take(navigation.managed());
  const auto retained = take(context.bookmark());
  REQUIRE(retained);
  CHECK(retained->position().row == first->read({1, 1}).rows[0].id);
  take(list.setModel(a, ModelRebindOptions{.restore = ModelRestorePolicy::RESET}));
  fixture.prepare();
  CHECK_FALSE(take(list.restoreStatus()));
  CHECK_FALSE(take(context.bookmark()));
}

TEST_CASE("Ref descriptor setters share checked admission and report a collected foreign target once") {
  Fixture fixture;
  Fixture foreign;
  auto a = ui::model(std::array{1, 2}, CollectionLimits{}, ModelOptions{.max_consumers = 1});
  auto b = ui::model(std::array{10, 20});
  auto wrong = ui::model(std::array{100});
  ui::Ref<VirtualList<int>> list;
  take(ui::mount(*fixture.scene, ui::virtualList(a).reference(list)));
  take(ui::mount(*foreign.scene, ui::virtualList(wrong)));
  take(list.setModel(a));
  CHECK(list.model(b));
  CHECK_FALSE(list.model(wrong));
  const auto diagnostics = take(fixture.scene->diagnostics());
  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(std::get<TreeError>(diagnostics.errors.front().error.cause) == TreeError::WRONG_SESSION);
  fixture.prepare();
  take(fixture.app->tree().read(list.node(), [](const auto &view) {
    CHECK(view.dataStatus().rowCount == 2);
  }));
}

struct CopyValue {
  CopyValue(int number, std::shared_ptr<LifetimeHookState> hooks) : value(number), state(std::move(hooks)) {
  }

  CopyValue(const CopyValue &other) : value(other.value), state(other.state) {
    if (state->onCopy) {
      auto action = std::move(state->onCopy);
      action();
    }
  }

  CopyValue(CopyValue &&) noexcept = default;

  ~CopyValue() {
    if (state && state->onDestroy) {
      auto action = std::move(state->onDestroy);
      action();
    }
  }
  int value;
  std::shared_ptr<LifetimeHookState> state;
};

TEST_CASE("collected row failure keeps its original scene after a copy rebinds the descriptor") {
  Fixture fixture;
  auto state = std::make_shared<LifetimeHookState>();
  auto model = ui::model(std::array{CopyValue(1, state)});
  take(ui::mount(*fixture.scene, ui::virtualList(model, &CopyValue::value)));
  const auto retainedModel = model;
  const auto before = snapshot(fixture, retainedModel);
  const auto id = before->read({0, 1}).rows[0].id;
  const CopyValue replacement(2, state);
  state->onCopy = [&] {
    model = {};
    throw std::runtime_error("copy rebound its descriptor");
  };
  CHECK_FALSE(model.row(id, replacement));
  CHECK(snapshot(fixture, retainedModel) == before);
  const auto diagnostics = take(fixture.scene->diagnostics());
  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(std::get<TreeError>(diagnostics.errors[0].error.cause) == TreeError::CALLBACK_FAILED);
}

TEST_CASE("a mutation copy allocation failure retains the previous immutable source") {
  Fixture fixture;
  auto state = std::make_shared<LifetimeHookState>();
  auto model = ui::model(std::array{CopyValue(1, state), CopyValue(2, state)});
  take(ui::mount(*fixture.scene, ui::virtualList(model, &CopyValue::value)));
  const auto before = snapshot(fixture, model);
  state->onCopy = [] {
    throw std::bad_alloc();
  };
  const CopyValue extra(3, state);
  std::expected<void, ManagedError> rejected;

  SUBCASE("insert copy") {
    rejected = model.insert(0, extra);
  }

  SUBCASE("replacement copy") {
    rejected = model.update(before->read({0, 1}).rows[0].id, extra);
  }
  REQUIRE_FALSE(rejected);
  CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::NO_MEMORY);
  CHECK(snapshot(fixture, model) == before);
  take(model.insert(0, extra));
  const auto after = snapshot(fixture, model);
  CHECK(after->revision() == before->revision() + 1);
  CHECK(after->read({0, 1}).rows[0].value.value == 3);
}

TEST_CASE("owned replacement row copies reject nested mutation before publication") {
  Fixture fixture;
  auto state = std::make_shared<LifetimeHookState>();
  auto model = ui::model(std::array{CopyValue(1, state)});
  take(ui::mount(*fixture.scene, ui::virtualList(model, &CopyValue::value)));
  const auto before = snapshot(fixture, model);
  const auto id = before->read({0, 1}).rows[0].id;
  const CopyValue replacement(2, state);
  state->onCopy = [&] {
    const auto nested = model.update(id, replacement);
    REQUIRE_FALSE(nested);
    CHECK(std::get<TreeError>(nested.error().cause) == TreeError::WOULD_BLOCK);
  };
  take(model.update(id, replacement));
  CHECK(snapshot(fixture, model)->read({0, 1}).rows[0].value.value == 2);
}

TEST_CASE("replacement staging destruction observes committed owner metadata before reentry") {
  Fixture fixture;
  auto state = std::make_shared<LifetimeHookState>();
  auto model = ui::model(std::array{CopyValue(1, state)});
  take(ui::mount(*fixture.scene, ui::virtualList(model, &CopyValue::value)));
  const auto before = snapshot(fixture, model);
  const auto id = before->read({0, 1}).rows[0].id;
  const CopyValue replacement(2, state);
  const CopyValue nested(3, state);
  bool called{};
  state->onDestroy = [&] {
    called = true;
    take(model.update(id, nested));
  };
  take(model.update(id, replacement));
  CHECK(called);
  const auto after = snapshot(fixture, model);
  CHECK(after->revision() == before->revision() + 2);
  CHECK(after->read({0, 1}).rows[0].value.value == 3);
  CHECK(before->read({0, 1}).rows[0].value.value == 1);
}
TEST_CASE("a mutation row copy may destroy the scene before publication") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto state = std::make_shared<LifetimeHookState>();
  auto model = ui::model(std::array{CopyValue(1, state)});
  take(ui::mount(*scene, ui::virtualList(model, &CopyValue::value)));
  take(scene->prepare(*app, {40, 12}));
  const auto retained = take(portals::detail::ManagedModelAccess::snapshot(take(model.managed()), scene->root().id()));
  state->onCopy = [&] {
    scene.reset();
  };
  const CopyValue extra(2, state);
  std::expected<void, ManagedError> rejected;

  SUBCASE("insert copy") {
    rejected = model.insert(0, extra);
  }

  SUBCASE("replacement copy") {
    rejected = model.update(retained->read({0, 1}).rows[0].id, extra);
  }
  REQUIRE_FALSE(rejected);
  CHECK_FALSE(scene);
  CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::CLOSED);
  CHECK(retained->read({0, 1}).rows[0].value.value == 1);
}

TEST_CASE("a large provider keeps noncopyable domain rows and no implicit registration reads") {
  struct MoveOnly {
    MoveOnly() = default;
    MoveOnly(const MoveOnly &) = delete;
  };

  class Provider final : public ModelSnapshot<MoveOnly> {
  public:
    Provider() : ModelSnapshot(1, 1000000) {
    }

    ModelRows<MoveOnly> read(RowRange) const override {
      ++reads;
      return {};
    }

    mutable std::size_t reads{};
  };

  Fixture fixture;
  auto provider = std::make_shared<Provider>();
  auto model = ui::model<MoveOnly>(provider);
  take(ui::mount(*fixture.scene, ui::virtualList(model, [](const MoveOnly &) {
    return 0;
  })));
  CHECK(provider->reads == 0);
  CHECK(model.managed());
  const MoveOnly value;
  for (const auto &rejected : std::array{model.clear(), model.erase(0), model.move(0, 1, 0), model.insert(0, value),
                                         model.update({1, 1}, value)}) {
    REQUIRE_FALSE(rejected);
    CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::INVALID_ARGUMENT);
  }
  CHECK(provider->reads == 0);
  take(fixture.scene->close());
  const auto closed = model.erase(0);
  REQUIRE_FALSE(closed);
  CHECK(std::get<TreeError>(closed.error().cause) == TreeError::CLOSED);
}
} // namespace

namespace {
TEST_CASE("finite history provider cancellation wins over successful history receipts") {
  struct History final : PositionChangeHistory {
    std::stop_source *stop{};
    PositionHistoryReply result;
    bool cancelOnDestruction{};

    ~History() override {
      if (cancelOnDestruction) {
        stop->request_stop();
      }
    }

    std::expected<PositionHistoryReply, SourceError> changesFrom(std::uint64_t) const override {
      if (!cancelOnDestruction) {
        stop->request_stop();
      }
      return result;
    }
  };

  struct Provider final : ModelSnapshot<int> {
    explicit Provider(std::shared_ptr<History> value) : ModelSnapshot<int>(2, 1), history(std::move(value)) {
    }

    ModelRows<int> read(RowRange) const override {
      return {};
    }

    RowLookup locateRow(RowId) const override {
      return {RowLookupState::NOT_FOUND, 0};
    }

    std::shared_ptr<const PositionChangeHistory> positionHistory() const override {
      return ephemeralHistory ? std::make_shared<History>(*history) : history;
    }

    std::shared_ptr<History> history;
    bool ephemeralHistory{};
  };

  std::stop_source stop;
  auto history = std::make_shared<History>();
  history->stop = &stop;
  const auto resource = take(ModelResource::create());
  auto snapshot = std::make_shared<Provider>(history);
  auto resolver = take(FinitePositionResolver<int>::create(resource, resource.identity, snapshot));
  ModelPosition position{PositionKind::ITEM, {1, 1}, 0, 1, PositionAffinity::FORWARD};
  SUBCASE("gap receives a valid exact map") {
    position.kind = PositionKind::GAP;
    position.row = {};
    const std::array operations{PositionChange::splice(0, 1, 1)};
    history->result = {take(PositionChangeMap::create(1, 2, 1, operations)), 1, PositionResolution::EXACT};
  }
  SUBCASE("missing item receives unavailable history") {
    history->result = {{}, 2, PositionResolution::UNAVAILABLE};
  }
  SUBCASE("history owner destruction cancels before an unsupported receipt") {
    snapshot->ephemeralHistory = true;
    history->cancelOnDestruction = true;
  }
  const auto bookmark = take(ModelBookmark::create(resource, position));
  const auto result = resolver.resolve({resource.identity, 2, 1, bookmark}, stop.get_token());
  CHECK(stop.stop_requested());
  REQUIRE_FALSE(result);
  CHECK(result.error() == SourceError::CANCELLED);
}
} // namespace

namespace {
TEST_CASE("cold provider table configs own arrays and preserve independent preparation overrides") {
  class Provider final : public ModelSnapshot<int> {
  public:
    Provider() : ModelSnapshot(1, 3) {
    }

    ModelRows<int> read(RowRange range) const override {
      ++reads;
      return {RangeState::READY, std::span(rows).subspan(static_cast<std::size_t>(range.first), range.count), {}};
    }

    std::array<ModelRow<int>, 3> rows{{{RowId{1, 1}, 11}, {RowId{2, 1}, 22}, {RowId{3, 1}, 33}}};
    mutable std::size_t reads{};
  };

  Fixture fixture({.maxModels = 1});
  auto provider = std::make_shared<Provider>();
  std::size_t defaultCalls{}, overrideCalls{};
  auto model = ui::model<int>(provider, {}, [&](const auto &frame, const auto &) -> std::expected<void, TreeError> {
    CHECK(frame.dataRevision() == 1);
    ++defaultCalls;
    return {};
  });
  ui::Ref<VirtualTable<int>> bid, ask;
  std::vector<std::string> formattedBid, formattedAsk;
  auto body = [&] {
    const auto column = [](std::string label, std::vector<std::string> &formatted) {
      return TableColumn<int>{
        {1, 1}, take(GraphemeText::from(label)), 8, [label = std::move(label), &formatted](const int &value) {
          auto result = label + std::to_string(value);
          formatted.push_back(result);
          return take(GraphemeText::from(result));
        }};
    };
    std::array bidColumns{column("BID", formattedBid)};
    std::array askColumns{column("ASK", formattedAsk)};
    ui::VirtualTableConfig<int> bidConfig;
    bidConfig.options.row_height = 2;
    bidConfig.options.overscan_rows = 0;
    bidConfig.policy.height = {.preferred = 5, .mode = SizeMode::FIXED};
    bidConfig.prepared = [&](const auto &frame, const auto &) -> std::expected<void, TreeError> {
      CHECK(frame.dataRevision() == 1);
      ++overrideCalls;
      return {};
    };
    ui::VirtualTableConfig<int> askConfig;
    askConfig.options.overscan_rows = 0;
    askConfig.policy.height = {.preferred = 5, .mode = SizeMode::FIXED};
    return ui::column(ui::virtualTable(bid, model, bidColumns, bidConfig),
                      ui::virtualTable(model, askColumns, std::move(askConfig)).reference(ask));
  }();
  CHECK(provider->reads == 0);
  take(ui::mount(*fixture.scene, std::move(body)));
  CHECK(provider->reads == 0);
  CHECK(defaultCalls == 0);
  CHECK(overrideCalls == 0);
  CHECK(bid.node().id() != NodeId{});
  CHECK(ask.node().id() != NodeId{});
  fixture.prepare();
  CHECK(defaultCalls > 0);
  CHECK(overrideCalls > 0);
  REQUIRE_FALSE(formattedBid.empty());
  REQUIRE_FALSE(formattedAsk.empty());
  CHECK(formattedBid.front() == "BID11");
  CHECK(formattedAsk.front() == "ASK11");
  take(setVirtualTableSelection(fixture.app->tree(), bid.node(), RowId{2, 1}));
  take(setVirtualTableSelection(fixture.app->tree(), ask.node(), RowId{3, 1}));
  fixture.prepare();
  take(fixture.app->tree().read(bid.node(), [](const auto &view) {
    CHECK(view.selection() == RowId{2, 1});
    CHECK(view.visibleRange().count == 2);
    CHECK(view.sizePolicy().height.preferred == 5);
  }));
  take(fixture.app->tree().read(ask.node(), [](const auto &view) {
    CHECK(view.selection() == RowId{3, 1});
    CHECK(view.visibleRange().count == 3);
    CHECK(view.sizePolicy().height.preferred == 5);
  }));
}
} // namespace
