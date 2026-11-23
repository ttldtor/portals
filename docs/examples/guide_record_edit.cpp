// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_model_edit.hpp>
#include <portals/ui_style.hpp>

#include <array>
#include <string>

using namespace portals;

struct City {
  std::string name;
  std::string country;

  bool operator==(const City &) const = default;
};

int main() {
  auto cities = ui::model(std::array{
    City{"Prague", "Czechia"}, City{"Vienna", "Austria"}, City{"Riga", "Latvia"},
    City{"Tallinn", "Estonia"}, City{"Oslo", "Norway"}, City{"Berlin", "Germany"}
  });
  auto navigation = ui::navigation(cities);
  auto edit = ui::edit(navigation, {.mode = EditMode::EXPLICIT});

  // clang-format off
  return ui::scene(ui::vbox(LinearOptions{.gap = 1},
    ui::hbox(LinearOptions{.gap = 2},
      ui::virtualList(navigation, &City::name)
        .width(20).framed("rounded", "Cities"),
      ui::vbox(LinearOptions{.gap = 1},
        ui::formField("City", edit.field(&City::name), {.required = true}),
        ui::formField("Country", edit.field(&City::country)),
        ui::hbox(LinearOptions{.gap = 2},
          ui::button("Save", [edit] { return edit.commit(); }),
          ui::button("Cancel", [edit] { return edit.cancel(); })))
        .minWidth(32).framed("rounded", "Edit city"))
      .height(11),
    ui::label("Tab: fields | Up/Down: cities | F2: save | F3: cancel | F10: quit"))
      .maxWidth(74).fitHeight())
    | ui::theme(nordTheme())
    | ui::onKey("F2", [edit] { return edit.commit(); })
    | ui::onKey("F3", [edit] { return edit.cancel(); })
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
