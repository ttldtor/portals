// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "../gallery/settings.hpp"
#include "meter.hpp"
#include <portals/ui_style.hpp>

namespace meter_example {
/** @param options Caller-owned settings, read after successful command-line parsing.
 * @param policies Existing scene policies for embedding and custom clocks.
 * @return Lazy owning meter; callbacks share a weak scene-state handle, never a widget pointer.
 */
inline ui::Scene meter(ui::OptionsRef<example::Options> options, ui::SceneOptions policies = {}) {
  auto level = std::make_shared<ManagedState<int>>();
  auto content = [options, level](ManagedScene &owner) -> std::expected<ui::Description<LevelMeter>, ManagedError> {
    auto state = owner.state(INITIAL_LEVEL, Invalidation::PAINT);

    if (!state) {
      return std::unexpected(state.error());
    }

    *level = std::move(*state);
    const auto theme = example::themeIndex(options.get().theme);
    auto styled = owner.theme(example::THEME_PRESETS.at(theme).make());

    if (!styled) {
      return std::unexpected(styled.error());
    }

    // Install the typed theme fallback before composing the pipe shortcut registry.
    auto input =
      owner.onInput([&owner, theme = theme](const InputEvent &event) mutable -> std::expected<bool, ManagedError> {
        if (!keyPressed(event, InputKey::F2, {})) {
          return false;
        }

        const auto next = (theme + 1) % example::THEME_PRESETS.size();

        return owner.theme(example::THEME_PRESETS[next].make()).transform([&theme, next] {
          theme = next;

          return true;
        });
      });

    if (!input) {
      return std::unexpected(input.error());
    }

    return ui::custom<LevelMeter>().style({.type = "Meter"}).onPrepare([level](LevelMeter &widget, Node<Widget>) {
      // State changes invalidate the root; prepare only consumes its latest snapshot.
      return level->snapshot().and_then([&widget](const auto &snapshot) {
        return widget.prepare(snapshot->value);
      });
    });
  };
  auto adjust = [level](const KeyEvent &event) {
    const auto step = event.key == InputKey::RIGHT ? LEVEL_STEP : -LEVEL_STEP;

    return level->snapshot().and_then([level, step](const auto &snapshot) {
      return level->set(std::clamp(snapshot->value + step, MIN_LEVEL, MAX_LEVEL));
    });
  };

  return ui::scene(std::move(content), policies)
    | ui::applicationOptions([options, result = policies.application]() mutable {
           result.writer.colorMode = options.get().ansi16 ? ColorMode::ANSI16 : ColorMode::TRUE_COLOR;

           return result;
         })
    | ui::style("Meter:focused { bold: true; }")
    | ui::onKey(ui::pressRepeat({key::LEFT, key::RIGHT}), std::move(adjust))
    | ui::onKey("F10 | Ctrl+C", ui::quit);
}
} // namespace meter_example
