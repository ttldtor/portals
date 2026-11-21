// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

int main() {
  using namespace portals;

  ui::Ref<DocumentView> sql;
  ui::Ref<DocumentView> markdown;

  // clang-format off
  return ui::scene(ui::vbox(
    ui::label("Source desk | Tab: panel | PgUp/PgDn: scroll | F6: exact | F7: auto | F10: quit"),
    ui::hbox(
      ui::viewer(sql, "orders.sql").language("mysql").lineNumbers().flex(1)
        .framed("rounded", "SQL dump"),
      ui::viewer(markdown, "notes.md").language("markdown").flex(1)
        .framed("rounded", "Markdown source")).flex(1)))
    | ui::theme(nordTheme())
    | ui::onKey("F6", [sql, markdown] {
        sql.syntax(syntax::EXACT);
        markdown.syntax(syntax::EXACT);
      })
    | ui::onKey("F7", [sql, markdown] {
        sql.syntax(syntax::AUTO);
        markdown.syntax(syntax::AUTO);
      })
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
