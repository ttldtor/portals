// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

int main() {
  using namespace portals;

  // clang-format off
  return ui::scene(ui::vbox(
    ui::label("Source desk | Tab: switch document | wheel / PgDn: scroll | F10: quit"),
    ui::hbox(
      ui::viewer("orders.sql").language("mysql").lineNumbers().flex(1)
        .framed("rounded", "SQL dump"),
      ui::viewer("notes.md").language("markdown").flex(1)
        .framed("rounded", "Markdown source")).flex(1)))
    | ui::theme(nordTheme())
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
