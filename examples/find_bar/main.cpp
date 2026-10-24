// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

#include <string_view>

using namespace portals;

int main() {
  constexpr std::string_view INTRODUCTION =
    "Portals keeps terminal interfaces small. Find Portals here, then find Portals again. "
    "Unicode stays whole: cafe\u0301, 界 and 👩‍💻.";
  constexpr std::string_view TRANSLATION =
    "Портал открывает документ. Другой портал показывает результат. "
    "Поиск точный: портал и Портал — разные запросы.";
  ui::Ref<FindBar> find;

  auto browser = ui::scene(ui::vbox(
    ui::label("Find in a small document | F3 next | Shift+F3 previous | F10 closes"),
    ui::label("Type a literal query. The bar shows its count and the selected context."),
    ui::findBar(find, INTRODUCTION),
    ui::hbox(
      ui::button("English sample").onClick([=] {
        find.document(INTRODUCTION);
      }),
      ui::button("Russian sample").onClick([=] {
        find.document(TRANSLATION);
      }),
      ui::button("Find Portals").onClick([=] {
        find.text() = "Portals";
      })),
    ui::label("Previous and Next wrap. Clear empties the query; Ctrl+Z restores the draft.")));

  // clang-format off
  return std::move(browser)
    | ui::style(".find-bar-summary { bold: true; }")
    | ui::onKey(key::F3, [=] {
        find.next();
      })
    | ui::onKey(key::SHIFT + key::F3, [=] {
        find.previous();
      })
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
  // clang-format on
}
