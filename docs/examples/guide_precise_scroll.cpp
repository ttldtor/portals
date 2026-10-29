// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_lyra.hpp>
#include <portals/ui_style.hpp>

/** @brief Show both-axis thumb dragging; --help completes before scene creation or terminal I/O.
 * @param argc Number of native command-line arguments.
 * @param argv Borrowed native arguments; the parser and bound flag live through the synchronous runner.
 * @return 0 on help/orderly cleanup, 1 on runtime failure, or 2 on invalid arguments.
 */
int main(int argc, char **argv) {
  using namespace portals;
  bool fifoHost{};
  auto parser =
    ::lyra::cli() |
    ::lyra::opt(fifoHost)["--fifo-host"](
      "Declare host FIFO ordering for complete input packets and query replies. Default: ordinary cell fallback. "
      "AUTO still requires 1016/2048 and nonzero text-area pixels; it restores native modes on exit. "
      "This declaration does not qualify a physical host.");

  // clang-format off
  return [&] {
    // The factory observes the flag only after the lazy command-line parse.
    ApplicationOptions settings;
    settings.nativePointer.ordering = fifoHost ? NativeInputOrder::FIFO : NativeInputOrder::UNCONFIRMED;

    return ui::scene(ui::vbox(
      ui::label("Drag either thumb; arrows and wheel keep ordinary scrolling. F10 exits."),
      ui::scrollArea(ui::vbox(
        ui::label("01 Overview / exact logical positions and ordinary cell rendering"),
        ui::label("02 Accounts / optional pixel input with bounded cell fallback"),
        ui::label("03 Orders / nonzero grab position remains attached to the thumb"),
        ui::label("04 Positions / both axes share the owning coordinate context"),
        ui::label("05 History / resize and font changes cancel incompatible capture"),
        ui::label("06 Connections / unsupported hosts keep their ordinary controls"),
        ui::label("07 Preferences / no font metrics are guessed from a window"),
        ui::label("08 About / physical pixel-host qualification remains separate"))
        .width(70))
        .framed("rounded", "Precise pointer input")
        .scrollOnFrame().arrows().width(38).height(7),
      ui::button("Exit", [] { ui::quit(); })))
      | ui::applicationOptions([settings] { return settings; })
      | ui::onKey(key::F10, ui::quit);
  } | ui::runApp(argc, argv, ui::lyra::cli(parser));
  // clang-format on
}
