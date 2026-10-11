// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "compactness.hpp"

#include <array>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace portals::compactness {
namespace {

struct Measurement {
  std::string_view name;
  std::uint64_t Metrics::*member;
};

constexpr std::array MEASUREMENTS = {Measurement{"files", &Metrics::files},
                                     Measurement{"source_bytes", &Metrics::sourceBytes},
                                     Measurement{"lexical_tokens_approx", &Metrics::lexicalTokensApprox},
                                     Measurement{"code_lines", &Metrics::codeLines},
                                     Measurement{"preprocessor_directives", &Metrics::preprocessorDirectives},
                                     Measurement{"preprocessor_tokens", &Metrics::preprocessorTokens},
                                     Measurement{"comment_bytes", &Metrics::commentBytes},
                                     Measurement{"string_literal_bytes", &Metrics::stringLiteralBytes},
                                     Measurement{"max_line_bytes", &Metrics::maxLineBytes},
                                     Measurement{"max_brace_nesting", &Metrics::maxBraceNesting}};

std::string pathUtf8(const std::filesystem::path &path) {
  const auto bytes = path.generic_u8string();

  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

void writeJsonString(std::ostream &output, std::string_view text) {
  constexpr std::string_view HEX_DIGITS = "0123456789abcdef";

  output << '"';

  for (const unsigned char byte : text) {
    if (byte == '"' || byte == '\\') {
      output << '\\' << static_cast<char>(byte);
    } else if (byte < 0x20) {
      output << "\\u00" << HEX_DIGITS[byte >> 4U] << HEX_DIGITS[byte & 0xFU];
    } else {
      output << static_cast<char>(byte);
    }
  }

  output << '"';
}

void writePaths(std::ostream &output, const Footprint &footprint) {
  output << '[';

  for (std::size_t index = 0; index < footprint.paths.size(); ++index) {
    if (index != 0) {
      output << ", ";
    }

    writeJsonString(output, pathUtf8(footprint.paths[index]));
  }

  output << ']';
}

void writeDelta(std::ostream &output, std::uint64_t before, std::uint64_t after) {
  if (after < before) {
    output << '-' << before - after;
  } else {
    output << after - before;
  }
}

void writePercent(std::ostream &output, std::uint64_t before, std::uint64_t after, std::string_view absent) {
  if (before == 0) {
    output << absent;

    return;
  }

  const auto delta =
    after < before ? -static_cast<long double>(before - after) : static_cast<long double>(after - before);

  output << std::fixed << std::setprecision(6) << delta * 100.0L / static_cast<long double>(before);
}

std::string renderJson(const Footprint &before, const Footprint &after) {
  std::ostringstream output;

  output.imbue(std::locale::classic());
  output << "{\n  \"schema_version\": 1,\n  \"analyzer\": \"portals-compactness-1\",\n"
            "  \"scope\": \"explicit_full_files\",\n  \"ui_slice\": \"NOT_MEASURED\",\n"
            "  \"equivalence\": \"NOT_CHECKED\",\n  \"before_files\": ";
  writePaths(output, before);
  output << ",\n  \"after_files\": ";
  writePaths(output, after);
  output << ",\n  \"metrics\": {\n";

  for (std::size_t index = 0; index < MEASUREMENTS.size(); ++index) {
    const auto measurement = MEASUREMENTS[index];
    const auto baseline = before.metrics.*measurement.member;
    const auto candidate = after.metrics.*measurement.member;

    output << "    ";
    writeJsonString(output, measurement.name);
    output << ": {\"before\": " << baseline << ", \"after\": " << candidate << ", \"delta\": ";
    writeDelta(output, baseline, candidate);
    output << ", \"percent\": ";
    writePercent(output, baseline, candidate, "null");
    output << '}' << (index + 1 == MEASUREMENTS.size() ? "\n" : ",\n");
  }

  output << "  }\n}\n";

  return output.str();
}

std::string renderMarkdown(const Footprint &before, const Footprint &after) {
  std::ostringstream output;

  output.imbue(std::locale::classic());
  output << "# Explicit full-file compactness comparison\n\n"
            "UI slice: NOT_MEASURED. Behavioral equivalence: NOT_CHECKED.\n\n"
            "Counts describe source spelling; they do not establish readability or correctness.\n\n"
            "| Metric | Before | After | Delta | Percent |\n"
            "| --- | ---: | ---: | ---: | ---: |\n";

  for (const auto measurement : MEASUREMENTS) {
    const auto baseline = before.metrics.*measurement.member;
    const auto candidate = after.metrics.*measurement.member;

    output << "| " << measurement.name << " | " << baseline << " | " << candidate << " | ";
    writeDelta(output, baseline, candidate);
    output << " | ";
    writePercent(output, baseline, candidate, "N/A");

    if (baseline != 0) {
      output << '%';
    }

    output << " |\n";
  }

  // JSON-escaped paths in an indented code block cannot alter Markdown structure.
  output << "\nBefore files (canonical, deduplicated):\n\n    ";
  writePaths(output, before);
  output << "\n\nAfter files (canonical, deduplicated):\n\n    ";
  writePaths(output, after);
  output << '\n';

  return output.str();
}

} // namespace

Options parseArguments(std::span<const std::string_view> arguments) {
  Options options;
  bool formatSeen = false;

  if (arguments.size() == 1 && arguments.front() == "--help") {
    options.help = true;

    return options;
  }

  for (std::size_t index = 0; index < arguments.size(); ++index) {
    const auto option = arguments[index];

    if (option != "--before" && option != "--after" && option != "--format") {
      throw std::runtime_error("unknown option: " + std::string(option));
    }

    if (++index == arguments.size() || arguments[index].empty() || arguments[index].starts_with("--")) {
      throw std::runtime_error("missing value for " + std::string(option));
    }

    const auto value = arguments[index];

    if (option == "--format") {
      if (formatSeen || (value != "json" && value != "markdown")) {
        throw std::runtime_error("--format must occur once with json or markdown");
      }

      formatSeen = true;
      options.format = value;
    } else {
      auto &paths = option == "--before" ? options.before : options.after;

      paths.emplace_back(std::u8string(value.begin(), value.end()));
    }
  }

  if (options.before.empty() || options.after.empty()) {
    throw std::runtime_error("both --before FILE and --after FILE are required; use --help for usage");
  }

  return options;
}

std::string renderReport(const Footprint &before, const Footprint &after, std::string_view format) {
  if (format == "json") {
    return renderJson(before, after);
  }

  if (format == "markdown") {
    return renderMarkdown(before, after);
  }

  throw std::runtime_error("unsupported report format");
}

std::string_view helpText() {
  return "Usage: portals_compactness --before FILE [--before FILE ...]\n"
         "                           --after FILE [--after FILE ...] [--format json|markdown]\n"
         "       portals_compactness --help\n\n"
         "Supply complete client files, including every local/shared helper. No includes are followed.\n"
         "Canonical duplicate paths count once on each side; JSON is the default format.\n"
         "lexical_tokens_approx counts a documented UTF-8/ASCII C++ lexical subset, not an AST.\n"
         "String/comment lengths measure source spelling bytes, not decoded runtime strings.\n"
         "Brace depth is lexical, excludes directives, and is not cognitive complexity.\n"
         "UI slices: NOT_MEASURED. Behavioral equivalence: NOT_CHECKED. No overall score.\n"
         "Zero baseline percent is null (JSON) or N/A (Markdown). Errors return exit code 1.\n"
         "Unicode/UCN identifiers and the <:: corner case are unsupported and rejected.\n"
         "Details and build instructions: docs/compactness.md.\n";
}

} // namespace portals::compactness
