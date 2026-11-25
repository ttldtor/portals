// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>
#include <portals/ui_virtual_rows.hpp>

#include <array>
#include <string>

using namespace portals;

struct Task {
  bool done;
  std::string caption;
  std::string category;

  bool operator==(const Task &) const = default;
};

int main() {
  auto work = ui::model(std::array{
    Task{false, "Review release notes", "Docs"},
    Task{true, "Check package install", "Build"},
    Task{false, "Verify keyboard focus", "UI"},
    Task{false, "Review translations", "Docs"},
    Task{false, "Publish documentation", "Docs"},
    Task{false, "Remove old artifacts", "Build"},
    Task{false, "Review accessibility", "UI"},
    Task{false, "Check resize behavior", "UI"},
    Task{false, "Prepare the change log", "Docs"},
    Task{false, "Review package metadata", "Build"},
    Task{false, "Record the new example", "UI"},
    Task{false, "Review the final patch", "Review"}});
  auto personal = ui::model(std::array{
    Task{false, "Buy groceries", "Home"},
    Task{true, "Water the plants", "Home"},
    Task{false, "Read a chapter", "Study"},
    Task{false, "Practice French", "Study"},
    Task{false, "Plan the weekend", "Home"},
    Task{false, "Sort the photos", "Home"},
    Task{false, "Back up the notes", "Home"},
    Task{false, "Call a friend", "Home"}});
  auto navigation = ui::navigation(work, {.readingAnchor = true});

  // clang-format off
  return ui::scene(ui::vbox(
    ui::hbox(
      ui::button("Work").onClick([navigation, work] { navigation.model(work); }),
      ui::button("Personal").onClick([navigation, personal] { navigation.model(personal); })),
    ui::virtualRows(navigation, [](auto row) {
      return ui::hbox(
        ui::checkbox("", row.field(&Task::done)),
        ui::label(row.field(&Task::category)).width(8),
        ui::lineEdit(row.field(&Task::caption)).minWidth(18).maxWidth(40),
        ui::button("Remove").onClick([row] { return row.erase(); }));
    }).height(8).framed("rounded", "Task lists"),
    ui::label("Tab: controls | Space: check | F10: quit")))
    | ui::theme(nordTheme())
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
