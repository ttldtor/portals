// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui.hpp>

#include <array>
#include <format>

namespace editor_history {
using namespace portals;

constexpr std::size_t EDITOR_COUNT = 3;
constexpr std::array MODE_NAMES{"IMMEDIATE (default)", "ON_COMPLETION (blur / Enter)", "EXPLICIT (Enter only)"};
constexpr std::array MODES{EditMode::IMMEDIATE, EditMode::ON_COMPLETION, EditMode::EXPLICIT};

/** @param status Exact editor outcome to explain.
 * @return Static human-readable outcome, including an unknown fallback.
 */
inline std::string_view statusName(LineEditStatus status) {
  switch (status) {
  case LineEditStatus::EDITED:
    return "draft or selection changed";
  case LineEditStatus::COMMITTED:
    return "model accepted";
  case LineEditStatus::CANCELLED:
    return "model adopted; history reset";
  case LineEditStatus::COPIED:
    return "selection copied";
  case LineEditStatus::CONFLICT:
    return "external model conflict; press Escape to adopt";
  case LineEditStatus::INVALID_TEXT:
    return "invalid text rejected";
  case LineEditStatus::LIMIT_EXCEEDED:
    return "whole edit exceeds a limit";
  case LineEditStatus::VALIDATION_FAILED:
    return "visible draft rejected by validator";
  case LineEditStatus::CLIPBOARD_FAILED:
    return "clipboard service failed";
  case LineEditStatus::FAILED:
    return "technical failure; see diagnostics after exit";
  }

  return "unknown editor outcome";
}

/** @brief Three editor modes using one owning lazy scene and ordinary state bindings. */
class Demo {
public:
  /** @param options Existing runner policies and optional borrowed custom-terminal clock. */
  explicit Demo(ui::SceneOptions options = {})
      : scene_(ui::scene(
          [this](ManagedScene &owner, Application &app) -> std::expected<ui::Description<Column>, ManagedError> {
            app_ = &app;

            for (auto &row : rows_) {
              auto model = owner.state(std::string("base"));

              if (!model) {
                return std::unexpected(model.error());
              }

              row.model = *model;
            }

            auto outcome = owner.state(std::string("Type in a draft; compare the read-only model below it."));

            if (!outcome) {
              return std::unexpected(outcome.error());
            }

            outcome_ = *outcome;

            return ui::vbox(ui::label("Editor history | F5/F6/F7 choose mode | F10 quit"),
                            ui::label("Type; Ctrl+Z Undo, Ctrl+Y Redo. Model rows are read-only."), buildRow(0),
                            buildRow(1), buildRow(2), ui::label("Last editor result:"),
                            ui::lineEdit(outcome_, {.options = {.history = {.enabled = false}, .readOnly = true}}),
                            ui::label("F3 or Append adds ! with field.text() += \"!\" (one history operation)."),
                            ui::label("Enter confirms; Tab/click blurs. EXPLICIT keeps its draft on blur."),
                            ui::label("Escape adopts model; IMMEDIATE does not undo already published changes."));
          },
          options)) {
    scene_.theme(modernTheme())
      .configure([this](ManagedScene &) {
        return focus(0).transform_error([this](TreeError error) {
          return ManagedError{ManagedPhase::BIND, rows_[0].edit.node().id(), error, {}, {}};
        });
      })
      .onKey(ui::key(InputKey::F10),
             [] {
               ui::quit();
             })
      .onKey(ui::key(InputKey::F3), [this] {
        return appendFocused();
      });

    for (std::size_t index = 0; index < EDITOR_COUNT; ++index) {
      scene_.onKey(ui::key(std::array{InputKey::F5, InputKey::F6, InputKey::F7}[index]), [this, index] {
        return focus(index);
      });
    }
  }

  Demo(const Demo &) = delete;
  Demo &operator=(const Demo &) = delete;
  Demo(Demo &&) = delete;
  Demo &operator=(Demo &&) = delete;

  /** @return Borrowed stationary scene for launch policies and additional application bindings. */
  ui::Scene &scene() {
    return scene_;
  }

  /** @brief Model and observing controls for one edit policy; the scene owns their storage. */
  struct Row {
    ManagedState<std::string> model; ///< Published value shared by the draft and model view.
    ui::Ref<LineEdit> edit;          ///< Editable draft with undo history.
    ui::Ref<LineEdit> view;          ///< Read-only view of the published model.
    ui::Ref<Button> undo;            ///< Undo action for this draft.
    ui::Ref<Button> redo;            ///< Redo action for this draft.
    ui::Ref<Button> append;          ///< Append one reversible exclamation mark.
  };

  /** @return Borrowed editor state in IMMEDIATE, ON_COMPLETION and EXPLICIT order.
   * References become usable after scene preparation and must not outlive this stationary owner.
   */
  const std::array<Row, EDITOR_COUNT> &rows() const {
    return rows_;
  }

private:
  std::expected<void, TreeError> focus(std::size_t index) {
    return app_->router().setFocus(rows_[index].edit.node());
  }

  std::expected<void, TreeError> appendFocused() {
    const auto focused = app_->router().focused();

    if (!focused) {
      return std::unexpected(focused.error());
    }

    for (const auto &row : rows_) {
      if (*focused == row.edit.node()) {
        row.edit.text() += "!";

        break;
      }
    }

    return {};
  }

  ui::Description<Column> buildRow(std::size_t index) {
    auto &row = rows_[index];
    ui::LineEditConfig config;

    // IMMEDIATE uses the real default; only the two explicit alternatives set an option.
    if (index != 0) {
      config.options.editMode = MODES[index];
    }

    config.policy.width = {.minimum = 12, .preferred = 24, .mode = SizeMode::FLEX};
    config.result = [this, index](LineEditResult result) -> std::expected<void, TreeError> {
      if (result.commit && result.commit->notification_error) {
        notificationFailure_ = result.commit->notification_error;
      }

      const auto status = std::format("{}: {}{}", MODE_NAMES[index], statusName(result.status),
                                      notificationFailure_ ? " (notification delivery failed)" : "");
      const auto changed = outcome_.set(status);

      if (changed && changed->notification_error) {
        notificationFailure_ = changed->notification_error;
      }

      return changed ? std::expected<void, TreeError>{} : std::unexpected(changed.error());
    };

    return ui::vbox(
      ui::label(MODE_NAMES[index]), ui::hbox(ui::label("Draft:"), ui::lineEdit(row.edit, row.model, std::move(config))),
      ui::hbox(
        ui::label("Model:"),
        ui::lineEdit(row.view, row.model,
                     {.options = {.history = {.enabled = false}, .readOnly = true},
                      .policy = {.width = {.minimum = 12, .preferred = 24, .mode = SizeMode::FLEX}, .height = {}}})),
      ui::hbox(ui::button(row.undo, "Undo",
                          [field = row.edit] {
                            return field.undo();
                          }),
               ui::button(row.redo, "Redo",
                          [field = row.edit] {
                            return field.redo();
                          }),
               ui::button(row.append, "Append !", [field = row.edit] {
                 field.text() += "!";
               })));
  }

  Application *app_{};
  std::array<Row, EDITOR_COUNT> rows_;
  ManagedState<std::string> outcome_;
  std::optional<SchedulerError> notificationFailure_;
  ui::Scene scene_;
};

} // namespace editor_history
