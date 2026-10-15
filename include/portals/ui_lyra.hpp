// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_app.hpp>

#include <lyra/lyra.hpp>

#include <sstream>
#include <utility>

namespace portals::ui::lyra {

/** @brief Optional built-in launch actions; parsing itself remains Lyra's responsibility. */
struct CliOptions {
  bool automaticHelp{true};    ///< Add Lyra's -?, -h and --help; disable when supplying custom help.
  std::string version{};       ///< Nonempty text enables a short-circuiting --version option.
  const bool *helpRequested{}; ///< Optional caller-owned custom help flag; must outlive invocation.
};

/** @brief Adapt an owning copy of a native Lyra parser to the lazy launch boundary.
 * @param parser Native parser copied into the returned callable; bound references remain borrowed.
 * @param options Help/version policy copied into the callable, including the borrowed custom help flag.
 * @return Lazy parser consuming the launch argument snapshot, with owning output or typed diagnostics.
 *
 * Bound values, callbacks and helpRequested must outlive synchronous runApp. Parsing can partially
 * update bound values on failure, as documented by Lyra. User callback exceptions propagate to the
 * generic runApp exception boundary. No process arguments, terminal or output streams are acquired here.
 * Disable automaticHelp when the supplied parser already declares help. A nonempty version reserves
 * --version. Custom help callbacks must set helpRequested and use Lyra's short_circuit_all result.
 */
inline CommandLineParser cli(::lyra::cli parser, CliOptions options = {}) {
  return [parser = std::move(parser), options = std::move(options)](
           const app::Arguments &arguments) -> std::expected<LaunchAction, CommandLineError> {
    bool showHelp{};
    bool showVersion{};
    auto invocation = parser;

    if (options.automaticHelp) {
      invocation |= ::lyra::help(showHelp);
    }

    if (!options.version.empty()) {
      invocation |= ::lyra::opt([&showVersion](bool flag) {
        showVersion = flag;

        return ::lyra::parser_result::ok(::lyra::parser_result_type::short_circuit_all);
      })["--version"]("Display version information.");
    }

    const auto result = invocation.parse(::lyra::args(arguments.begin(), arguments.end()));

    if (!result) {
      return std::unexpected(CommandLineError{CommandLineErrorCode::INVALID_ARGUMENT, result.message()});
    }

    if (showHelp || (options.helpRequested && *options.helpRequested)) {
      std::ostringstream output;

      output << invocation;

      return LaunchAction{LaunchDisposition::FINISH, std::move(output).str()};
    }

    if (showVersion) {
      return LaunchAction{LaunchDisposition::FINISH, options.version + '\n'};
    }

    return LaunchAction{};
  };
}

} // namespace portals::ui::lyra
