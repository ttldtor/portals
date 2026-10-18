// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/visible_model.hpp>

#include <array>
#include <cstdlib>
#include <numeric>

using namespace portals;

template <class T, class E> T checked(std::expected<T, E> value) {
  if (!value) {
    std::exit(EXIT_FAILURE);
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

class Values final : public ModelSnapshot<int> {
public:
  Values() : ModelSnapshot(1, 4) {
  }

  ModelRows<int> read(RowRange range) const override {
    return {RangeState::READY, std::span(values_).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }

private:
  const std::array<ModelRow<int>, 4> values_{{{{1, 1}, 10}, {{2, 1}, 20}, {{3, 1}, 30}, {{4, 1}, 40}}};
};

int main() {
  auto app = checked(Application::create());
  auto scene = checked(ManagedScene::create(*app));
  auto model = checked(scene->model<int>(std::make_shared<Values>()));
  ui::Ref<VirtualTable<int>> table;
  auto visible = table.visibleModel();

  if (visible.snapshot()) {
    return EXIT_FAILURE;
  }

  const std::array columns{TableColumn<int>{
    .id = {1, 1}, .header = checked(GraphemeText::from("Value")), .width = 8, .format = [](const int &) {
      return checked(GraphemeText::from("value"));
    }}};

  checked(ui::mount(*scene, ui::virtualTable<int>(table, model, columns)));
  const CellSize size{20, 3};
  const auto bounds = checked(CellRect::from({}, size));

  checked(scene->prepare(*app, size));
  std::optional<LayoutSnapshot> layout;

  checked(app->tree().frame([&] {
    checked(layoutTree(app->tree(), app->root(), bounds, bounds));
    layout.emplace(checked(LayoutSnapshot::capture(app->tree(), app->root())));
  }));
  checked(scene->prepareViews(*app, *layout));
  checked(app->tree().frame([&] {
    checked(layoutTree(app->tree(), app->root(), bounds, bounds));
  }));
  checked(scene->completedLayout(*app));

  const auto rows = checked(visible.snapshot());

  if (rows.size() != 2 || std::accumulate(rows.begin(), rows.end(), 0) != 30) {
    return EXIT_FAILURE;
  }

  checked(scene->remove(table.node()));
  scene.reset();

  return rows.rows().front().id == RowId{1, 1} && *rows.begin() == 10 && !visible.snapshot() ? EXIT_SUCCESS
                                                                                             : EXIT_FAILURE;
}
