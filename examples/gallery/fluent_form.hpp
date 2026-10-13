// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "common.hpp"
#include "options.hpp"
#include <portals/input_match.hpp>
#include <portals/ui_style.hpp>

namespace gallery {
inline constexpr auto FOCUS_POLICY = portals::style::ct::pss("Button:focused { bold: true; }", "gallery-focus.pss");
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
                      Composition composition = Composition::PIPE)
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
    return scene_.run({});
  }

  /** @param terminal Borrowed transport opened and closed by the existing runner.
   * @return Statistics or typed primary, runner/restoration and independent cleanup failures.
   */
  std::expected<portals::ApplicationStats, portals::ManagedError> run(portals::ApplicationTerminal &terminal) {
    return scene_.run(terminal, {});
  }

  int calls{}; ///< Observable Run count; the action does not change the editor or checkbox model.

  /** @return Zero on success, one on a typed failure, with library reporting after terminal restoration. */
  int runApp() {
    return portals::ui::runApp(scene_);
  }

  /** @param terminal Borrowed replay/custom transport, living through this call.
   * @return Same final status and library reporting as the native run.
   */
  int runApp(portals::ApplicationTerminal &terminal) {
    return scene_ | portals::ui::runApp(terminal);
  }

private:
  auto recipe(portals::ManagedScene &owner)
    -> std::expected<portals::ui::Description<portals::Column>, portals::ManagedError> {
    using namespace portals;

    return owner.state(false).transform([this](ManagedState<bool> alerts) {
      return ui::vbox(
        ui::label("Portals gallery | F2 theme | F10 quit"), ui::label("Name (Enter commits; Tab changes focus):"),
        ui::lineEdit(editor_, "Portals", {.options = {.editMode = EditMode::EXPLICIT}}), ui::checkbox("Alerts", alerts),
        ui::button(runButton_, "Run (no model change)"), ui::button(quitButton_, "Quit"));
    });
  }

  std::expected<void, portals::ManagedError> configure(portals::ManagedScene &owner) {
    using namespace portals;

    // The fallback preserves the original modifier-insensitive F2 and exit shortcuts.
    return owner.onInput([this, &owner](const InputEvent &event) -> std::expected<bool, ManagedError> {
      if (example::quitKey(event)) {
        (void)scene_.quit();

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
      (void)scene_.quit();
    };
    auto form = ui::scene(std::move(factory), options);

    if (composition == Composition::PIPE) {
      form | ui::theme(example::THEME_PRESETS.at(theme_).make()) | ui::style(FOCUS_POLICY, {.name = "gallery-focus"}) |
        ui::configure(std::move(configuration)) | ui::onKey(key::F10, std::move(quit));
    } else {
      form.theme(example::THEME_PRESETS.at(theme_).make())
        .style(FOCUS_POLICY, {.name = "gallery-focus"})
        .configure(std::move(configuration))
        .onKey(key::F10, std::move(quit));
    }

    return std::move(form) |
           ui::onClick(runButton_,
                       [this](const ui::ClickParams &params) {
                         if (params.mods == InputModifiers{}) {
                           ++calls;
                         }
                       }) |
           ui::onClick(quitButton_, [this](const ui::ClickParams &params) {
             if (params.mods == InputModifiers{}) {
               (void)scene_.quit();
             }
           });
  }

  std::size_t theme_{};                          ///< Current preset, changed only after successful replacement.
  portals::ui::Ref<portals::LineEdit> editor_;   ///< Weak editor observation published after complete mount.
  portals::ui::Ref<portals::Button> runButton_;  ///< Run target bound during mount, before late handlers execute.
  portals::ui::Ref<portals::Button> quitButton_; ///< Quit target; scene owns its handler and cleanup.
  portals::ui::Scene scene_;                     ///< Owning runtime and callbacks; destroyed before borrowed members.
};
} // namespace gallery
