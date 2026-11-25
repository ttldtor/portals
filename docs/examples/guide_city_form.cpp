// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/table_column.hpp>
#include <portals/ui.hpp>
#include <portals/ui_model_form.hpp>
#include <portals/ui_style.hpp>

#include <array>
#include <format>
#include <string>

using namespace portals;

enum class Region {
  EUROPE,
  ASIA
};

struct City {
  std::string name;
  int orders{};
  std::string country;

  bool operator==(const City &) const = default;
};

int main() {
  auto europe =
    ui::model(std::array{City{"Prague", 12, "Czechia"}, City{"Vienna", 25, "Austria"}, City{"Riga", 18, "Latvia"},
                         City{"Tallinn", 9, "Estonia"}, City{"Oslo", 31, "Norway"}, City{"Berlin", 45, "Germany"}});
  auto asia =
    ui::model(std::array{City{"Tokyo", 48, "Japan"}, City{"Seoul", 36, "South Korea"}, City{"Taipei", 27, "Taiwan"},
                         City{"Kyoto", 15, "Japan"}, City{"Osaka", 22, "Japan"}, City{"Busan", 14, "South Korea"}});
  auto navigation = ui::navigation(europe, {.readingAnchor = true});
  auto edit = ui::edit(navigation, {.mode = EditMode::EXPLICIT});
  auto russian = ui::model(false);
  auto region = ui::bySource<City, Region>({{europe, Region::EUROPE}, {asia, Region::ASIA}});
  auto regionCaption = [region](const NavigationPosition &position) {
    return region(position).transform([](Region value) {
      return value == Region::EUROPE ? std::string("Europe") : std::string("Asia");
    });
  };
  auto translate = [](std::string_view key, bool inRussian) -> std::expected<std::string, TextResourceError> {
    if (key != "orders") {
      return std::unexpected(TextResourceError::MISSING_RESOURCE);
    }

    return inRussian ? "Заказы" : "Orders";
  };

  // clang-format off
  return ui::scene(ui::vbox(LinearOptions{.gap = 1},
    ui::label(navigation.current(), regionCaption),
    ui::hbox(LinearOptions{.gap = 2},
      ui::vbox(LinearOptions{.gap = 1},
        ui::virtualList(navigation, &City::name)
          .height(6).framed("rounded", "Cities"),
        ui::virtualTable(navigation,
          tableColumn("City", &City::name, 16),
          tableColumn("Orders", &City::orders, 7))
            .height(7).framed("rounded", "Accepted values"))
          .width(28),
      ui::form(edit,
        ui::field("City", &City::name).required(),
        ui::field(text(i18n("orders"), translate, russian), &City::orders)
          .validate([](int count) { return count >= 0; }),
        ui::field("Country (read only)", &City::country).readOnly(),
        ui::hbox(LinearOptions{.gap = 2},
          ui::button("Save", [edit] { return edit.commit(); }),
          ui::button("Cancel", [edit] { return edit.cancel(); })),
        ui::hbox(LinearOptions{.gap = 2},
          ui::button("Europe", [navigation, europe] { navigation.model(europe); }),
          ui::button("Asia", [navigation, asia] { navigation.model(asia); })))
        .minWidth(36).framed("rounded", "Current city draft"))
      .height(17),
    ui::label("F2: save | F3: cancel | F4: English | F5: Russian | F6/F7: source | F10: quit"),
    ui::label("Choose a city to edit. Save publishes its draft; switching keeps it with that city."),
    ui::label(navigation.current(), [regionCaption](const NavigationPosition &position) {
      return regionCaption(position).transform([](const std::string &name) {
        return std::format("{} keeps its current city and reading place.", name);
      });
    }))
      .maxWidth(88).fitHeight().framed("rounded", "City orders workspace"))
    | ui::theme(nordTheme())
    | ui::onKey("F2", [edit] { return edit.commit(); })
    | ui::onKey("F3", [edit] { return edit.cancel(); })
    | ui::onKey("F4", [russian]() mutable { russian.value() = false; })
    | ui::onKey("F5", [russian]() mutable { russian.value() = true; })
    | ui::onKey("F6", [navigation, europe] { navigation.model(europe); })
    | ui::onKey("F7", [navigation, asia] { navigation.model(asia); })
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
