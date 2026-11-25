// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "navigation_checkpoint_provider.hpp"

#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

#include <format>
#include <string>

using namespace portals;
using namespace checkpoint_example;

int main() {
  auto original = ui::finiteTextDocument(NOTE);
  auto reopened = ui::finiteTextDocument(NOTE);
  auto status = ui::model(std::string("Save the reading place, then restore it in the second opening."));
  NavigationCheckpoint saved;
  auto marker = [](const ui::TextDocumentValue &value) {
    return std::string(value.selection.cursor, ' ') + "^";
  };
  auto save = [&]() -> std::expected<void, ManagedError> {
    auto result = remember(original, saved);
    if (result) {
      status.value() =
        std::format("Saved {} bytes of location metadata. Document text is not included.", saved.bytes().size());
    }

    return result;
  };
  auto restore = [&]() -> std::expected<void, ManagedError> {
    if (saved.bytes().empty()) {
      status.value() = "Save a place first.";

      return {};
    }

    auto result = resume(reopened, saved);
    if (result) {
      status.value() = "Restored the same place in an independently created document.";
    }

    return result;
  };

  // clang-format off
  return ui::scene(ui::vbox(LinearOptions{.gap = 1},
    ui::vbox(ui::label(original), ui::label(original.source(), marker))
      .framed("rounded", "First opening"),
    ui::vbox(ui::label(reopened), ui::label(reopened.source(), marker))
      .framed("rounded", "Independent opening"),
    ui::hbox(LinearOptions{.gap = 1},
      ui::button("Read middle", [original] { return seek(original, READING_OFFSET); }),
      ui::button("Save place", save),
      ui::button("Restore place", restore),
      ui::button("Back to start", [reopened] { return seek(reopened, 0); })),
    ui::label(status),
    ui::label("F2: save place | F3: restore place | F10: quit"))
      .maxWidth(84).fitHeight().framed("rounded", "Portable reading place"))
    | ui::theme(nordTheme())
    | ui::onKey("F2", save)
    | ui::onKey("F3", restore)
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
