// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

#include <format>

int main() {
  using namespace portals;
  constexpr std::string_view CONNECTION =
    "Connection checklist\n"
    "Retry: check the cable, then retry the connection.\n"
    "Retry: verify the server address before reconnecting.\n"
    "Unicode host names stay whole: café, cafe\u0301, 東京 and 👩‍💻.";
  constexpr std::string_view DISPLAY =
    "Display checklist\n"
    "Retry: resize the terminal after reconnecting.\n"
    "Retry: check UTF-8 when café or 東京 looks wrong.\n"
    "Шрифт: проверьте символы 界 и 👩‍💻 перед повтором.";
  constexpr std::size_t JOURNAL_LINES = 40;
  ui::Ref<FormField> operatorName;
  ui::Ref<FindBar> search;
  ui::Ref<Label> selected;
  ui::Ref<LogView> journal;

  // clang-format off
  auto scene = ui::scene(ui::vbox(
    ui::label("Search journal | F3 next | Shift+F3 previous | F10 closes"),
    ui::formField(operatorName, "Operator", "Alex"),
    ui::label("Search a troubleshooting note: try Retry, café, 東京 or 👩‍💻."),
    ui::findBar(search, CONNECTION, {}, {.selected = [=](FindMatch match) {
      if (!match) {
        selected.text() = "No selected match";
        journal.append("Selection cleared\n");

        return;
      }

      selected.text() = operatorName.text()
        + text(" · {} · {}/{}", match.text(), match.ordinal(), match.storedCount());
      journal.append(std::format("Match {}/{}: {}\n", match.ordinal(), match.storedCount(), match.text()));
    }}),
    ui::hbox(
      ui::button("Connection note").onClick([=] {
        search.document(CONNECTION);
        journal.append("Opened connection note\n");
      }),
      ui::button("Display note").onClick([=] {
        search.document(DISPLAY);
        journal.append("Opened display note\n");
      }),
      ui::button("Find Retry").onClick([=] {
        search.text() = "Retry";
      })),
    ui::label(selected, "Choose a query to record a match"),
    ui::label("Recent events | PageUp or wheel reads history"),
    ui::logView(journal, "Ready: connection note\n", {.limits = {.maxLines = JOURNAL_LINES}}).height(6),
    ui::hbox(
      ui::button("Return to tail").onClick([=] {
        journal.tail();
      }),
      ui::button("Clear journal").onClick([=] {
        journal.clear();
      }))));

  return scene
    | ui::style(".find-bar-summary { bold: true; }")
    | ui::onKey(key::F3, [=] {
        search.next();
      })
    | ui::onKey(key::SHIFT + key::F3, [=] {
        search.previous();
      })
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
  // clang-format on
}
