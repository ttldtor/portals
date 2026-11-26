// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>
#include <portals/ui_virtual_rows.hpp>

#include <array>
#include <format>
#include <string>

using namespace portals;

struct TaskCard {
  bool done;
  std::string title;
  std::string category;
  std::string note;
  std::string next;

  bool operator==(const TaskCard &) const = default;
};

std::string cardPosition(const VirtualRowsPosition &position) {
  if (!position.count || !position.firstIndex || !position.visible) {
    return "No visible tasks";
  }

  return std::format("Visible {}-{} of {} | Current {}", *position.firstIndex + 1,
                     *position.firstIndex + position.visible, *position.count,
                     position.currentIndex.value_or(*position.firstIndex) + 1);
}

int main() {
  auto work = ui::model(
    std::array{TaskCard{false, "Check keyboard navigation", "UI", "Keep focus on the selected task.",
                        "Then edit its title with F2."},
               TaskCard{true, "Check the installed package", "Build", "", ""},
               TaskCard{false, "Review the guide", "Docs", "Check the complete application and its animation.", ""},
               TaskCard{false, "Try a narrow window", "UI", "Resize the terminal while reading this task.",
                        "The same task should remain in view."},
               TaskCard{false, "Publish the notes", "Docs", "", ""},
               TaskCard{false, "Remove old artifacts", "Build", "Keep the EXE and GIF for review.", ""}});
  auto personal = ui::model(std::array{
    TaskCard{false, "Read a chapter", "Study", "Resume from the saved bookmark.", ""},
    TaskCard{false, "Plan the weekend", "Home", "", ""}, TaskCard{true, "Water the plants", "Home", "", ""}});
  auto navigation = ui::navigation(work, {.readingAnchor = true});

  ui::Ref<VirtualRows<TaskCard>> cards;
  auto current = navigation.current();
  auto position = cards.position();

  // clang-format off
  return ui::scene(ui::vbox(
    ui::hbox(
      ui::button(current, ui::bySource<TaskCard>({{work, "Work *"}, {personal, "Work"}}))
        .onClick([navigation, work] { navigation.model(work); }),
      ui::button(current, ui::bySource<TaskCard>({{work, "Personal"}, {personal, "Personal *"}}))
        .onClick([navigation, personal] { navigation.model(personal); })),
    ui::virtualRows(navigation, [](auto row) {
      return ui::vbox(
        ui::hbox(ui::checkbox("", row.field(&TaskCard::done)),
                 ui::lineEdit(row.field(&TaskCard::title)),
                 ui::button("Remove").onClick([row] { return row.erase(); })),
        ui::label(row.field(&TaskCard::note)).visible(!row.value().note.empty()),
        ui::label(row.field(&TaskCard::next)).visible(!row.value().next.empty()))
        .framed("rounded", row.value().category);
    }).autoRowHeight().reference(cards).height(13).framed("rounded", "Task cards"),
    ui::label(position, cardPosition),
    ui::label(position, [](const VirtualRowsPosition &value) -> std::string {
      return value.editing ? "Tab fields | Esc cards | F10 quit"
                           : "Up/Down | F2 edit | Tab lists | F10 quit";
    })).maxWidth(76))
    | ui::theme(nordTheme())
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
  // clang-format on
}
