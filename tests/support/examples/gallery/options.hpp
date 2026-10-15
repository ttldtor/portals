// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "../../../../examples/gallery/themes.hpp"
#include <algorithm>
#include <array>
#include <expected>
#include <ostream>
#include <portals/style.hpp>
#include <string>
#include <string_view>

namespace example {
/** @brief Parsed example settings; stage borrows argv for the main function's lifetime. */
struct FixtureOptions {
  std::size_t theme{};    ///< Index in THEME_PRESETS; Modern is the compatibility default.
  bool check{};           ///< Run a finite in-memory replay; never open a native window.
  bool ansi16{};          ///< Encode the output using the terminal's sixteen ANSI colors.
  bool help{};            ///< Print usage and exit before creating the runtime.
  std::string_view stage; ///< Optional gallery fixture stage, borrowed from argv.
};

/** @brief Parse theme/rendering switches and optional historical gallery fixture modes.
 * @param argc Number of argv entries including the executable.
 * @param argv Borrowed command-line arguments, retained only by the stage view.
 * @param galleryStages Accept the gallery's historical semantic fixture modes.
 * @return Valid options or an owning diagnostic for an unknown/missing option.
 */
inline std::expected<FixtureOptions, std::string> parseFixtureOptions(int argc, char **argv,
                                                                      bool galleryStages = false) {
  FixtureOptions result;

  for (int index = 1; index < argc; ++index) {
    const std::string_view option = argv[index];

    if (option == "--check") {
      result.check = true;
    } else if (option == "--ansi16") {
      result.ansi16 = true;
    } else if (option == "--help") {
      result.help = true;
    } else if (option == "--theme") {
      if (++index == argc) {
        return std::unexpected("--theme requires a name");
      }

      const auto found = std::ranges::find(THEME_PRESETS, std::string_view(argv[index]), &ThemePreset::name);

      if (found == THEME_PRESETS.end()) {
        return std::unexpected("Unknown theme: " + std::string(argv[index]));
      }

      result.theme = static_cast<std::size_t>(found - THEME_PRESETS.begin());
    } else if (galleryStages &&
               (option == "initial" || option == "keyboard" || option == "mouse" || option == "disabled")) {
      result.stage = option;
      result.check = true;
    } else {
      return std::unexpected("Unknown option: " + std::string(option));
    }
  }

  return result;
}

/** @brief Write shared example options, theme names and keyboard hints.
 * @param output Borrowed output stream; errors follow its configured stream policy.
 * @param program Executable name displayed in the usage line.
 * @return No value; stream failures follow its configured policy.
 */
inline void printSceneUsage(std::ostream &output, std::string_view program) {
  output << "Usage: " << program << " [--ansi16] [--theme NAME] [--help]\nThemes: ";

  for (std::size_t index = 0; index < THEME_PRESETS.size(); ++index) {
    output << (index ? ", " : "") << THEME_PRESETS[index].name;
  }

  output << "\nF2 cycles themes; F10 or Ctrl+C exits.\n";
}

} // namespace example
