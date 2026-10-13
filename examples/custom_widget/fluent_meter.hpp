// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "meter.hpp"
#include <portals/ui_style.hpp>

namespace meter_example {
inline constexpr auto FOCUS_POLICY = portals::style::ct::pss("Meter:focused { bold: true; }", "meter-focus.pss");
/** @brief Equivalent method and pipe composition spellings. */
enum class Composition {
  METHODS,
  PIPE
};

/** @brief Owning fluent scene for the same final LevelMeter extension.
 * State changes invalidate the existing scene root. Preparation consumes its latest snapshot;
 * paint uses retained presentation. Callbacks borrow this stationary object until scene cleanup.
 */
class FluentMeterScene {
public:
  /** @param options Existing runner/color policies; a replay clock must outlive the run.
   * @param theme Initial example::THEME_PRESETS index; invalid indices throw out_of_range.
   * @param composition Method or pipe spelling with identical registration order and behavior.
   * Construction can allocate/throw; checked lazy setup and run retain their original typed errors.
   */
  explicit FluentMeterScene(portals::ui::SceneOptions options = {}, std::size_t theme = 0,
                            Composition composition = Composition::METHODS)
      : theme_(theme), scene_(makeScene(options, composition)) {
  }

  FluentMeterScene(const FluentMeterScene &) = delete;
  FluentMeterScene &operator=(const FluentMeterScene &) = delete;
  FluentMeterScene(FluentMeterScene &&) = delete;
  FluentMeterScene &operator=(FluentMeterScene &&) = delete;

  /** @return Statistics or typed lazy setup/runtime/cleanup failure after native restoration. */
  std::expected<ApplicationStats, ManagedError> run() {
    return scene_.run({});
  }

  /** @param terminal Borrowed transport opened and closed by the existing runner.
   * @return Statistics or original typed cause plus independent runner/restoration and cleanup errors.
   */
  std::expected<ApplicationStats, ManagedError> run(ApplicationTerminal &terminal) {
    return scene_.run(terminal, {});
  }

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
  auto recipe(ManagedScene &owner) -> std::expected<ui::Description<LevelMeter>, ManagedError> {
    auto level = owner.state(INITIAL_LEVEL, Invalidation::PAINT);

    if (!level) {
      return std::unexpected(level.error());
    }

    level_ = *level;

    return ui::custom(meter_)
      .style({.type = "Meter"})
      .onPrepare([level = level_](LevelMeter &meter, Node<Widget>) -> std::expected<void, TreeError> {
        // The state invalidates the root on change; no additional invalidation inside prepare is needed.
        return level.snapshot()
          .and_then([&meter](const auto &snapshot) {
            return meter.prepare(snapshot->value);
          })
          .transform([](bool) {
          });
      });
  }

  std::expected<void, TreeError> adjust(int step) {
    return level_.snapshot()
      .and_then([this, step](const auto &snapshot) {
        return level_.set(std::clamp(snapshot->value + step, MIN_LEVEL, MAX_LEVEL));
      })
      .transform([](const auto &) {
      });
  }

  std::expected<void, ManagedError> configure(ManagedScene &owner) {
    // Retain modifier-insensitive legacy admission as the fallback beneath exact onKey chords.
    return owner.onInput([this, &owner](const InputEvent &event) -> std::expected<bool, ManagedError> {
      if (example::quitKey(event)) {
        (void)scene_.quit();

        return true;
      }

      if (keyPressed(event, InputKey::F2)) {
        const auto next = (theme_ + 1) % example::THEME_PRESETS.size();

        return owner.theme(example::THEME_PRESETS[next].make()).transform([this, next] {
          theme_ = next;

          return true;
        });
      }

      if (!keyPressed(event, {InputKey::LEFT, InputKey::RIGHT})) {
        return false;
      }

      const auto step = keyPressed(event, InputKey::RIGHT) ? LEVEL_STEP : -LEVEL_STEP;

      return adjust(step)
        .transform([] {
          return true;
        })
        .transform_error([this](TreeError cause) {
          return ManagedError{
            .phase = ManagedPhase::INPUT, .node = meter_.node().id(), .cause = cause, .application = {}, .cleanup = {}};
        });
    });
  }

  ui::Scene makeScene(ui::SceneOptions options, Composition composition) {
    auto factory = [this](ManagedScene &owner) {
      return recipe(owner);
    };
    auto configuration = [this](ManagedScene &owner) {
      return configure(owner);
    };
    auto quit = [this] {
      (void)scene_.quit();
    };
    auto left = [this] {
      return adjust(-LEVEL_STEP);
    };
    auto right = [this] {
      return adjust(LEVEL_STEP);
    };

    if (composition == Composition::PIPE) {
      return ui::scene(std::move(factory), options) | ui::theme(example::THEME_PRESETS.at(theme_).make()) |
             ui::style(FOCUS_POLICY, {.name = "meter-focus"}) | ui::configure(std::move(configuration)) |
             ui::onKey(ui::key(InputKey::F10), std::move(quit)) | ui::onKey(ui::key(InputKey::LEFT), std::move(left)) |
             ui::onKey(ui::key(InputKey::RIGHT), std::move(right));
    }

    auto meter = ui::scene(std::move(factory), options);

    meter.theme(example::THEME_PRESETS.at(theme_).make())
      .style(FOCUS_POLICY, {.name = "meter-focus"})
      .configure(std::move(configuration))
      .onKey(ui::key(InputKey::F10), std::move(quit))
      .onKey(ui::key(InputKey::LEFT), std::move(left))
      .onKey(ui::key(InputKey::RIGHT), std::move(right));

    return meter;
  }

  ManagedState<int> level_;   ///< Weak scene-owned scalar state; immutable snapshots are read during prepare.
  std::size_t theme_{};       ///< Theme preset index, committed only after successful replacement.
  ui::Ref<LevelMeter> meter_; ///< Typed observing reference, published only after the complete mount.
  ui::Scene scene_;           ///< Destroy callbacks and owned runtime before the members they borrow.
};
} // namespace meter_example
