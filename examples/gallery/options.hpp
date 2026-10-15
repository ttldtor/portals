// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "settings.hpp"
#include "themes.hpp"
#include <algorithm>
#include <portals/ui_lyra.hpp>
#include <string>

namespace example {

/** @param options Caller-owned settings populated by Lyra before UI creation.
 * @return Owning parser with bindings to options; help is supplied by the runner adapter.
 */
inline auto commandLine(Options &options) {
  return lyra::cli()
    | lyra::opt(options.theme, "name")["-t"]["--theme"]("Initial theme").choices([](const std::string &name) {
        return std::ranges::any_of(THEME_PRESETS, [&name](const auto &preset) {
          return preset.name == name;
        });
      })
    | lyra::opt(options.ansi16)["--ansi16"]("Use sixteen ANSI colors");
}
} // namespace example
