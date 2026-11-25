// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/log_file.hpp>
#include <portals/ui_lyra.hpp>
#include <portals/ui_style.hpp>
#include <portals/ui_virtual_rows.hpp>

#include <expected>
#include <filesystem>
#include <format>
#include <string>

std::string logPosition(const portals::VirtualRowsPosition &position) {
  const auto above = position.hasBefore ? (*position.hasBefore ? "more above" : "start") : "above unknown";
  const auto below = position.hasAfter ? (*position.hasAfter ? "more below" : "end") : "below unknown";

  return std::format("{} records visible | {} | {}", position.visible, above, below);
}

int main(int argc, char **argv) {
  using namespace portals;

  std::string path = "application.log";
  bool help{};
  // clang-format off
  auto cli = ::lyra::cli()
    | ::lyra::help(help)
    | ::lyra::arg(path, "file")("UTF-8 log file; default: application.log");

  return [&]() -> std::expected<ui::Scene, ManagedError> {
    auto file = FileLogModel::create(std::filesystem::path(std::u8string(path.begin(), path.end())));

    if (!file) {
      return std::unexpected(ManagedError{
        .phase = ManagedPhase::MOUNT,
        .cause = file.error().error == FileLogError::NO_MEMORY
                   ? TreeError::NO_MEMORY : TreeError::INVALID_ARGUMENT});
    }

    ui::Ref<Label> inspected;
    ui::Ref<VirtualRows<LogLine>> rows;
    auto position = rows.position();
    auto model = ui::model<LogLine>(*file);

    return ui::scene(ui::vbox(
      ui::label(path),
      ui::virtualRows(model, [inspected](auto row) {
        return ui::hbox(
          ui::button("Inspect").onClick([row, inspected] {
            return row.snapshot().transform([inspected](const auto &snapshot) {
              inspected.text() = std::string(snapshot.value().text);
            });
          }),
          ui::spacing(1),
          ui::label(std::string(row.value().text)));
      }).reference(rows).height(10).framed("rounded", "Log records"),
      ui::label(inspected, "Choose Inspect to read a record here."),
      ui::label(position, logPosition),
      ui::label(position, [](const VirtualRowsPosition &value) -> std::string {
        return value.editing ? "Enter/Space Inspect | Esc rows | F10 quit"
                             : "Up/Down rows | F2 controls | F10 quit";
      })).maxWidth(88))
      | ui::theme(nordTheme())
      | ui::onKey("F10 | Ctrl+C", ui::quit);
  }
    | ui::runApp(argc, argv, ui::lyra::cli(cli, {.automaticHelp = false, .helpRequested = &help}));
  // clang-format on
}
