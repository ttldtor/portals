// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace portals::compactness {

/** Lexical measurements of explicitly supplied complete source files. */
struct Metrics {
  std::uint64_t files{};                  ///< Number of canonical, distinct input paths.
  std::uint64_t sourceBytes{};            ///< Original file bytes, including BOM and newlines.
  std::uint64_t lexicalTokensApprox{};    ///< Approximate preprocessing tokens, including directives.
  std::uint64_t codeLines{};              ///< Physical lines intersecting token spelling.
  std::uint64_t preprocessorDirectives{}; ///< Directives introduced at logical line start.
  std::uint64_t preprocessorTokens{};     ///< Tokens belonging to directives, including the introducer.
  std::uint64_t commentBytes{};           ///< Normalized source spelling bytes occupied by comments.
  std::uint64_t stringLiteralBytes{};     ///< Normalized string spelling bytes including prefix and suffix.
  std::uint64_t maxLineBytes{};           ///< Maximum physical line length in UTF-8 bytes, excluding BOM/newline.
  std::uint64_t maxBraceNesting{};        ///< Maximum lexical brace depth outside directives, per file.
};

/** A full-file footprint; include local/shared helpers explicitly. */
struct Footprint {
  std::vector<std::filesystem::path> paths; ///< Sorted, canonical input paths.
  Metrics metrics;                          ///< Sums of counts and maxima of maximum measurements.
};

/** Supported CLI options; paths are interpreted relative to the working directory. */
struct Options {
  std::vector<std::filesystem::path> before; ///< Explicit baseline files.
  std::vector<std::filesystem::path> after;  ///< Explicit candidate files.
  std::string format{"json"};                ///< Either json or markdown.
  bool help{};                               ///< Print usage without measurement.
};

/** Measure a supported UTF-8 C++ lexical subset; throws runtime_error on unsupported/malformed input.
 * @param source Complete file bytes, optionally with a leading UTF-8 BOM.
 * @return Measurements for one file; this does not validate C++ syntax or semantic equivalence.
 */
Metrics measureSource(std::string_view source);

/** Read and measure canonical distinct files; throws on any path, read, or lexical error.
 * @param paths Explicit complete footprint, including helpers; duplicates count once.
 * @return Sorted canonical paths and aggregate measurements.
 */
Footprint measureFiles(std::span<const std::filesystem::path> paths);

/** Parse CLI arguments; throws runtime_error on invalid options or missing sides.
 * @param arguments Arguments excluding the executable name.
 * @return Validated options; help must be the sole argument.
 */
Options parseArguments(std::span<const std::string_view> arguments);

/** Render a deterministic comparison; zero-baseline percentages are null in JSON.
 * @param before Complete baseline footprint.
 * @param after Complete candidate footprint.
 * @param format Either json or markdown; unsupported formats throw runtime_error.
 * @return Serialized report, including explicit paths and unmeasured/unverified scopes.
 */
std::string renderReport(const Footprint &before, const Footprint &after, std::string_view format);

/** Return the standalone CLI usage and measurement limitations.
 * @return Help text suitable for standard output.
 */
std::string_view helpText();

} // namespace portals::compactness
