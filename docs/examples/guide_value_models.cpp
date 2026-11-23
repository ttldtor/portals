// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

#include <format>
#include <string>
#include <utility>

using namespace portals;

struct Region {
  std::string name;
  int population;

  bool operator==(const Region &) const = default;
};

int main() {
  auto europe = ui::model(Region{"Europe", 742});
  auto asia = ui::model(Region{"Asia", 4800});
  auto selected = europe;
  ui::Ref<Label> summary;
  auto describe = [](const Region &region) {
    return std::format("{}: {} million people", region.name, region.population);
  };
  auto choose = [&](ui::Model<Region> candidate) {
    if (summary.model(candidate)) {
      selected = std::move(candidate);
    }
  };
  auto increase = [&] {
    selected.field(&Region::population) += 1;
  };

  // clang-format off
  return ui::scene(ui::vbox(LinearOptions{.gap = 1},
    ui::label(summary, europe, describe),
    ui::hbox(LinearOptions{.gap = 2},
      ui::button("Europe", [&] { choose(europe); }),
      ui::button("Asia", [&] { choose(asia); }),
      ui::button("+1 million", increase)),
    ui::label("Each source keeps its own value. Tab selects a button; Enter activates it."))
      .maxWidth(76).fitHeight().framed("rounded", "Region populations"))
    | ui::theme(nordTheme())
    | ui::onKey("F4", increase)
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
