// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

int main() {
  using namespace portals;

  // clang-format off
  return ui::scene(
           ui::viewer("README.md")
             .lineNumbers()
             .framed("rounded", "Document"))
    | ui::theme(nordTheme())
    | ui::style(R"pss(
        DocumentView {
          role-body-foreground: #dce1e8;
          role-gutter-foreground: #7d8797;
          role-position-foreground: #62b4d7;
          role-position-bold: true;
        }
      )pss")
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}