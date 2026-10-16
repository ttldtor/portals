// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Show the arranged container size as the terminal changes.
 * @return Standard runner status after typed diagnostics and terminal cleanup.
 */
int main() {
  using namespace portals;
  ui::Ref<Column> content;
  ui::Ref<Label> dimensions;

  // clang-format off
  return ui::scene(ui::vbox(ui::label("Resize the terminal | F10 quit"), ui::label(dimensions, ""))
                     .reference(content))
    | ui::onLayout(content, [dimensions](const NodeGeometry &geometry) {
        const auto size = geometry.local.size();
        dimensions.text() = text("{} columns, {} rows", size.width, size.height);
      })
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
