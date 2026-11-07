// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::Ref<DocumentView> reader;
  auto readme = ui::documentFile("README.md");
  auto releases = ui::documentFile("ReleaseNotes.md");

  // clang-format off
  return ui::scene(ui::vbox(
    ui::label("Documents | arrows / PgUp / PgDn / wheel | F10 closes"),
    ui::hbox(
      ui::button("README").onClick([=] { reader.model(readme); }),
      ui::button("Release notes").onClick([=] { reader.model(releases); }),
      ui::button("Refresh").onClick([=] { reader.reload(); }),
      ui::button("Top").onClick([=] { reader.byte(0); })),
    ui::viewer(reader, readme).lineNumbers().framed("rounded", "Document")))
    | ui::theme(nordTheme())
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
