// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

/** @brief Compare built-in animations and control an owning custom sequence.
 * @return Standard runner status after diagnostics and terminal restoration.
 */
int main() {
  using namespace portals;
  ui::Ref<Spinner> transfer;

  // clang-format off
  return ui::scene(ui::vbox(
    ui::label("Loading indicators | F10: quit"),
    ui::hbox(
      ui::vbox(
        ui::spinner("dots").label("Dots"),
        ui::spinner("line").label("Line"),
        ui::spinner("braille").label("Braille"),
        ui::spinner("bar-bounce").label("Bar bounce"))
        .width(24).framed("rounded", "Classic"),
      ui::vbox(
        ui::spinner("arc").label("Arc"),
        ui::spinner("circle").label("Circle"),
        ui::spinner("square").label("Square"),
        ui::spinner("triangle").label("Triangle"))
        .width(24).framed("rounded", "Shapes"),
      ui::vbox(
        ui::spinner("arrow").label("Arrow"),
        ui::spinner("blocks").label("Blocks"),
        ui::spinner("ellipsis").label("Ellipsis"),
        ui::spinner("pulse").label("Pulse"))
        .width(24).framed("rounded", "Motion")),
    ui::vbox(
      ui::spinner().reference(transfer)
        .frames({"[   ]", "[=  ]", "[== ]", "[===]"})
        .interval(120).label("Receiving quotes"),
      ui::hbox(
        ui::button("Start", [transfer] { return transfer.start(); }),
        ui::button("Stop", [transfer] { return transfer.stop(); })))
      .framed("rounded", "Custom frames"),
    ui::label("Animations show activity; they do not measure completed work.")).maxWidth(72))
    | ui::theme(nordTheme())
    | ui::style("Spinner { foreground: #62b4d7; } Spinner:disabled { foreground: #808080; }")
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
