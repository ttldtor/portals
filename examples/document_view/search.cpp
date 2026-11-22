// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_document_export.hpp>
#include <portals/ui_document_search.hpp>
#include <portals/ui_style.hpp>

int main() {
  using namespace portals;

  ui::Ref<DocumentView> reader;
  ui::Ref<FindBar> search;
  ui::Ref<Button> save;
  ui::Ref<Label> status;

  // clang-format off
  return ui::scene(ui::vbox(
    ui::label("Orders | F3: next | Shift+F3: previous | F4: hex | F5: text | F10: quit"),
    ui::findBar(search, reader, "INSERT"),
    ui::viewer(reader, ui::documentFile("archive.sql")).language("mysql").lineNumbers().flex(1)
      .framed("rounded", "Orders"),
    ui::hbox(LinearOptions{.gap = 1}, ui::button(save, "Export copy"), ui::label(status, "Ready"))))
    | ui::theme(nordTheme())
    | ui::onClick(save, [reader, status] {
        ui::exportFile(reader, "orders-copy.sql", [status](const auto &result) {
          status.text() = result.complete ? "Export complete" : "Export failed";
        });
      })
    | ui::onKey("F3", [search] { search.next(); })
    | ui::onKey("Shift+F3", [search] { search.previous(); })
    | ui::onKey("F4", [reader] { reader.mode(DocumentViewMode::HEX); })
    | ui::onKey("F5", [reader] { reader.mode(DocumentViewMode::TEXT); })
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
