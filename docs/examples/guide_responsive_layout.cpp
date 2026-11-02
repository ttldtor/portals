// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/table_column.hpp>
#include <portals/text_format.hpp>
#include <portals/ui_menu.hpp>
#include <portals/ui_style.hpp>

#include <array>
#include <string>

/** @brief Resize a work queue while retaining its editor, actions and virtual rows.
 * @return Standard runner status after diagnostics and terminal cleanup.
 */
int main() {
  using namespace portals;

  struct Job {
    std::string name, owner, status;
    bool operator==(const Job &) const = default;
  };

  constexpr std::int32_t MIN_TABLE_WIDTH = 46;
  constexpr std::int32_t MIN_TABLE_HEIGHT = 6;
  constexpr std::int32_t MIN_NAME_WIDTH = 12;
  constexpr std::int32_t MAX_NAME_WIDTH = 40;

  // clang-format off
  return [=] {
    ui::Ref<LineEdit> name;
    ui::Ref<Label> status;
    ui::ActionSet actions;
    auto save = actions.action("save", "Save", [name, status] {
      status.text() = name.text() + " saved";
    });
    auto reset = actions.action("reset", "Reset", [name, status] {
      name.text() = "Morning queue";
      status.text() = "Ready";
    });
    auto exit = actions.action("exit", "Quit", ui::quit);

    auto scene = ui::scene([=](ManagedScene &owner) {
      const std::array<ModelRow<Job>, 12> rows{{
        {{1, 1}, {"Build SDK", "Ada", "Ready"}},
        {{2, 1}, {"Run unit tests", "Grace", "Running"}},
        {{3, 1}, {"Check headers", "Margaret", "Ready"}},
        {{4, 1}, {"Render guide", "Ada", "Waiting"}},
        {{5, 1}, {"Review examples", "Grace", "Running"}},
        {{6, 1}, {"Check packages", "Margaret", "Ready"}},
        {{7, 1}, {"Inspect resize", "Ada", "Ready"}},
        {{8, 1}, {"Test keyboard", "Grace", "Waiting"}},
        {{9, 1}, {"Verify cleanup", "Margaret", "Ready"}},
        {{10, 1}, {"Read diagnostics", "Ada", "Waiting"}},
        {{11, 1}, {"Prepare archive", "Grace", "Ready"}},
        {{12, 1}, {"Publish notes", "Margaret", "Waiting"}}
      }};
      auto columns = tableColumns(
        tableColumn("Job", &Job::name, cellFormat<std::string>("{}"), 20),
        tableColumn("Owner", &Job::owner, cellFormat<std::string>("{}"), 10),
        tableColumn("Status", &Job::status, cellFormat<std::string>("{}"), 10));

      return RowCollection<Job>::from(rows)
        .transform_error([](TreeError error) {
          return ManagedError{.phase = ManagedPhase::STATE, .cause = error, .application = {}, .cleanup = {}};
        })
        .and_then([&](auto collection) { return owner.state(std::move(collection)); })
        .and_then([&](auto state) {
          return owner.derive(state, [](const RowCollection<Job> &collection) { return collection; });
        })
        .and_then([&](auto source) { return owner.model(source); })
        .and_then([&](auto model) {
          return std::move(columns)
            .transform_error([](TextError error) {
              return ManagedError{.phase = ManagedPhase::MOUNT, .cause = TreeError::INVALID_ARGUMENT,
                                  .application = {}, .cleanup = {}, .text = error};
            })
            .transform([&](auto schema) {
              return ui::vbox(
                ui::hbox(ui::menu({save, reset, exit}).framed("rounded", "Queue"),
                         ui::spacing(2), ui::label("Resize | Ctrl+S: save | F10: quit")).fitHeight(),
                ui::hbox(ui::label("Name"), ui::spacing(1),
                         ui::lineEdit(name, "Morning queue")
                           .minWidth(MIN_NAME_WIDTH).maxWidth(MAX_NAME_WIDTH)).fitHeight(),
                ui::virtualTable(model, schema).framed("rounded", "Work queue")
                  .minWidth(MIN_TABLE_WIDTH).minHeight(MIN_TABLE_HEIGHT).flexY(4),
                ui::spacer(),
                ui::hbox(ui::button(save), ui::spacing(1), ui::button(reset), ui::spacing(1),
                         ui::button(exit), ui::spacer(), ui::label(status, "Ready")).fitHeight());
            });
        });
    });

    return std::move(scene)
      | ui::theme(nordTheme())
      | ui::onKey("Ctrl+S", save)
      | ui::onKey("F10 | Ctrl+C", exit);
  }
  | ui::runApp();
  // clang-format on
}
