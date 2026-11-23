// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_model_edit.hpp>

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using namespace portals;

struct City {
  std::string name;
  int population;
  bool operator==(const City &) const = default;
};

template <class T, class E> T checked(std::expected<T, E> result) {
  if (!result) {
    throw std::runtime_error("Unexpected checked edit failure");
  }
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}
} // namespace

int main() try {
  auto app = checked(Application::create());
  auto scene = checked(ManagedScene::create(*app));
  auto cities = ui::model(std::array{City{"Prague", 100}, City{"Vienna", 200}});
  auto navigation = ui::navigation(cities);
  auto edit = ui::edit(navigation, {.mode = EditMode::EXPLICIT});
  auto name = edit.field("city.name", &City::name);
  auto population = edit.field("city.population", &City::population);

  checked(ui::mount(*scene, ui::virtualList(navigation, &City::name)));
  auto managed = checked(navigation.managed());
  checked(managed.navigate({1, 1}));
  const auto target = checked(name.snapshot()).target;
  require(checked(name.set("Praha")) == EditStatus::DIRTY, "Explicit string edit published early");
  require(checked(population.set(101)) == EditStatus::DIRTY, "Explicit numeric edit published early");
  checked(managed.navigate({2, 1}));
  require(*checked(name.snapshot()).accepted == "Vienna", "Navigation selected the wrong record");
  require(*checked(name.snapshot(target)).draft == "Praha", "Navigation lost the original draft");

  checked(managed.navigate({1, 1}));
  checked(edit.commit().result());
  require(*checked(name.snapshot()).accepted == "Praha", "String field did not commit");
  require(*checked(population.snapshot()).accepted == 101, "Numeric field did not commit");
  checked(name.set("Conflicting draft"));
  checked(cities.update({1, 1}, City{"External", 102}));
  require(!edit.commit().result(), "Stale draft overwrote external data");
  require(*checked(name.snapshot()).draft == "Conflicting draft", "Conflict discarded the draft");
  checked(edit.cancel().result());
  require(*checked(name.snapshot()).accepted == "External", "Cancel did not adopt the accepted record");
  checked(scene->close());
  return 0;
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';
  return 1;
}
