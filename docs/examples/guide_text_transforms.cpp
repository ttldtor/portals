// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <utility>

/** @brief Show whitespace-delimited words and extended graphemes from the live editor draft. */
int main() {
  using namespace portals;

  // clang-format off
  return [] {
    ui::Ref<LineEdit> input;
    auto scene = ui::scene(ui::vbox(ui::lineEdit(input, "Hello, Portals!"), ui::label("Words"),
                                    ui::virtualList(ui::words(input.draftText())).height(4),
                                    ui::label("Graphemes (including spaces)"),
                                    ui::virtualList(ui::graphemes(input.draftText())).height(4), ui::button("Quit", [] {
                                      ui::quit();
                                    })));
    return std::move(scene)
      | ui::onKey("F10", ui::quit);
  } | ui::runApp();
  // clang-format on
}
