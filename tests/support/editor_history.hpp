// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "../../examples/editor_history/editor.hpp"
#include <iostream>
#include <stdexcept>

namespace editor_history_test {
using namespace portals;
constexpr std::size_t EDITOR_COUNT = editor_history::EDITOR_COUNT;
constexpr CellSize CHECK_VIEWPORT{96, 24};
constexpr std::uint64_t REPLAY_END_MS = 700;

static_assert(
  !std::is_constructible_v<EditorCommandResult, std::expected<void, TreeError>, std::weak_ptr<const void>, bool>);
static_assert(!std::is_convertible_v<EditorCommandResult, std::expected<void, TreeError>>);

/** @brief Print the original typed cause before a failing scripted assertion.
 * @tparam E Enum or public error envelope carrying a phase/cause or kind.
 * @param error Borrowed typed failure printed synchronously.
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
inline void expect(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

/** @brief Test-only observation and replay bindings around the real stationary public editor. */
class Demo {
public:
  /** @param options Runner policies; borrowed replay clock must outlive this owner. */
  explicit Demo(ui::SceneOptions options = {}) : demo_(options), rows_(demo_.rows()) {
    demo_.scene()
      .configure([this](ManagedScene &scene, Application &app) {
        scene_ = &scene;
        app_ = &app;
      })
      .onKey(ui::key(InputKey::F1), [this] {
        if (checkpoint_) {
          checkpoint_();
        }
      });
  }

  Demo(const Demo &) = delete;
  Demo &operator=(const Demo &) = delete;
  Demo(Demo &&) = delete;
  Demo &operator=(Demo &&) = delete;

  /** @param terminal Optional borrowed replay terminal.
   * @return Runner status, or one if scripted execution left recoverable diagnostics.
   */
  int run(ApplicationTerminal *terminal = nullptr) {
    const auto status = terminal ? demo_.scene().runApp(*terminal) : demo_.scene().runApp();
    const auto diagnostics = checked(demo_.scene().diagnostics(), "inspect public editor diagnostics");

    expect(diagnostics.errors.empty() && !diagnostics.droppedErrors && !diagnostics.observerFailures,
           "public editor must leave diagnostics clean");

    return status;
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
      checked(scene_->prepare(*app_, CHECK_VIEWPORT), "prepare visible model rows");
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
    checked(demo_.scene().prepareChecked(), "prepare public editor");

    for (std::size_t index = 0; index < EDITOR_COUNT; ++index) {
      auto &row = rows_[index];
      const auto immediate = index == 0;

      checked(app_->router().setFocus(row.edit.node()), "focus checked editor");
      checked(app_->router().invoke(row.append.node(), {Action::ACTIVATE}), "invoke fluent Append button");
      verify(index, "base!", immediate ? "base!" : "base");
      expect(row.edit.canUndo(), "Append must be reversible");
      checked(app_->router().invoke(row.undo.node(), {Action::ACTIVATE}), "invoke compact Undo button");
      verify(index, "base", "base");
      expect(row.edit.canRedo(), "Undo must expose Redo");
      checked(app_->router().invoke(row.redo.node(), {Action::ACTIVATE}), "invoke Redo button");
      verify(index, "base!", immediate ? "base!" : "base");
      checked(app_->router().dispatch(KeyEvent{InputKey::ENTER}), "confirm through Enter");
      verify(index, "base!", "base!");
      checked(app_->router().dispatch(KeyEvent{InputKey::CHARACTER, U'z', {.control = true}}), "route Ctrl+Z");
      verify(index, "base", immediate ? "base" : "base!");
      checked(app_->router().dispatch(KeyEvent{InputKey::ESCAPE}), "route Escape");
      verify(index, immediate ? "base" : "base!", immediate ? "base" : "base!");

      checked((row.edit.text(ProgrammaticChange::RESET_BASELINE) = "base").result(), "load RESET baseline");
      checked(app_->router().dispatch(KeyEvent{InputKey::ENTER}), "confirm reset baseline");
      expect(!row.edit.canUndo() && !row.edit.canRedo(), "RESET must clear history");
      checked(app_->router().setFocus(row.view.node()), "focus read-only model row");
      checked(app_->router().dispatch(TextInputEvent{U'x'}), "route read-only typing");
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
  editor_history::Demo demo_;
  const std::array<editor_history::Demo::Row, EDITOR_COUNT> &rows_;
  ManagedScene *scene_{};
  Application *app_{};
  std::function<void()> checkpoint_;
};
} // namespace editor_history_test
