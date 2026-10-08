// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "common.hpp"
#include "options.hpp"
#include <portals/ui.hpp>

namespace gallery {
// Application outlives the form; the scene owns its nodes, state and callbacks.
class Form {
public:
  explicit Form(portals::Application &app, std::size_t theme = 0)
      : app_(app), theme_(theme), scene_(example::take(portals::ManagedScene::create(app))) {
    using namespace portals;
    using example::take;

    const auto name = take(scene_->state<std::string>("Portals"));
    const auto alerts = take(scene_->state<bool>(false));
    ui::NodeRef<LineEdit> editor;

    take(ui::mount(*scene_, ui::column(ui::label("Portals gallery | F2 theme | F10 quit"),
                                       ui::label("Name (Enter commits; Tab changes focus):"),
                                       ui::lineEdit(name).reference(editor), ui::checkbox("Alerts", alerts),
                                       ui::button("Run (no model change)",
                                                  [&]() -> std::expected<void, TreeError> {
                                                    ++calls;

                                                    return {};
                                                  }),
                                       ui::button("Quit", [&]() -> std::expected<void, TreeError> {
                                         app_.requestStop();

                                         return {};
                                       }))));

    take(app.router().setFocus(editor.node()));
    take(scene_->theme(example::THEME_PRESETS.at(theme_).make()));
    take(scene_->onInput([&](const InputEvent &event) -> std::expected<bool, ManagedError> {
      if (example::quitKey(event)) {
        app_.requestStop();

        return true;
      }

      const auto *key = std::get_if<KeyEvent>(&event);

      if (key && key->action != KeyAction::RELEASE && key->key == InputKey::F2) {
        const auto next = (theme_ + 1) % example::THEME_PRESETS.size();
        auto changed = scene_->theme(example::THEME_PRESETS[next].make());

        if (!changed) {
          return std::unexpected(changed.error());
        }

        theme_ = next;

        return true;
      }

      return false;
    }));
  }

  std::expected<portals::ApplicationStats, portals::ManagedError> run() {
    return scene_->run();
  }

  std::expected<portals::ApplicationStats, portals::ManagedError> run(portals::ApplicationTerminal &terminal) {
    return scene_->run(terminal);
  }

  int calls{}; ///< Observable business action count; Run deliberately changes no model.

private:
  portals::Application &app_;
  std::size_t theme_{};
  std::unique_ptr<portals::ManagedScene> scene_;
};
} // namespace gallery
