// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

int main() {
  using namespace portals;

  ui::Ref<DocumentView> document;

  // clang-format off
  return ui::scene(
    ui::viewer(document, "README.md").hex().fastScroll()
      .framed("rounded", "F2 Text | F3 Hex | F10 Quit"))
    | ui::theme(nordTheme())
    | ui::style(R"pss(
        DocumentView {
          role-hex-zero-foreground: #7d8797;
          role-hex-control-foreground: #e5b567;
          role-hex-high-byte-foreground: #b48ead;
        }
      )pss")
    | ui::onKey("F2", [document] { document.mode(DocumentViewMode::TEXT); })
    | ui::onKey("F3", [document] { document.mode(DocumentViewMode::HEX); })
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
