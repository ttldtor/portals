// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_model_find.hpp>

#include <array>
#include <string>

using namespace portals;

namespace {
constexpr std::string_view DOCUMENT =
  "Portals keeps terminal interfaces small. Find Portals here, then find Portals again. "
  "Unicode stays whole: cafe\u0301, 界 and 👩‍💻. Overlaps: aaaa.";

} // namespace

int main() {
  // clang-format off
  return [] {
    ui::Ref<FindBar> find;
    const std::array<ModelRow<std::string>, 1> rows{{{{1, 1}, std::string(DOCUMENT)}}};
    auto browser = ui::scene(ui::vbox(
      ui::label("Find in a document | F3 next | Shift+F3 previous | F10 closes"),
      ui::label("Type a literal query. The bar shows its count and selected context."),
      ui::findBar(find, RowCollection<std::string>::from(rows),
                  [](const std::string &text) -> std::string_view { return text; }),
      ui::label("Previous and Next wrap. Clear empties the query; Ctrl+Z restores the draft.")));

    return std::move(browser)
      | ui::onKey(key::F3, [find] { find.next(); })
      | ui::onKey(key::SHIFT + key::F3, [find] { find.previous(); })
      | ui::onKey(key::F10, ui::quit);
  }
  | ui::runApp();
  // clang-format on
}
