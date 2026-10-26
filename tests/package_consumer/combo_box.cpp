// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/background.hpp>
#include <portals/combo_box.hpp>
#include <portals/ui_combo_box.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <thread>

namespace {
struct City {
  std::string name;
};

class Cities final : public portals::ModelSnapshot<City> {
public:
  Cities() : ModelSnapshot(1, 2) {
  }

  portals::ModelRows<City> read(portals::RowRange range) const override {
    return {
      portals::RangeState::READY, std::span(rows_).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }

private:
  std::array<portals::ModelRow<City>, 2> rows_{{{{1, 1}, {"Prague"}}, {{2, 1}, {"Rome"}}}};
};
} // namespace

int main() {
  using namespace portals;
  ui::Ref<ComboBox> city;
  ui::Ref<DropDown> delivery;
  std::shared_ptr<const ModelSnapshot<City>> cities = std::make_shared<const Cities>();
  auto scene =
    ui::scene(ui::vbox(ui::comboBox(city, cities, &City::name), ui::dropDown(delivery, {"Courier", "Pickup"}))) |
    ui::onSelect(city,
                 [](ChoiceSelection) {
                 }) |
    ui::onSelect(city,
                 []() -> std::expected<void, TreeError> {
                   return {};
                 }) |
    ui::onSelect(delivery, ui::quit);
  auto prepared = scene.prepareChecked();
  if (!prepared || city.node() == Node<ComboBox>{} || delivery.node() == Node<DropDown>{}) {
    return 1;
  }
  auto value = city.selection();
  auto opened = city.opened();
  if (!value || *value || !opened || *opened) {
    return 2;
  }

  auto pool = BackgroundPool::create();
  if (!pool) {
    return 3;
  }
  auto app = Application::create(ApplicationOptions{.backgroundExecutor = pool->get()});
  if (!app) {
    return 4;
  }
  auto managed = ManagedScene::create(**app);
  if (!managed) {
    return 5;
  }
  auto model = (*managed)->model<City>(cities);
  if (!model) {
    return 6;
  }
  ui::Ref<ComboBox> modelChoice;
  auto mounted = ui::mount(**managed, ui::comboBox(modelChoice, *model, &City::name));
  if (!mounted) {
    std::cerr << "Model choice mount failed in phase " << static_cast<int>(mounted.error().phase) << '\n';
    return 7;
  }
  auto modelFrame = (*managed)->prepare(**app, CellSize{80, 24});
  if (!modelFrame) {
    std::cerr << "Model frame preparation failed\n";
    return 10;
  }
  auto admitted = modelChoice.select(1);
  if (!admitted) {
    std::cerr << "Model choice selection admission failed\n";
    if (const auto *cause = std::get_if<TreeError>(&admitted.error().cause)) {
      std::cerr << "TreeError=" << static_cast<int>(*cause) << '\n';
    }
    return 7;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    (*pool)->service();
    if (!(*app)->scheduler().dispatch() || !(*managed)->afterDispatch(**app)) {
      return 8;
    }
    auto selected = modelChoice.selection();
    if (selected && *selected && selected->text() == "Rome") {
      (*managed)->close();
      (*pool)->requestStop();
      (*pool)->join();
      return 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return 9;
}
