// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_model_form.hpp>
#include <portals/ui_style.hpp>

#include <array>

using namespace portals;

struct City {
  std::string name;
  int orders{};
  std::string region;

  bool operator==(const City &) const = default;
};

int main() {
  auto cities =
    ui::model(std::array{City{"Prague", 12, "Europe"}, City{"Vienna", 25, "Europe"}, City{"Tokyo", 48, "Asia"}});
  auto navigation = ui::navigation(cities);
  auto edit = ui::edit(navigation, {.mode = EditMode::EXPLICIT});
  auto russian = ui::model(false);
  auto translate = [](std::string_view key, bool inRussian) -> std::expected<std::string, TextResourceError> {
    if (key != "orders") {
      return std::unexpected(TextResourceError::MISSING_RESOURCE);
    }

    return inRussian ? "Заказы" : "Orders";
  };

  // clang-format off
  return ui::scene(ui::vbox(LinearOptions{.gap = 1},
    ui::hbox(LinearOptions{.gap = 2},
      ui::virtualList(navigation, &City::name)
        .width(20).framed("rounded", "Cities"),
      ui::form(edit,
        ui::field("City", &City::name).required(),
        ui::field(text(i18n("orders"), translate, russian), &City::orders)
          .validate([](int count) { return count >= 0; }),
        ui::field("Region (read only)", &City::region).readOnly(),
        ui::hbox(LinearOptions{.gap = 2},
          ui::button("Save", [edit] { return edit.commit(); }),
          ui::button("Cancel", [edit] { return edit.cancel(); })))
        .minWidth(36).framed("rounded", "Edit city"))
      .height(17),
    ui::label("Tab: fields | F2: save | F3: cancel | F4: English | F5: Russian | F10: quit"),
    ui::label("Orders require a whole nonnegative number. Cancel restores accepted values."))
      .maxWidth(84).fitHeight())
    | ui::theme(nordTheme())
    | ui::onKey("F2", [edit] { return edit.commit(); })
    | ui::onKey("F3", [edit] { return edit.cancel(); })
    | ui::onKey("F4", [russian]() mutable { russian.value() = false; })
    | ui::onKey("F5", [russian]() mutable { russian.value() = true; })
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
