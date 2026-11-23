// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

using namespace portals;

struct Counter {
  int clicks{};

  bool operator==(const Counter &) const = default;
};

int main() {
  auto counter = ui::model(Counter{});
  auto clicks = counter.field(&Counter::clicks);
  ui::Ref<Button> increment;

  // clang-format off
  return ui::scene(ui::vbox(
    ui::label(clicks),
    ui::button(increment, text("Increment ({})", clicks)))
      .width(36).height(6).framed("rounded", text("Accepted clicks: {}", clicks)))
    | ui::onClick(increment, [clicks]() mutable { clicks += 1; })
    | ui::theme(nordTheme())
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
