// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_suggestions.hpp>

#include <array>
#include <string>

using namespace portals;

int main() {
  return [] {
    ui::Ref<LineEdit> query;
    ui::Ref<VirtualList<SearchItem>> results;
    ui::Ref<Label> status;
    ui::Ref<Label> chosen;
    const std::array<ModelRow<std::string>, 7> commands{{
      {{1, 1}, "Open file"},
      {{2, 1}, "Open recent document"},
      {{3, 1}, "Find in document"},
      {{4, 1}, "Show cafe\u0301 notes"},
      {{5, 1}, "Show Straße history"},
      {{6, 1}, "Открыть справку"},
      {{7, 1}, "Show 👩‍💻 workspace"}
    }};

    ui::SuggestionsConfig config;
    config.status = status;
    config.activated = [chosen](SearchSelection selection) { chosen.text() = selection.text(); };
    config.failed = [status](SearchError) { status.text() = "Search failed; the query is preserved."; };

    auto browser = ui::scene(ui::vbox(
      ui::label("Search commands | type a prefix or typo | F10 closes"),
      ui::lineEdit(query),
      ui::label("Tab to results; arrows select; Enter confirms. Ctrl+Z undoes input."),
      ui::virtualList(results,
        ui::suggestions(query.draftText(), RowCollection<std::string>::from(commands),
          [](const std::string &text) -> std::string_view { return text; }),
        std::move(config)).height(8),
      ui::label(status, "Type to search the reusable index."),
      ui::label(chosen, "No command chosen.")));

    return std::move(browser)
      | ui::onKey(key::F10, ui::quit);
  }
  | ui::runApp();
}
