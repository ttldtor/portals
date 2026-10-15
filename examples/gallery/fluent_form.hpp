// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "settings.hpp"
#include "themes.hpp"
#include <portals/ui_style.hpp>

namespace gallery {
/** @param options Borrowed settings, read after command-line parsing.
 * @return Lazy theme stage; theme failures leave the current index unchanged.
 */
inline auto onTheme(portals::ui::OptionsRef<example::Options> options) {
  using namespace portals;

  return ui::configure([options](ManagedScene &owner) {
    const auto theme = example::themeIndex(options.get().theme);

    return owner.theme(example::THEME_PRESETS[theme].make()).and_then([&owner, theme] {
      return owner.onKey(KeyEvent{.key = InputKey::F2}, [&owner, theme = theme]() mutable {
        const auto next = (theme + 1) % example::THEME_PRESETS.size();

        return owner.theme(example::THEME_PRESETS[next].make())
          .transform_error([&owner](const ManagedError &error) {
            static_cast<void>(owner.reportError(error));
            const auto *cause = std::get_if<TreeError>(&error.cause);

            return cause ? *cause : TreeError::CALLBACK_FAILED;
          })
          .transform([&theme, next] {
            theme = next;
          });
      });
    });
  });
}

/** @param options Caller-owned settings, read after successful command-line parsing.
 * @param run Optional application action; by default Run leaves field models unchanged.
 * @param policies Existing scene policies for embedding and custom clocks.
 * @return Lazy owning scene with internal editor and checkbox state.
 */
inline portals::ui::Scene form(portals::ui::OptionsRef<example::Options> options, std::function<void()> run = {},
                               portals::ui::SceneOptions policies = {}) {
  using namespace portals;

  return ui::scene(ui::vbox(ui::label("Portals gallery | F2 theme | F10 quit"),
                            ui::label("Name (Enter commits; Tab changes focus):"),
                            ui::lineEdit("Portals", {.options = {.editMode = EditMode::EXPLICIT}}),
                            ui::checkbox("Alerts"),
                            ui::button("Run (no model change)",
                                       [run = std::move(run)] {
                                         if (run) {
                                           run();
                                         }
                                       }),
                            ui::button("Quit",
                                       [] {
                                         ui::quit();
                                       })),
                   policies)
    | ui::applicationOptions([options, result = policies.application]() mutable {
           result.writer.colorMode = options.get().ansi16 ? ColorMode::ANSI16 : ColorMode::TRUE_COLOR;

           return result;
         })
    | ui::style("Button:focused { bold: true; }")
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | onTheme(options);
}
} // namespace gallery
