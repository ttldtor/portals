// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "settings.hpp"
#include "themes.hpp"
#include <portals/ui.hpp>

namespace gallery {
/** @param themes Same-scene logical theme selection; historical superseded IDs do not identify a live theme.
 * @return Checked F2 preview and commit, preserving the complete managed error.
 */
inline std::expected<void, portals::ManagedError> cycleTheme(portals::ui::ThemeSelectionRef themes) {
  using namespace portals;
  const auto observed = themes.snapshot();

  if (!observed) {
    return std::unexpected(observed.error());
  }

  const auto &snapshot = **observed;
  const auto &current =
    snapshot.status == ui::ThemeSelectionStatus::ACTIVE ? snapshot.previewedId : snapshot.lastCommittedId;
  const bool currentIsMeaningful = snapshot.status != ui::ThemeSelectionStatus::SUPERSEDED;
  const auto next =
    currentIsMeaningful && current ? (example::themeIndex(*current) + 1) % example::THEME_PRESETS.size() : 0;

  return themes.preview(example::THEME_PRESETS[next].name).and_then([themes] {
    return themes.commit();
  });
}

/** @param options Borrowed settings, read after command-line parsing.
 * @param themes Scene-owned catalog/model observation shared with the selector.
 * @return Lazy initial commit and F2 cycling, preserving complete managed failures.
 */
inline auto onTheme(portals::ui::OptionsRef<example::Options> options, portals::ui::ThemeSelectionRef themes) {
  using namespace portals;

  return ui::configure([options, themes](ManagedScene &owner) -> std::expected<void, ManagedError> {
    auto initialized = themes.preview(options.get().theme).and_then([themes] {
      return themes.commit();
    });

    if (!initialized) {
      return initialized;
    }

    return owner.onKey(ui::press("F2"), [themes] {
      return cycleTheme(themes);
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

  ui::ThemeSelectionRef themes;
  ui::ThemeSelectorRef selector;

  // clang-format off
  auto body = ui::vbox(
    ui::label("Portals gallery | F2 theme | F10 quit"),
    ui::label("Name (Enter commits; Tab changes focus):"),
    ui::lineEdit("Portals", {.options = {.editMode = EditMode::EXPLICIT}}),
    ui::checkbox("Alerts"),
    ui::button("Run (no model change)", [run = std::move(run)] {
      if (run) {
        run();
      }
    }),
    ui::button("Quit", [] { ui::quit(); }),
    ui::button("Themes", [selector] { return selector.open(); }));

  return ui::scene(ui::overlayHost(std::move(body), ui::themeSelector(selector, themes)), policies)
    | ui::applicationOptions([options, result = policies.application]() mutable {
           result.writer.colorMode = options.get().ansi16 ? ColorMode::ANSI16 : ColorMode::TRUE_COLOR;

           return result;
         })
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | example::themeCatalog(themes)
    | onTheme(options, themes);
  // clang-format on
}
} // namespace gallery
