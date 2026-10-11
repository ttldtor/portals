// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../replay/terminal.hpp"

#include <portals/input_match.hpp>
#include <portals/ui.hpp>

#include <array>
#include <format>
#include <iostream>
#include <stdexcept>

namespace {
using namespace portals;

constexpr std::size_t EDITOR_COUNT = 3;
constexpr CellSize CHECK_VIEWPORT{96, 24};
constexpr std::uint64_t REPLAY_END_MS = 700;
constexpr std::array MODE_NAMES{"IMMEDIATE (default)", "ON_COMPLETION (blur / Enter)", "EXPLICIT (Enter only)"};
constexpr std::array MODES{EditMode::IMMEDIATE, EditMode::ON_COMPLETION, EditMode::EXPLICIT};

static_assert(
  !std::is_constructible_v<EditorCommandResult, std::expected<void, TreeError>, std::weak_ptr<const void>, bool>);
static_assert(!std::is_convertible_v<EditorCommandResult, std::expected<void, TreeError>>);

/** @param status Exact editor outcome to explain.
 * @return Static human-readable outcome, including an unknown fallback.
 */
std::string_view statusName(LineEditStatus status) {
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

/** @brief Print original typed errors, including nested runner and restoration failures.
 * @tparam E Public error value with an enum identity or documented error fields.
 * @param error Borrowed failure, inspected only during this call.
 * @return No value; stderr follows its configured stream policy.
 */
template <class E> void printError(const E &error) {
  if constexpr (std::is_enum_v<E>) {
    std::cerr << static_cast<int>(error);
  } else if constexpr (requires {
                         error.phase;
                         error.cause;
                       }) {
    std::cerr << "phase=" << static_cast<int>(error.phase) << " cause=";
    std::visit(
      [](const auto &cause) {
        printError(cause);
      },
      error.cause);

    if constexpr (requires { error.cleanup; }) {
      if (error.cleanup) {
        std::cerr << " cleanup=";
        printError(*error.cleanup);
      }

      if (error.application) {
        std::cerr << " runner=";
        printError(*error.application);
      }
    } else if (error.restoration) {
      std::cerr << " restoration=";
      printError(*error.restoration);
    }
  } else if constexpr (requires {
                         error.cause;
                         error.message;
                       }) {
    printError(error.cause);
    std::cerr << ' ' << error.message;
  } else {
    std::cerr << "kind=" << static_cast<int>(error.kind);
  }
}

/** @brief Stop finite setup/check operations after printing their original typed failure.
 * Runtime callbacks return checked results directly; this helper is not their error policy.
 * @tparam T Void or movable owning success value.
 * @tparam E Public error accepted by printError.
 * @param result Owning checked operation result; argument construction can throw before entry.
 * @param operation Static or borrowed operation description, used synchronously in diagnostics.
 * @return Moved success value, or no value for void.
 * @throws std::runtime_error After the original typed cause has been printed on failure.
 */
template <class T, class E> T checked(std::expected<T, E> result, std::string_view operation) {
  if (!result) {
    std::cerr << operation << ": ";
    printError(result.error());
    std::cerr << '\n';

    throw std::runtime_error(std::string(operation));
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

/** @brief Require a behavioral assertion in the headless public-API check.
 * @param condition Actual observed contract result.
 * @param message Owning diagnostic context is copied only on failure.
 * @return No value; failure throws with the specific assertion context.
 */
void expect(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

/** @brief Three editor modes using one existing managed scene and ordinary state/bindings. */
class Demo {
public:
  /** @param app Borrowed UI application, outliving this scene and all callbacks. */
  explicit Demo(Application &app) : app_(app), scene_(checked(ManagedScene::create(app), "create scene")) {
    for (auto &row : rows_) {
      row.model = checked(scene_->state(std::string("base")), "create editor model");
    }

    outcome_ = checked(scene_->state(std::string("Type in a draft; compare the read-only model below it.")),
                       "create outcome model");
    checked(ui::mount(*scene_,
                      ui::column(ui::label("Editor history | F5/F6/F7 choose mode | F10 quit"),
                                 ui::label("Type; Ctrl+Z Undo, Ctrl+Y Redo. Model rows are read-only."), buildRow(0),
                                 buildRow(1), buildRow(2), ui::label("Last editor result:"),
                                 ui::lineEdit(outcome_, {.options = {.history = {.enabled = false}, .readOnly = true}}),
                                 ui::label("F3 or Append adds ! with field.text() += \"!\" (one history operation)."),
                                 ui::label("Enter confirms; Tab/click blurs. EXPLICIT keeps its draft on blur."),
                                 ui::label("Escape adopts model; IMMEDIATE does not undo already published changes."))),
            "mount editor history demo");

    for (const auto &row : rows_) {
      checked(scene_->onClick(row.redo.node(),
                              [field = row.edit] {
                                return field.redo();
                              }),
              "bind late Redo");
    }

    checked(scene_->theme(modernTheme()), "apply default theme");
    checked(app_.router().setFocus(rows_[0].edit.node()), "focus immediate editor");
    checked(scene_->onInput([this](const InputEvent &event) -> std::expected<bool, ManagedError> {
      if (keyPressed(event, InputKey::F10)) {
        app_.requestStop();

        return true;
      }

      for (std::size_t index = 0; index < EDITOR_COUNT; ++index) {
        const auto key = std::array{InputKey::F5, InputKey::F6, InputKey::F7}[index];

        if (keyPressed(event, key)) {
          const auto focused = app_.router().setFocus(rows_[index].edit.node());

          if (!focused) {
            return std::unexpected(
              ManagedError{ManagedPhase::INPUT, rows_[index].edit.node().id(), focused.error(), {}, {}});
          }

          return true;
        }
      }

      if (keyPressed(event, InputKey::F3)) {
        const auto focused = app_.router().focused();

        if (!focused) {
          return std::unexpected(ManagedError{ManagedPhase::INPUT, {}, focused.error(), {}, {}});
        }

        for (const auto &row : rows_) {
          if (*focused == row.edit.node()) {
            // This proxy records its own typed failure in the existing scene collector.
            // Do not return the same already reported failure through another adapter.
            const auto appended = (row.edit.text() += "!").result();

            if (!appended) {
              lastAppendFailure_ = appended.error();
            }

            return true;
          }
        }
      }

      if (checkpoint_ && keyPressed(event, InputKey::F1)) {
        checkpoint_();

        return true;
      }

      return false;
    }),
            "bind demo shortcuts");
  }

  /** @param terminal Optional borrowed replay transport; null selects the native terminal.
   * @return Zero after a clean run and collector, one after any typed failure; output follows restoration.
   */
  int run(ApplicationTerminal *terminal = nullptr) {
    const auto result = terminal ? scene_->run(*terminal) : scene_->run();

    if (!result) {
      std::cerr << "run: ";
      printError(result.error());
      std::cerr << '\n';
    }

    const auto diagnostics = scene_->diagnostics();

    if (!diagnostics) {
      std::cerr << "read diagnostics: ";
      printError(diagnostics.error());
      std::cerr << '\n';

      return 1;
    }

    for (const auto &diagnostic : diagnostics->errors) {
      std::cerr << "collector: ";
      printError(diagnostic.error);

      for (const auto &context : diagnostic.contexts) {
        std::cerr << " [" << context.operation << ']';
      }

      std::cerr << '\n';
    }

    if (diagnostics->droppedErrors || diagnostics->observerFailures) {
      std::cerr << "collector overflow=" << diagnostics->droppedErrors
                << " observer failures=" << diagnostics->observerFailures << '\n';
    }

    if (notificationFailure_) {
      std::cerr << "committed value retained; notification delivery failed: ";
      printError(*notificationFailure_);
      std::cerr << '\n';
    }

    return result && diagnostics->errors.empty() && !diagnostics->droppedErrors && !diagnostics->observerFailures &&
               !lastAppendFailure_ && !notificationFailure_
             ? 0
             : 1;
  }

  /** @brief Check draft, published value and the visible read-only model after normal preparation.
   * @param index Mode row in [0, EDITOR_COUNT).
   * @param draft Expected owning visible draft bytes.
   * @param model Expected published model bytes and model-view bytes.
   * @param synchronize Prepare between direct checks; false inspects the preceding replay frame without reentry.
   * @return No value; a precise mismatch or typed operation failure throws.
   */
  void verify(std::size_t index, std::string_view draft, std::string_view model, bool synchronize = true) {
    if (synchronize) {
      checked(scene_->prepare(app_, CHECK_VIEWPORT), "prepare visible model rows");
    }

    expect(checked(rows_.at(index).edit.text().get(), "read visible draft") == draft, "visible draft mismatch");
    expect(checked(rows_.at(index).model.snapshot(), "read model")->value == model, "published model mismatch");
    expect(checked(rows_.at(index).view.text().get(), "read visible model row") == model,
           "read-only model row mismatch");
  }

  /** @brief Exercise buttons, keyboard, fluent policy, modes and read-only views without a native session.
   * @return No value; each failed behavioral check or typed result throws.
   */
  void checkContracts() {
    for (std::size_t index = 0; index < EDITOR_COUNT; ++index) {
      auto &row = rows_[index];
      const auto immediate = index == 0;

      checked(app_.router().setFocus(row.edit.node()), "focus checked editor");
      checked(app_.router().invoke(row.append.node(), {Action::ACTIVATE}), "invoke fluent Append button");
      verify(index, "base!", immediate ? "base!" : "base");
      expect(row.edit.canUndo(), "Append must be reversible");
      checked(app_.router().invoke(row.undo.node(), {Action::ACTIVATE}), "invoke compact Undo button");
      verify(index, "base", "base");
      expect(row.edit.canRedo(), "Undo must expose Redo");
      checked(app_.router().invoke(row.redo.node(), {Action::ACTIVATE}), "invoke late Redo button");
      verify(index, "base!", immediate ? "base!" : "base");
      checked(app_.router().dispatch(KeyEvent{InputKey::ENTER}), "confirm through Enter");
      verify(index, "base!", "base!");
      checked(app_.router().dispatch(KeyEvent{InputKey::CHARACTER, U'z', {.control = true}}), "route Ctrl+Z");
      verify(index, "base", immediate ? "base" : "base!");
      checked(app_.router().dispatch(KeyEvent{InputKey::ESCAPE}), "route Escape");
      verify(index, immediate ? "base" : "base!", immediate ? "base" : "base!");

      checked((row.edit.text(ProgrammaticChange::RESET_BASELINE) = "base").result(), "load RESET baseline");
      checked(app_.router().dispatch(KeyEvent{InputKey::ENTER}), "confirm reset baseline");
      expect(!row.edit.canUndo() && !row.edit.canRedo(), "RESET must clear history");
      checked(app_.router().setFocus(row.view.node()), "focus read-only model row");
      checked(app_.router().dispatch(TextInputEvent{U'x'}), "route read-only typing");
      verify(index, "base", "base");
      expect(!row.view.canUndo() && !row.view.canRedo(), "read-only model row has no effective history commands");
    }

    const auto diagnostics = checked(scene_->diagnostics(), "inspect checked collector");

    expect(diagnostics.errors.empty() && diagnostics.droppedErrors == 0, "contract check must leave collector clean");
  }

  /** @param callback Owning headless checkpoint, called synchronously by the normal F1 input intercept. */
  void checkpoint(std::function<void()> callback) {
    checkpoint_ = std::move(callback);
  }

  /** @return Owning three model values; usable after run closed the scene's mounted subtree. */
  std::array<std::string, EDITOR_COUNT> values() const {
    std::array<std::string, EDITOR_COUNT> result;

    for (std::size_t index = 0; index < EDITOR_COUNT; ++index) {
      result[index] = checked(rows_[index].model.snapshot(), "read final model")->value;
    }

    return result;
  }

private:
  struct Row {
    ManagedState<std::string> model;
    ui::Ref<LineEdit> edit;
    ui::Ref<LineEdit> view;
    ui::Ref<Button> undo;
    ui::Ref<Button> redo;
    ui::Ref<Button> append;
  };

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

    return ui::column(
      ui::label(MODE_NAMES[index]),
      ui::row(ui::label("Draft:"), ui::lineEdit(row.model, std::move(config)).reference(row.edit)),
      ui::row(ui::label("Model:"),
              ui::lineEdit(row.model, {.options = {.history = {.enabled = false}, .readOnly = true},
                                       .policy = {.width = {.minimum = 12, .preferred = 24, .mode = SizeMode::FLEX},
                                                  .height = {}}})
                .reference(row.view)),
      ui::row(ui::button("Undo",
                         [field = row.edit] {
                           return field.undo();
                         })
                .reference(row.undo),
              ui::control<Button>("Redo", {}, {}).reference(row.redo),
              ui::button("Append !", [this, field = row.edit]() -> std::expected<void, TreeError> {
                const auto result = (field.text() += "!").result();

                if (!result) {
                  lastAppendFailure_ = result.error();
                }

                // The text proxy reports itself; the collector controls runner stop policy.
                return {};
              }).reference(row.append)));
  }

  Application &app_;
  std::array<Row, EDITOR_COUNT> rows_;
  ManagedState<std::string> outcome_;
  std::optional<ManagedError> lastAppendFailure_;
  std::optional<SchedulerError> notificationFailure_;
  std::function<void()> checkpoint_;
  std::unique_ptr<ManagedScene> scene_;
};

/** @return Finite native VT input through the existing replay terminal; no synthetic binding is used. */
std::vector<example::ReplayTerminal::Input> replayInput() {
  return {{20, "A", {}},       {40, "\x1bOP", {}},  {60, "\x1a", {}},    {80, "\x1bOP", {}},    {100, "\x19", {}},
          {120, "\x1bOP", {}}, {140, "\x1bOR", {}}, {160, "\x1bOP", {}}, {180, "\x1b[17~", {}}, {200, "B", {}},
          {220, "\x1bOP", {}}, {240, "\t", {}},     {260, "\x1bOP", {}}, {280, "\x1b[18~", {}}, {300, "C", {}},
          {320, "\x1bOP", {}}, {340, "\t", {}},     {360, "\x1bOP", {}}, {380, "\x1b[18~", {}}, {400, "\r", {}},
          {420, "\x1bOP", {}}, {440, "\x1a", {}},   {460, "\x1bOP", {}}, {480, "\x19", {}},     {500, "\x1bOP", {}},
          {520, "\x1bOR", {}}, {540, "\x1b", {}},   {600, "\x1bOP", {}}, {640, "\x1b[21~", {}}};
}
} // namespace

/** @brief Run the three-mode editor interactively or through real finite input replay.
 * @param argc Argument count, including the program name.
 * @param argv Borrowed arguments, used only during option parsing.
 * @return Zero after success/help, two for unsupported options, one for any check or typed runtime failure.
 */
int main(int argc, char **argv) {
  bool check{};
  bool replay{};

  for (int index = 1; index < argc; ++index) {
    const std::string_view option = argv[index];

    if (option == "--check") {
      check = true;
      replay = true;
    } else if (option == "--replay") {
      replay = true;
    } else if (option == "--help") {
      std::cout << "Usage: portals_editor_history [--check | --replay | --help]\n"
                   "F5/F6/F7 select IMMEDIATE/ON_COMPLETION/EXPLICIT. F3 appends !.\n"
                   "Ctrl+Z Undo, Ctrl+Y Redo; Enter confirms, Escape adopts model; Tab blurs.\n"
                   "F10 exits. --check verifies buttons, visible model rows and finite VT replay.\n";

      return 0;
    } else {
      std::cerr << "Unknown option: " << option << '\n';

      return 2;
    }
  }

  try {
    if (check) {
      auto app = checked(Application::create(), "create contract-check application");
      Demo demo(*app);

      demo.checkContracts();
    }

    example::ReplayTerminal terminal(replayInput(), REPLAY_END_MS, CHECK_VIEWPORT);
    auto app = checked(Application::create({}, replay ? terminal.clock() : SchedulerClock{}), "create application");
    Demo demo(*app);
    std::size_t checkpoints{};

    if (check) {
      demo.checkpoint([&] {
        switch (checkpoints++) {
        case 0:
          demo.verify(0, "baseA", "baseA", false);
          break;
        case 1:
          demo.verify(0, "base", "base", false);
          break;
        case 2:
          demo.verify(0, "baseA", "baseA", false);
          break;
        case 3:
          demo.verify(0, "baseA!", "baseA!", false);
          break;
        case 4:
          demo.verify(1, "baseB", "base", false);
          break;
        case 5:
          demo.verify(1, "baseB", "baseB", false);
          break;
        case 6:
          demo.verify(2, "baseC", "base", false);
          break;
        case 7:
          demo.verify(2, "baseC", "base", false);
          break;
        case 8:
          demo.verify(2, "baseC", "baseC", false);
          break;
        case 9:
          demo.verify(2, "base", "baseC", false);
          break;
        case 10:
          demo.verify(2, "baseC", "baseC", false);
          break;
        case 11:
          demo.verify(2, "baseC", "baseC", false);
          break;
        default:
          throw std::runtime_error("unexpected replay checkpoint");
        }
      });
    }

    const auto result = demo.run(replay ? &terminal : nullptr);

    if (result) {
      return result;
    }

    if (check) {
      expect(checkpoints == 12, "all twelve replay checkpoints must run");
      expect(terminal.closed && !terminal.output.empty(), "replay must render output and close its transport");
      expect(demo.values() == std::array<std::string, EDITOR_COUNT>{"baseA!", "baseB", "baseC"},
             "final replay models must preserve the three confirmation policies");
      std::cout << "editor history: buttons, read-only model rows, three modes and 12 VT checkpoints passed\n";
    } else if (replay) {
      const auto values = demo.values();

      std::cout << "Replay models: " << values[0] << " | " << values[1] << " | " << values[2] << '\n';
    }

    return 0;
  } catch (const std::exception &error) {
    std::cerr << "editor history demo: " << error.what() << '\n';

    return 1;
  }
}
