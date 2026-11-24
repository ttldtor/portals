// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>
#include <portals/ui_text_document.hpp>

#include <format>
#include <string>

using namespace portals;

int main() {
  auto note = ui::finiteTextDocument("Hello world");
  constexpr std::size_t WORD_START = 6;
  auto word = note.bookmark(WORD_START);
  auto position = [](const ui::TextDocumentValue &value) {
    return std::format("Caret: {} | {} | {}", value.selection.cursor, value.dirty ? "modified" : "saved", value.target);
  };

  // clang-format off
  return ui::scene(ui::vbox(LinearOptions{.gap = 1},
    ui::label("The bookmark follows world when text is inserted before it."),
    ui::vbox(
      ui::label(note),
      ui::label(note.source(), [](const ui::TextDocumentValue &value) {
        return std::string(value.selection.cursor, ' ') + "^";
      })).framed("rounded", "Document and caret"),
    ui::label(note.source(), position),
    ui::hbox(LinearOptions{.gap = 1},
      ui::button("Insert prefix", [note] { return note.insert(0, "Hi, "); }),
      ui::button("Find bookmark", [note, word] { return note.restore(word); }),
      ui::button("Undo", [note] { return note.undo(); }),
      ui::button("Redo", [note] { return note.redo(); })),
    ui::hbox(LinearOptions{.gap = 1},
      ui::button("Savepoint", [note] { return note.savepoint("memory:note"); }),
      ui::button("Savepoint as copy", [note] { return note.savepoint("memory:copy"); })),
    ui::label(note).minWidth(40).framed("rounded", "Shared document"),
    ui::label("Savepoints are in memory. This example does not write files."),
    ui::label("F2: savepoint | Ctrl+Z: undo | Ctrl+Y: redo | F10: quit"))
      .maxWidth(84).fitHeight().framed("rounded", "Text anchors"))
    | ui::theme(nordTheme())
    | ui::onKey("F2", [note] { return note.savepoint("memory:note"); })
    | ui::onKey("Ctrl+Z", [note] { return note.undo(); })
    | ui::onKey("Ctrl+Y", [note] { return note.redo(); })
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
