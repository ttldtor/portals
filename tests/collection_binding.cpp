// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/managed_text_fixture.hpp"

namespace {
static_assert(requires(ManagedScene &scene) { scene.model<int>({}); });

RowCollection<int> rows(std::initializer_list<ModelRow<int>> values) {
  return take(RowCollection<int>::from(std::span(values.begin(), values.size())));
}

void identityAndModel() {
  auto first = rows({{{1, 1}, 10}, {{2, 1}, 20}});
  check(first.locate({2, 1}).index == 1 && first.locate({2, 2}).state == RowLookupState::NOT_FOUND,
        "collection index uses complete stable identity");
  const ModelRow<int> duplicate[]{{{1, 1}, 10}, {{1, 1}, 11}};
  check(!RowCollection<int>::from(duplicate), "duplicate full identities rejected");
  Fixture f;
  auto state = take(f.scene->state(first));
  auto source = take(f.scene->derive(state, [](const RowCollection<int> &value) {
    return value;
  }));
  auto model = take(f.scene->model(source));
  auto rejected = model.publisher();
  check(!rejected && rejected.error() == TreeError::WOULD_BLOCK, "bound model is sink-only");
  ui::Ref<VirtualList<int>> list;
  take(ui::mount(*f.scene, ui::virtualList(model, ManagedListFormatter<int>([](const ModelRow<int> &row) {
                                             return take(GraphemeText::from(std::to_string(row.value)));
                                           }))
                             .reference(list)
                             .height(1)));
  take(setVirtualListSelection(f.app->tree(), list.node(), RowId{2, 1}));
  f.prepare();
  prepareViews(f);
  std::optional<RowId> beforeSelection;
  take(f.app->tree().read(list.node(), [&](const VirtualList<int> &value) {
    beforeSelection = value.selection();
  }));
  check(beforeSelection == RowId{2, 1}, "offscreen existing ID is preserved by full index lookup");
  auto reorder = rows({{{2, 1}, 21}, {{1, 1}, 10}});
  take(state.set(reorder));
  f.prepare();
  prepareViews(f);
  take(f.app->tree().read(list.node(), [&](const VirtualList<int> &value) {
    beforeSelection = value.selection();
  }));
  check(beforeSelection == RowId{2, 1}, "stable selection survives reorder and value update");
  check(first.rows()[1].value == 20, "old collection owner remains unchanged");
  take(state.set(rows({{{1, 1}, 10}})));
  f.prepare();
  // View preparation uses a real final viewport and the scene's single model pin.
  std::optional<LayoutSnapshot> layout;
  take(f.app->tree().frame([&] {
    const auto bounds = take(CellRect::from({0, 0}, {30, 4}));
    take(layoutTree(f.app->tree(), take(f.app->tree().root()), bounds, bounds));
    layout = take(LayoutSnapshot::capture(f.app->tree(), f.scene->root()));
  }));
  take(f.scene->prepareViews(*f.app, *layout));
  std::optional<RowId> selected;
  take(f.app->tree().read(list.node(), [&](const VirtualList<int> &value) {
    selected = value.selection();
  }));
  check(!selected, "bound list clears ID missing from full collection");
}

void allocationAndLimits() {
  const ModelRow<int> values[]{{{1, 1}, 10}, {{2, 1}, 20}, {{3, 1}, 30}};
  auto tooMany = RowCollection<int>::from(values, CollectionLimits{2});
  check(!tooMany && tooMany.error() == TreeError::NODE_LIMIT, "collection rejects count before copying payload");
  const auto before = allocationTest::count;
  auto initial = take(RowCollection<int>::from(values));
  const auto allocations = allocationTest::count - before;
  for (std::size_t i = 0; i < allocations; ++i) {
    allocationTest::remaining = static_cast<std::ptrdiff_t>(i);
    auto failed = RowCollection<int>::from(values);
    allocationTest::remaining = -1;
    check(!failed && failed.error() == TreeError::NO_MEMORY, "collection owns rows and index atomically under OOM");
  }
  std::size_t modelAllocations{};
  {
    Fixture f;
    auto state = take(f.scene->state(initial));
    auto source = take(f.scene->derive(state, [](const RowCollection<int> &value) {
      return value;
    }));
    const auto count = allocationTest::count;
    auto model = take(f.scene->model(source));
    modelAllocations = allocationTest::count - count;
  }
  for (std::size_t i = 0; i < modelAllocations; ++i) {
    ManagedOptions limits;
    limits.maxModels = 1;
    Fixture f(limits);
    auto state = take(f.scene->state(initial));
    auto source = take(f.scene->derive(state, [](const RowCollection<int> &value) {
      return value;
    }));
    allocationTest::remaining = static_cast<std::ptrdiff_t>(i);
    auto failed = f.scene->model(source);
    allocationTest::remaining = -1;
    check(!failed && treeCause(failed.error(), TreeError::NO_MEMORY),
          "adapter initial allocation rolls back its Model/endpoint budget");
    auto capped = f.scene->model(source, CollectionModelOptions{.maxRows = 2});
    check(!capped && treeCause(capped.error(), TreeError::NODE_LIMIT),
          "initial model cap rejects without consuming its budget");
    auto valid = take(f.scene->model(source));
    check(!valid.publisher() && valid.publisher().error() == TreeError::WOULD_BLOCK,
          "successful retry remains sink-only");
    f.prepare();
  }
  std::cout << "collection allocation points: " << allocations << " model initial points: " << modelAllocations << '\n';
}
} // namespace

int main() {
  try {
    identityAndModel();
    allocationAndLimits();
  } catch (const std::exception &error) {
    allocationTest::remaining = -1;
    std::cerr << error.what() << '\n';
    return 1;
  }
}
