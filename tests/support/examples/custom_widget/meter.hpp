// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "../../../../examples/custom_widget/meter.hpp"
#include "../gallery/options.hpp"

namespace meter_example {
class MeterScene {
public:
  /** @param app UI-affine application that must outlive this scene.
   * @param theme Initial index into example::THEME_PRESETS; invalid indices throw.
   */
  explicit MeterScene(Application &app, std::size_t theme = 0)
      : app_(app), theme_(theme), scene_(take(ManagedScene::create(app))) {
    level_ = take(scene_->state(INITIAL_LEVEL));

    meter_ =
      take(ui::mount(*scene_, ui::custom<LevelMeter>()
                                .style({.type = "Meter"})
                                .onPrepare([&](LevelMeter &meter, Node<Widget>) -> std::expected<void, TreeError> {
                                  return level_.snapshot()
                                    .and_then([&](const auto &snapshot) {
                                      return meter.prepare(snapshot->value);
                                    })
                                    .and_then([&](bool changed) -> std::expected<void, TreeError> {
                                      return changed ? app_.tree().invalidate(meter_, Invalidation::PAINT)
                                                     : std::expected<void, TreeError>{};
                                    });
                                })));

    take(scene_->theme(example::THEME_PRESETS.at(theme_).make()));
    take(scene_->onInput([&](const InputEvent &event) -> std::expected<bool, ManagedError> {
      if (example::quitKey(event)) {
        app_.requestStop();

        return true;
      }

      if (keyPressed(event, InputKey::F2)) {
        const auto next = (theme_ + 1) % example::THEME_PRESETS.size();
        auto changed = scene_->theme(example::THEME_PRESETS[next].make());

        if (!changed) {
          return std::unexpected(changed.error());
        }

        theme_ = next;

        return true;
      }

      if (!keyPressed(event, {InputKey::LEFT, InputKey::RIGHT})) {
        return false;
      }

      const auto step = keyPressed(event, InputKey::RIGHT) ? LEVEL_STEP : -LEVEL_STEP;
      return level_.snapshot()
        .and_then([&](const auto &current) {
          return level_.set(std::clamp(current->value + step, MIN_LEVEL, MAX_LEVEL));
        })
        .transform([](const auto &) {
          return true;
        })
        .transform_error([&](TreeError cause) {
          return ManagedError{
            .phase = ManagedPhase::INPUT, .node = meter_.id(), .cause = cause, .application = {}, .cleanup = {}};
        });
    }));
  }

  /** @return Runner statistics or the typed managed error, after native terminal cleanup. */
  std::expected<ApplicationStats, ManagedError> run() {
    return scene_->run();
  }

  /** @param terminal Borrowed terminal opened and closed by the managed runner.
   * @return Runner statistics or the typed managed error, including cleanup failures.
   */
  std::expected<ApplicationStats, ManagedError> run(ApplicationTerminal &terminal) {
    return scene_->run(terminal);
  }

private:
  Application &app_;
  ManagedState<int> level_;
  std::size_t theme_{};
  Node<LevelMeter> meter_;
  // Destroy captured adapters before their state handle and application reference.
  std::unique_ptr<ManagedScene> scene_;
};
} // namespace meter_example
