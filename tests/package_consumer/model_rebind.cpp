// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <array>
#include <iostream>

bool modelRebindHeaderProbe();

class Numbers final : public portals::ModelSnapshot<int> {
public:
  explicit Numbers(int base) : ModelSnapshot(1, 3), rows_{{{{1, 1}, base}, {{2, 1}, base + 1}, {{3, 1}, base + 2}}} {
  }

  portals::ModelRows<int> read(portals::RowRange range) const override {
    return {
      portals::RangeState::READY, std::span(rows_).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }

  portals::RowLookup locateRow(portals::RowId id) const override {
    return id.generation == 1 && id.value && id.value <= rows_.size()
             ? portals::RowLookup{portals::RowLookupState::FOUND, id.value - 1}
             : portals::RowLookup{portals::RowLookupState::NOT_FOUND, 0};
  }

private:
  std::array<portals::ModelRow<int>, 3> rows_;
};

int main() {
  using namespace portals;
  auto app = Application::create();
  if (!app || !modelRebindHeaderProbe()) {
    return 1;
  }
  auto scene = ManagedScene::create(**app);
  if (!scene) {
    return 2;
  }
  auto a = (*scene)->model<int>(std::make_shared<Numbers>(10));
  auto b = (*scene)->model<int>(std::make_shared<Numbers>(20));
  if (!a || !b) {
    return 3;
  }
  ui::Ref<VirtualList<int>> unopened;
  if (unopened.setModel(*a, {.restore = ModelRestorePolicy::RESET})) {
    return 4;
  }
  auto navigation = (*scene)->navigation(*a);
  if (!navigation) {
    std::cerr << "navigation creation failed\n";
    return 5;
  }
  if (!(*scene)->prepare(**app, {80, 24})) {
    std::cerr << "initial preparation failed\n";
    return 51;
  }
  auto moved = navigation->navigate({2, 1});
  if (!moved) {
    std::cerr << "initial navigation failed: " << static_cast<int>(moved.error()) << '\n';
    return 52;
  }
  if (!navigation->setModel(*b) || !(*scene)->prepare(**app, {80, 24}) || !navigation->navigate({3, 1}) ||
      !navigation->setModel(*a)) {
    return 6;
  }
  auto current = navigation->current();
  if (!current) {
    return 7;
  }
  auto position = current->snapshot();
  if (!position || (*position)->value.row != RowId{2, 1} ||
      (*position)->value.resolution != PositionResolution::EXACT) {
    return 8;
  }
  return (*scene)->close() ? 0 : 9;
}
