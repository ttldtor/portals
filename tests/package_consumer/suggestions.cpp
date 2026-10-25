// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_suggestions.hpp>

#include <array>

int main() {
  using namespace portals;
  ui::Ref<LineEdit> input;
  const std::array<ModelRow<std::string>, 1> rows{{{{1, 1}, "Open installed help"}}};
  auto view = ui::virtualList(ui::suggestions(input.draftText(), RowCollection<std::string>::from(rows),
                                              [](const std::string &text) -> std::string_view {
                                                return text;
                                              }));

  return SearchOptions{}.topK == 32 && sizeof(view) != 0 ? 0 : 1;
}
