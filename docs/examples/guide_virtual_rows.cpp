// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>
#include <portals/ui_virtual_rows.hpp>

#include <array>
#include <format>
#include <string>

using namespace portals;

struct Task {
  bool done;
  std::string caption;
  std::string category;

  bool operator==(const Task &) const = default;
};

std::string taskPosition(const VirtualRowsPosition &position) {
  if (!position.count) {
    return "Preparing tasks...";
  }

  if (*position.count == 0) {
    return "No tasks";
  }

  if (!position.firstIndex || !position.visible) {
    return "Preparing tasks...";
  }

  auto result = std::format("Visible {}-{} of {}", *position.firstIndex + 1, *position.firstIndex + position.visible,
                            *position.count);

  if (position.currentIndex) {
    const bool outside = *position.currentIndex < *position.firstIndex ||
                         *position.currentIndex >= *position.firstIndex + position.visible;

    result += std::format(" | Current {}{}", *position.currentIndex + 1, outside ? " (outside view)" : "");
  }

  return result;
}

int main() {
  auto work =
    ui::model(std::array{Task{false, "Review release notes", "Docs"}, Task{true, "Check package install", "Build"},
                         Task{false, "Verify keyboard focus", "UI"}, Task{false, "Review translations", "Docs"},
                         Task{false, "Publish documentation", "Docs"}, Task{false, "Remove old artifacts", "Build"},
                         Task{false, "Review accessibility", "UI"}, Task{false, "Check resize behavior", "UI"},
                         Task{false, "Prepare the change log", "Docs"}, Task{false, "Review package metadata", "Build"},
                         Task{false, "Record the new example", "UI"}, Task{false, "Review the final patch", "Review"}});
  auto personal = ui::model(std::array{Task{false, "Buy groceries", "Home"}, Task{true, "Water the plants", "Home"},
                                       Task{false, "Read a chapter", "Study"}, Task{false, "Practice French", "Study"},
                                       Task{false, "Plan the weekend", "Home"}, Task{false, "Sort the photos", "Home"},
                                       Task{false, "Back up the notes", "Home"}, Task{false, "Call a friend", "Home"}});
  auto navigation = ui::navigation(work, {.readingAnchor = true});

  ui::Ref<VirtualRows<Task>> rows;
  auto current = navigation.current();
  auto position = rows.position();

  // clang-format off
  return ui::scene(ui::vbox(
    ui::hbox(
      ui::button(current, ui::bySource<Task>({{work, "Work *"}, {personal, "Work"}}))
        .onClick([navigation, work] { navigation.model(work); }),
      ui::button(current, ui::bySource<Task>({{work, "Personal"}, {personal, "Personal *"}}))
        .onClick([navigation, personal] { navigation.model(personal); })),
    ui::virtualRows(navigation, [](auto row) {
      return ui::hbox(
        ui::checkbox("", row.field(&Task::done)),
        ui::label(row.field(&Task::category)).width(8),
        ui::lineEdit(row.field(&Task::caption)).maxWidth(36),
        ui::button("Remove").onClick([row] { return row.erase(); }));
    }).reference(rows).height(8).framed("rounded", "Task lists"),
    ui::label(position, taskPosition),
    ui::label(position, [](const VirtualRowsPosition &value) -> std::string {
      return value.editing ? "Tab fields | Esc rows | F10 quit"
                           : "Up/Down rows | F2 fields | Tab lists | F10 quit";
    })).maxWidth(76))
    | ui::theme(nordTheme())
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
