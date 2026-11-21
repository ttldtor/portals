// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_lyra.hpp>

int main(int argc, char **argv) {
  using namespace portals;

  // clang-format off
  constexpr std::string_view HINTS = "Ctrl+O: open path | F2 Text | F3 Hex | F5 refresh | F10 quit";
  std::string initialPath = "README.md";
  bool help{};
  auto cli = ::lyra::cli()
    | ::lyra::help(help).description(
        "Read a text or binary file in bounded windows. Change the path and choose Open, "
        "or press Ctrl+O. Relative paths use the working directory; paths are literal, "
        "without environment or home expansion. No file contents are changed. "
        "The interactive run temporarily changes terminal modes and restores them on orderly exit. "
        "Exit status: 0 for success/help, 2 for invalid CLI, 1 for application failure.")
    | ::lyra::arg(initialPath, "file")("Read-only file path (default: README.md)").optional();

  return [&] {
    ui::Ref<LineEdit> path;
    ui::Ref<DocumentView> reader;
    auto open = [=]() -> std::expected<void, ManagedError> {
      auto selected = path.text().get();

      if (!selected) {
        return std::unexpected(selected.error());
      }

      return reader.setModel(ui::documentFile(*selected));
    };

    return ui::scene(ui::vbox(
      ui::hbox(ui::label("File: "), ui::lineEdit(path, initialPath), ui::button("Open").onClick(open)),
      ui::viewer(reader, initialPath).fastScroll().framed("rounded", "Document"),
      ui::label(HINTS)))
      | ui::theme(nordTheme())
      | ui::onKey("Ctrl+O", open)
      | ui::onKey("F5", [=] { reader.reload(); })
      | ui::onKey("F2", [=] { reader.mode(DocumentViewMode::TEXT); })
      | ui::onKey("F3", [=] { reader.mode(DocumentViewMode::HEX); })
      | ui::onKey("F10 | Ctrl+C", ui::quit)
      | ui::reportOnError();
  }
  | ui::runApp(argc, argv, ui::lyra::cli(cli, {.automaticHelp = false, .helpRequested = &help}));
  // clang-format on
}
