// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/table_column.hpp>
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

#include <array>
#include <expected>
#include <string>

using namespace portals;

struct Stop {
  std::string city;
  int nights;

  bool operator==(const Stop &) const = default;
};

int main() {
  auto stops = ui::model(std::array{Stop{"Prague", 2}, Stop{"Vienna", 3}, Stop{"Berlin", 1},
                                  Stop{"Paris", 4}, Stop{"Rome", 2}, Stop{"Oslo", 1}});
  auto navigation = ui::navigation(stops, {.readingAnchor = true});
  auto replace = [&](const Stop &stop) -> std::expected<void, ManagedError> {
    auto current = navigation.currentRow();

    if (!current) {
      return std::unexpected(current.error());
    }

    return *current ? stops.update(**current, stop) : std::expected<void, ManagedError>{};
  };

  // clang-format off
  return ui::scene(ui::vbox(LinearOptions{.gap = 1},
    ui::hbox(
      ui::virtualList(navigation, &Stop::city).width(22).framed("rounded", "Stops"),
      ui::virtualTable(navigation,
        tableColumn("City", &Stop::city, 18),
        tableColumn("Nights", &Stop::nights, 8))
          .framed("rounded", "Stay"))
        .height(7),
    ui::hbox(LinearOptions{.gap = 2},
      ui::button("Prague: 2 nights", [&] { return replace(Stop{"Prague", 2}); }),
      ui::button("Vienna: 3 nights", [&] { return replace(Stop{"Vienna", 3}); })),
    ui::label("Choose a stop, then replace its destination. Both views keep their place."))
      .maxWidth(76).fitHeight().framed("rounded", "Itinerary"))
    | ui::theme(nordTheme())
    | ui::onKey("F2", [&] { return replace(Stop{"Prague", 2}); })
    | ui::onKey("F3", [&] { return replace(Stop{"Vienna", 3}); })
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
