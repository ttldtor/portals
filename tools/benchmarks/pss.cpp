// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/style.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {
using namespace portals;
using Clock = std::chrono::steady_clock;
using ParseResult = std::expected<StyleSheet, StyleError>;
constexpr std::size_t DEFAULT_SAMPLES = 100;
constexpr std::size_t DEFAULT_WARMUP = 10;
constexpr std::size_t DEFAULT_BATCH = 16;
constexpr std::size_t MAX_SAMPLES = 10000;
constexpr std::size_t MAX_WARMUP = 1000;
constexpr std::size_t MAX_BATCH = 1024;
constexpr std::size_t MEDIUM_RULES = 64;
constexpr std::string_view RULE =
  "Button.primary#submit { foreground: #123456; background: #abcdef; bold: true; italic: false; "
  "underline: true; min-width: 12; }\n";

/** @brief Explicit repeat counts and report destination for one process run. */
struct Options {
  std::size_t samples{DEFAULT_SAMPLES}; ///< Timed batches per source case.
  std::size_t warmup{DEFAULT_WARMUP};   ///< Untimed batches per source case.
  std::size_t batch{DEFAULT_BATCH};     ///< Individually timed calls averaged in each sample.
  bool checkOnly{};                     ///< Validate all cases without collecting timing.
  bool reverse{};                       ///< Reverse source order for alternating process runs.
  std::string output;                   ///< Optional JSON file, otherwise standard output.
};

/** @brief Generated source and independently constructed semantic expectation. */
struct Case {
  std::string name;                ///< Stable report identifier.
  std::string source;              ///< Owning input generated before timing.
  StyleSheet expected;             ///< Complete successful result, checked outside timing.
  std::optional<StyleError> error; ///< Exact error category, position and static message.
  std::vector<double> samples;     ///< Nanoseconds per parse for each timed batch.
};

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

std::size_t number(std::string_view text, std::size_t maximum) {
  std::size_t value{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);

  check(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() && value <= maximum,
        "invalid numeric option");

  return value;
}

Options parseOptions(int argc, char **argv) {
  Options options;

  for (int index = 1; index < argc; ++index) {
    const std::string_view key(argv[index]);

    if (key == "--check") {
      options.checkOnly = true;

      continue;
    }

    if (key == "--reverse") {
      options.reverse = true;

      continue;
    }

    check(index + 1 < argc, "missing option value");

    const std::string_view value(argv[++index]);

    if (key == "--samples") {
      options.samples = number(value, MAX_SAMPLES);
      check(options.samples > 0, "samples must be positive");
    } else if (key == "--warmup") {
      options.warmup = number(value, MAX_WARMUP);
    } else if (key == "--batch") {
      options.batch = number(value, MAX_BATCH);
      check(options.batch > 0, "batch must be positive");
    } else if (key == "--output") {
      options.output = value;
    } else {
      throw std::runtime_error("unknown option");
    }
  }

  return options;
}

StyleRule expectedRule() {
  StyleRule rule;

  rule.selector.type = "Button";
  rule.selector.classes = {"primary"};
  rule.selector.id = "submit";
  rule.declarations.foreground = RgbColor{0x12, 0x34, 0x56};
  rule.declarations.background = RgbColor{0xab, 0xcd, 0xef};
  rule.declarations.bold = true;
  rule.declarations.italic = false;
  rule.declarations.underline = true;
  rule.declarations.minWidth = 12;

  return rule;
}

Case validCase(std::string name, std::size_t rules) {
  Case result;

  result.name = std::move(name);
  result.source.reserve(RULE.size() * rules);
  result.expected.rules.reserve(rules);

  for (std::size_t index = 0; index < rules; ++index) {
    result.source += RULE;
    result.expected.rules.push_back(expectedRule());
  }

  return result;
}

std::vector<Case> makeCases() {
  std::vector<Case> cases;

  cases.push_back(validCase("empty", 0));
  cases.push_back(validCase("small", 1));
  cases.push_back(validCase("rules_64", MEDIUM_RULES));
  cases.push_back(validCase("rules_1024", MAX_STYLE_RULES));

  auto duplicate = validCase("duplicate_declaration", 1);

  duplicate.source = "Button.primary#submit { foreground: #123456; background: #abcdef; bold: false; bold: true; "
                     "italic: false; underline: true; min-width: 12; }\n";
  cases.push_back(std::move(duplicate));

  Case early;

  early.name = "invalid_early";
  early.source = "@";
  early.error = StyleError{TreeError::INVALID_ARGUMENT, 1, 1, "Expected selector"};
  cases.push_back(std::move(early));

  auto late = validCase("invalid_late", MAX_STYLE_RULES - 1);

  late.source += "@";
  late.error = StyleError{TreeError::INVALID_ARGUMENT, MAX_STYLE_RULES, 1, "Expected selector"};
  cases.push_back(std::move(late));

  return cases;
}

std::uint64_t validate(const Case &test, const ParseResult &result) {
  if (test.error) {
    check(!result.has_value(), "invalid source unexpectedly succeeded");

    const auto &actual = result.error();
    const auto &expected = *test.error;

    check(actual.cause == expected.cause && actual.line == expected.line && actual.column == expected.column &&
            actual.message == expected.message,
          "error category, position or message changed");

    return actual.line + actual.column + actual.message.size();
  }

  check(result.has_value(), "valid source failed");
  check(*result == test.expected, "owning rules, selectors or declarations changed");

  return result->rules.size() + 1;
}

double percentile(std::vector<double> values, double fraction) {
  std::ranges::sort(values);

  const auto rank = static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(values.size())));

  return values[rank - 1];
}

void writeReport(std::ostream &output, const Options &options, const std::vector<Case> &cases, std::uint64_t checksum) {
  output << std::format(
    "{{\"schema\":\"portals.pss.v1\",\"api\":\"parsePss\",\"unit\":\"ns_per_parse\","
    "\"timing\":\"parse_return_with_clock_overhead\",\"successful_result_destruction\":\"excluded\","
    "\"partial_error_cleanup\":\"included\",\"memory\":\"NOT_MEASURED\","
    "\"samples\":{},\"warmup\":{},\"batch\":{},\"reverse\":{},\"checksum\":{},\"cases\":[",
    options.samples, options.warmup, options.batch, options.reverse, checksum);

  for (std::size_t index = 0; index < cases.size(); ++index) {
    const auto &test = cases[index];

    if (index != 0) {
      output << ',';
    }

    output << std::format("{{\"case\":\"{}\",\"source_bytes\":{},\"valid\":{},\"expected_rules\":{},"
                          "\"p50\":{},\"p95\":{},\"raw\":[",
                          test.name, test.source.size(), !test.error.has_value(),
                          test.error ? 0 : test.expected.rules.size(), percentile(test.samples, 0.50),
                          percentile(test.samples, 0.95));

    for (std::size_t sample = 0; sample < test.samples.size(); ++sample) {
      if (sample != 0) {
        output << ',';
      }

      output << std::format("{}", test.samples[sample]);
    }

    output << "]}";
  }

  output << "]}\n";
  check(output.good(), "report write failed");
}
} // namespace

/** @brief Validate PSS semantics or measure runtime parsing of pre-generated sources.
 * @param argc Number of command-line arguments.
 * @param argv Options --check, --reverse, --samples, --warmup, --batch and --output.
 * @return Zero on complete semantic validation/reporting, one on failure.
 */
int main(int argc, char **argv) {
  try {
    const auto options = parseOptions(argc, argv);
    auto cases = makeCases();
    std::uint64_t checksum{};

    if (options.reverse) {
      std::ranges::reverse(cases);
    }

    for (const auto &test : cases) {
      checksum += validate(test, parsePss(test.source));
    }

    if (options.checkOnly) {
      std::cout << std::format("Checked {} PSS cases; checksum={}\n", cases.size(), checksum);

      return 0;
    }

    for (auto &test : cases) {
      test.samples.reserve(options.samples);

      for (std::size_t sample = 0; sample < options.warmup + options.samples; ++sample) {
        double elapsed{};

        for (std::size_t call = 0; call < options.batch; ++call) {
          const auto begin = Clock::now();
          const auto result = parsePss(test.source);
          const auto end = Clock::now();

          elapsed += std::chrono::duration<double, std::nano>(end - begin).count();
          checksum += validate(test, result);
          // The owning return is initialized directly; validation and destruction follow the timer.
        }

        if (sample >= options.warmup) {
          test.samples.push_back(elapsed / static_cast<double>(options.batch));
        }
      }
    }

    if (options.output.empty()) {
      writeReport(std::cout, options, cases, checksum);
    } else {
      std::ofstream output(options.output);

      check(output.is_open(), "cannot open report");
      writeReport(output, options, cases, checksum);
    }

    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
  }
}
