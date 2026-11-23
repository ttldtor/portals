// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/table_column.hpp>
#include <portals/ui.hpp>

#include <array>
#include <string_view>

using namespace portals;

struct City {
  std::string_view name;
  int orders;
};

int main() {
  auto europe = ui::model(std::array{City{"Prague", 3}, City{"Vienna", 6}, City{"Berlin", 9}, City{"Paris", 12},
                                     City{"Rome", 15}, City{"Oslo", 18}, City{"Madrid", 21}, City{"Lisbon", 24},
                                     City{"Warsaw", 27}, City{"Riga", 30}, City{"Tallinn", 33}, City{"Helsinki", 36}});
  auto asia = ui::model(std::array{City{"Tokyo", 3}, City{"Seoul", 6}, City{"Osaka", 9}, City{"Taipei", 12},
                                   City{"Kyoto", 15}, City{"Busan", 18}, City{"Nagoya", 21}, City{"Kobe", 24},
                                   City{"Sendai", 27}, City{"Nara", 30}, City{"Fukuoka", 33}, City{"Sapporo", 36}});
  auto navigation = ui::navigation(europe, {.readingAnchor = true});
  ui::Ref<Label> region;

  // clang-format off
  return ui::scene(ui::vbox(
    ui::label(region, navigation.current(), ui::bySource<City>({{europe, "Europe"}, {asia, "Asia"}})),
    ui::hbox(
      ui::virtualList(navigation, &City::name).width(22).framed("rounded", "Cities"),
      ui::virtualTable(navigation,
        tableColumn("City", &City::name, 18),
        tableColumn("Orders", &City::orders, 8))
          .framed("rounded", "Orders"))
        .height(8),
    ui::label("F2: Europe | F3: Asia | F10: quit | both models remember their own place")))
    | ui::theme(nordTheme())
    | ui::onKey("F2 | F3", [&](const KeyEvent &event) {
        const bool europeSelected = event.key == InputKey::F2;
        navigation.model(europeSelected ? europe : asia);
      })
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
