// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_style.hpp>
#include <string>

/** @brief Compare frame patterns and title decorations; Tab moves focus, F10 exits. */
int main() {
  using namespace portals;

  ui::Ref<Label> selected;
  const auto framedChoice = [selected](std::string pattern) {
    return ui::plainButton(pattern,
                           [selected, pattern] {
                             selected.text() = text("Frame: {}", pattern);
                           })
      .width(30)
      .height(3)
      .framed(pattern, "Title");
  };

  // clang-format off
  return ui::scene(ui::vbox(
    ui::label("Frames and titles | Tab: focus | Enter: select | F10: quit"),
    ui::grid(
      framedChoice("single"),
      framedChoice("rounded").titleDecoration("spaced"),
      framedChoice("double").titleDecoration("square"),
      framedChoice("heavy").titleDecoration("angle"),
      framedChoice("ascii").titleDecoration("round"))
      .columns(2).columnGap(2).rowGap(1).fitHeight(),
    ui::label(selected, "Select a frame with Enter")))
    | ui::theme(nordTheme())
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
  // clang-format on
}
