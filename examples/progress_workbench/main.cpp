// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <format>

/** @brief Manually track one work item; the gauge owns its current fractional value. */
int main() {
  using namespace portals;
  constexpr double STEP = .125;
  ui::Ref<Gauge> progress;
  ui::Ref<FormField> workItem;
  ui::Ref<Label> status;
  ui::Ref<LogView> history;

  auto record = [=](std::expected<void, ManagedError> changed, std::string_view action) {
    if (changed) {
      status.text() = workItem.text() + text(" · {}", action);
      history.append(std::format("{}\n", action));
    }

    return changed;
  };

  auto scene = ui::scene(ui::vbox(
    ui::label("Progress workbench | Tab moves focus | F10 closes"),
    ui::formField(workItem, "Work item", "Review the change", {.help = "Name the work you are tracking manually"}),
    ui::gauge(progress, .25).width(40),
    ui::label("Each step is one eighth; exceeding 0..1 reports an error and ends this run"),
    ui::hbox(
      ui::button("Advance 1/8").onClick([=] {
        return record(progress.adjust(STEP), "Advanced by one eighth");
      }),
      ui::button("Back 1/8").onClick([=] {
        return record(progress.adjust(-STEP), "Moved back by one eighth");
      }),
      ui::button("Reset").onClick([=] {
        return record(progress.value(0), "Reset to zero");
      }),
      ui::button("Complete").onClick([=] {
        return record(progress.value(1), "Marked complete");
      })),
    ui::label(status, "Started at one quarter"),
    ui::logView(history, "Started at one quarter\n", {.limits = {.maxLines = 32}}).height(8)));

  return scene
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
}
