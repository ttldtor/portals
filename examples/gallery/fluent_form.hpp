// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "settings.hpp"
#include "themes.hpp"
#include <portals/ui.hpp>

namespace gallery {
/** @param themes Same-scene selector model; historical superseded IDs do not identify a live theme.
 * @return Checked F2 preview and commit, preserving the complete managed error.
 */
inline std::expected<void, portals::ManagedError> cycleTheme(portals::ui::Ref<portals::ThemeSelector> themes) {
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

/** @param options Caller-owned settings, read after successful command-line parsing.
 * @param run Optional application action; by default Run leaves field models unchanged.
 * @param policies Existing scene policies for embedding and custom clocks.
 * @return Lazy owning scene with internal editor and checkbox state.
 */
inline portals::ui::Scene form(portals::ui::OptionsRef<example::Options> options, std::function<void()> run = {},
                               portals::ui::SceneOptions policies = {}) {
  using namespace portals;

  ui::Ref<ThemeSelector> selector;

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

  return ui::scene(policies, std::move(body),
                   ui::themeSelector(selector, example::themeCatalog(options.get().theme)))
    | ui::applicationOptions([options, result = policies.application]() mutable {
           result.writer.colorMode = options.get().ansi16 ? ColorMode::ANSI16 : ColorMode::TRUE_COLOR;

           return result;
         })
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::onKey("F2", [selector] { return cycleTheme(selector); });
  // clang-format on
}
} // namespace gallery
