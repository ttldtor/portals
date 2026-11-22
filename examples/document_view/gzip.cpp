// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_gzip.hpp>
#include <portals/ui_style.hpp>

int main() {
  using namespace portals;

  ui::Ref<DocumentView> reader;
  ui::Ref<Label> selected;
  const auto archive = ui::gzipFile("archive.sql.gz");
  const auto original = ui::documentFile("archive.sql");

  // clang-format off
  return ui::scene(ui::vbox(
    ui::label("Archive desk | F2: original | F3: gzip | Home/End: first/last page | F10: quit"),
    ui::label(selected, "gzip: archive.sql.gz"),
    ui::viewer(reader, archive).language("mysql").lineNumbers().flex(1)
      .framed("rounded", "Orders")))
    | ui::theme(nordTheme())
    | ui::onKey("F2", [reader, selected, original] {
        reader.model(original);
        selected.text() = "Plain: archive.sql";
      })
    | ui::onKey("F3", [reader, selected, archive] {
        reader.model(archive);
        selected.text() = "gzip: archive.sql.gz";
      })
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
