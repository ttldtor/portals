// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "common.hpp"
#include "options.hpp"
#include <portals/input_match.hpp>
#include <portals/ui.hpp>
#include <stop_token>

namespace gallery {
/** @brief Equivalent spellings of the same owning scene configuration. */
enum class Composition {
  METHODS,
  PIPE
};

/** @brief Owning fluent form; the managed Form remains the compiled comparison specimen.
 * The object is stationary because callbacks borrow it. Scene closes before captured state dies.
 * Enter commits the editor; Tab changes focus; Run increments only the action count.
 */
class FluentForm {
public:
  /** @param options Existing runner/color policies and optional replay clock, borrowed through run.
   * @param theme Initial index into example::THEME_PRESETS; an invalid index throws out_of_range.
   * @param composition Method or pipe spelling; both register the same operations in the same order.
   * Recipe construction may allocate/throw; checked preparation and run preserve ManagedError.
   */
  explicit FluentForm(portals::ui::SceneOptions options = {}, std::size_t theme = 0,
                      Composition composition = Composition::METHODS)
      : theme_(theme), scene_(makeScene(options, composition)) {
  }

  FluentForm(const FluentForm &) = delete;
  FluentForm &operator=(const FluentForm &) = delete;
  FluentForm(FluentForm &&) = delete;
  FluentForm &operator=(FluentForm &&) = delete;

  /** @return Statistics or the original typed setup/runtime/cleanup failure after native restoration.
   * Admits a single run on the constructing UI thread.
   */
  std::expected<portals::ApplicationStats, portals::ManagedError> run() {
    return scene_.run(stop_.get_token());
  }

  /** @param terminal Borrowed transport opened and closed by the existing runner.
   * @return Statistics or typed primary, runner/restoration and independent cleanup failures.
   */
  std::expected<portals::ApplicationStats, portals::ManagedError> run(portals::ApplicationTerminal &terminal) {
    return scene_.run(terminal, stop_.get_token());
  }

  int calls{}; ///< Observable Run count; the action does not change the editor or checkbox model.

private:
  auto recipe(portals::ManagedScene &owner)
    -> std::expected<portals::ui::Description<portals::Column>, portals::ManagedError> {
    using namespace portals;

    return owner.state(false).transform([this](ManagedState<bool> alerts) {
      return ui::column(
        ui::label("Portals gallery | F2 theme | F10 quit"), ui::label("Name (Enter commits; Tab changes focus):"),
        ui::lineEdit(editor_, "Portals", {.options = {.editMode = EditMode::EXPLICIT}}), ui::checkbox("Alerts", alerts),
        ui::button("Run (no model change)",
                   [this] {
                     ++calls;
                   }),
        ui::button("Quit", [this] {
          stop_.request_stop();
        }));
    });
  }

  std::expected<void, portals::ManagedError> configure(portals::ManagedScene &owner) {
    using namespace portals;

    // The fallback preserves the original modifier-insensitive F2 and exit shortcuts.
    return owner.onInput([this, &owner](const InputEvent &event) -> std::expected<bool, ManagedError> {
      if (example::quitKey(event)) {
        stop_.request_stop();

        return true;
      }

      if (!keyPressed(event, InputKey::F2)) {
        return false;
      }

      const auto next = (theme_ + 1) % example::THEME_PRESETS.size();

      return owner.theme(example::THEME_PRESETS[next].make()).transform([this, next] {
        theme_ = next;

        return true;
      });
    });
  }

  portals::ui::Scene makeScene(portals::ui::SceneOptions options, Composition composition) {
    using namespace portals;

    auto factory = [this](ManagedScene &owner) {
      return recipe(owner);
    };
    auto configuration = [this](ManagedScene &owner) {
      return configure(owner);
    };
    auto quit = [this] {
      stop_.request_stop();
    };

    if (composition == Composition::PIPE) {
      return ui::scene(std::move(factory), options) | ui::theme(example::THEME_PRESETS.at(theme_).make()) |
             ui::configure(std::move(configuration)) | ui::onKey(ui::key(InputKey::F10), std::move(quit));
    }

    auto form = ui::scene(std::move(factory), options);

    form.theme(example::THEME_PRESETS.at(theme_).make())
      .configure(std::move(configuration))
      .onKey(ui::key(InputKey::F10), std::move(quit));

    return form;
  }

  std::size_t theme_{};                        ///< Current preset, changed only after successful replacement.
  std::stop_source stop_;                      ///< Existing cooperative runner cancellation, owned through cleanup.
  portals::ui::Ref<portals::LineEdit> editor_; ///< Weak editor observation published after complete mount.
  portals::ui::Scene scene_;                   ///< Owning runtime and callbacks; destroyed before borrowed members.
};
} // namespace gallery
