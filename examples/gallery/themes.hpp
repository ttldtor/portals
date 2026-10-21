// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <algorithm>
#include <array>
#include <portals/style.hpp>
#include <portals/ui.hpp>
#include <stdexcept>
#include <string_view>

namespace example {
/** @brief Named stylesheet factory shared by example CLI and theme cycling. */
struct ThemePreset {
  std::string_view name;         ///< Command-line spelling, with static lifetime.
  portals::StyleSheet (*make)(); ///< Creates a fresh owning stylesheet.
};

/** @brief Static preset order shared by CLI selection and F2 theme cycling. */
inline constexpr std::array THEME_PRESETS{ThemePreset{"modern", portals::modernTheme},
                                          ThemePreset{"turbo", portals::turboTheme},
                                          ThemePreset{"nord", portals::nordTheme},
                                          ThemePreset{"gruvbox", portals::gruvboxTheme},
                                          ThemePreset{"tokyo-night", portals::tokyoNightTheme},
                                          ThemePreset{"catppuccin-latte", portals::catppuccinLatteTheme}};

/** @param name Exact preset name; CLI validates choices before scene creation.
 * @return Stable preset index, or out_of_range for a programmatically invalid option.
 */
inline std::size_t themeIndex(std::string_view name) {
  const auto preset = std::ranges::find(THEME_PRESETS, name, &ThemePreset::name);

  if (preset == THEME_PRESETS.end()) {
    throw std::out_of_range("Unknown theme");
  }

  return static_cast<std::size_t>(preset - THEME_PRESETS.begin());
}

/** @param initialId Initial committed preset, validated before selector publication.
 * @return Owning selector catalog using the same names, factories and order as the CLI presets.
 */
inline portals::ui::ThemeSelectorCatalog themeCatalog(std::string_view initialId) {
  portals::ui::ThemeSelectorCatalog catalog;
  catalog.initialId = initialId;

  for (const auto &preset : THEME_PRESETS) {
    catalog.choices.push_back({preset.name, preset.name, preset.make});
  }

  return catalog;
}
} // namespace example
