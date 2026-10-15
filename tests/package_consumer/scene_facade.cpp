// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "tests/support/examples/replay/terminal.hpp"
#include <portals/ui.hpp>

#include "tests/support/custom_widget_fluent.hpp"
#include "tests/support/examples/custom_widget/meter.hpp"
#include "tests/support/examples/gallery/form.hpp"
#include "tests/support/gallery_fluent.hpp"

namespace {
bool installedExampleEquivalence() {
  using namespace portals;

  for (std::size_t theme = 0; theme < example::THEME_PRESETS.size(); ++theme) {
    for (auto colors : {ColorMode::TRUE_COLOR, ColorMode::ANSI16}) {
      ApplicationOptions policies;

      policies.writer.colorMode = colors;

      const std::vector<example::ReplayTerminal::Input> formScript{{50,
                                                                    "\x01"
                                                                    "Ada\r\t \t\r",
                                                                    {}},
                                                                   {100, "\x1bOQ", {}},
                                                                   {150, "\x1b[21~", {}}};
      example::ReplayTerminal beforeFormTerminal(formScript, 200, {80, 24});
      auto formApp = example::take(Application::create(policies, beforeFormTerminal.clock()));
      gallery::Form beforeForm(*formApp, theme);
      auto beforeFormStats = beforeForm.run(beforeFormTerminal);

      if (!beforeFormStats || !beforeFormTerminal.closed || beforeForm.calls != 1) {
        return false;
      }

      {
        example::ReplayTerminal terminal(formScript, 200, {80, 24});
        ui::SceneOptions options;

        options.application = policies;
        options.clock = terminal.clock();

        gallery::FluentForm after(options, theme);
        auto result = after.run(terminal);

        if (!result || !terminal.closed || after.calls != beforeForm.calls ||
            result->frames != beforeFormStats->frames || terminal.output != beforeFormTerminal.output) {
          return false;
        }
      }

      const std::vector<example::ReplayTerminal::Input> meterScript{
        {50, "\x1b[C\x1b[C", {}}, {100, "\x1bOQ", CellSize{50, 8}}, {150, "\x1b[21~", {}}};
      example::ReplayTerminal beforeMeterTerminal(meterScript, 200, {80, 8});
      auto meterApp = example::take(Application::create(policies, beforeMeterTerminal.clock()));
      meter_example::MeterScene beforeMeter(*meterApp, theme);
      auto beforeMeterStats = beforeMeter.run(beforeMeterTerminal);

      if (!beforeMeterStats || !beforeMeterTerminal.closed) {
        return false;
      }

      {
        example::ReplayTerminal terminal(meterScript, 200, {80, 8});
        ui::SceneOptions options;

        options.application = policies;
        options.clock = terminal.clock();

        meter_example::FluentMeterScene after(options, theme);
        auto result = after.run(terminal);

        if (!result || !terminal.closed || result->frames != beforeMeterStats->frames ||
            terminal.output != beforeMeterTerminal.output) {
          return false;
        }
      }
    }
  }

  return true;
}
} // namespace

int main() {
  using namespace portals;

  example::ReplayTerminal terminal({}, 100, {32, 4});
  ui::SceneOptions options;
  ui::Ref<LineEdit> field;
  ui::Ref<Button> button;
  bool configured{};

  options.clock = terminal.clock();

  auto form = ui::scene(ui::column(ui::lineEdit(field, "initial"), ui::button(button, "Apply")), options);

  form.prepare().configure([&](ManagedScene &) {
    const auto initial = field.text().get();

    configured = initial && *initial == "initial";

    return button.onClick([field] {
      field.text() += "!";
    });
  });

  if (!configured || !form.checked() || !terminal.output.empty()) {
    return 1;
  }

  auto result = std::move(form) |
                ui::onKey(ui::key(InputKey::F2),
                          [field] {
                            return field.undo();
                          }) |
                ui::run(terminal);

  return result && terminal.closed && !field.text().get() && installedExampleEquivalence() ? 0 : 2;
}
