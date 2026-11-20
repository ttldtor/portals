// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::Ref<DocumentView> reader;

  // clang-format off
  return ui::scene(ui::vbox(
    ui::label("Ctrl+arrows / Alt+wheel: fast | PgUp/PgDn: page | F10: quit"),
    ui::hbox(
      ui::button("Top").onClick([=] { reader.home(); }),
      ui::button("Back 8").onClick([=] { reader.scroll(-8); }),
      ui::button("Previous").onClick([=] { reader.page(-1); }),
      ui::button("Next").onClick([=] { reader.page(1); }),
      ui::button("End").onClick([=] { reader.end(); })),
    ui::viewer(reader, "README.md").fastScroll().pageOverlap(2).framed("rounded", "Document")))
    | ui::theme(nordTheme())
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
