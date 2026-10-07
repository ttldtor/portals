// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "native.hpp"

#include <charconv>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

namespace {
using namespace std::chrono_literals;
constexpr unsigned default_timeout_ms = 250;
constexpr unsigned maximum_timeout_ms = 10000;
constexpr unsigned default_byte_limit = 4096;
constexpr unsigned maximum_byte_limit = 65536;
constexpr std::size_t environment_limit = 1024;

struct Options {
  bool query = true;
  unsigned timeout_ms = default_timeout_ms;
  unsigned byte_limit = default_byte_limit;
  std::string width_policy = "ambiguous-narrow";
};

void field(std::string_view name, std::string_view value) {
  std::cout << name << '=' << portals::probe::escape_bytes(value) << '\n';
}

bool number(std::string_view argument, unsigned maximum, unsigned &value) {
  unsigned parsed = 0;
  const auto result = std::from_chars(argument.data(), argument.data() + argument.size(), parsed);

  if (argument.empty() || result.ec != std::errc{} || result.ptr != argument.data() + argument.size() || parsed == 0 ||
      parsed > maximum) {
    return false;
  }

  value = parsed;

  return true;
}

void help() {
  std::cout << "Usage: portals_terminal_probe [options]\n"
               "  --help                    Show this help\n"
               "  --no-query, --headless     Inspect only; never change modes or read input\n"
               "  --timeout-ms N            Total query window (1..10000, default 250)\n"
               "  --max-bytes N             Captured input limit (1..65536, default 4096)\n"
               "  --width-policy narrow|wide  Select advisory ambiguous-width policy\n"
               "Queries require terminal stdin AND stdout. Raw report values are escaped.\n"
               "Replies are syntactic candidates, not authenticated terminal identity.\n";
}

bool parse(int argc, char **argv, Options &options) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);

    if (argument == "--no-query" || argument == "--headless") {
      options.query = false;

      continue;
    }

    if (index + 1 == argc) {
      return false;
    }

    const std::string_view value(argv[++index]);

    if (argument == "--timeout-ms") {
      if (!number(value, maximum_timeout_ms, options.timeout_ms)) {
        return false;
      }
    } else if (argument == "--max-bytes") {
      if (!number(value, maximum_byte_limit, options.byte_limit)) {
        return false;
      }
    } else if (argument == "--width-policy" && (value == "narrow" || value == "wide")) {
      options.width_policy = value == "narrow" ? "ambiguous-narrow" : "ambiguous-wide";
    } else {
      return false;
    }
  }

  return true;
}

void environment() {
  constexpr std::string_view names[] = {"TERM",       "TERM_PROGRAM",  "TERM_PROGRAM_VERSION", "COLORTERM",
                                        "WT_SESSION", "WT_PROFILE_ID", "ConEmuANSI",           "TMUX",
                                        "STY",        "SSH_TTY",       "WSL_DISTRO_NAME"};

  for (const auto name : names) {
    const char *value = std::getenv(name.data());
    const std::string_view raw = value == nullptr ? std::string_view{} : std::string_view(value);
    field("env." + std::string(name), value == nullptr ? "unset" : raw.substr(0, environment_limit));

    if (raw.size() > environment_limit) {
      field("env." + std::string(name) + ".truncated", "true");
    }
  }

  field("terminal.identity", "unknown (environment is a hint, not proof)");
  field("terminal.version", "unknown (TERM_PROGRAM_VERSION, if present, is only a hint)");
}
} // namespace

int main(int argc, char **argv) {
  for (int index = 1; index < argc; ++index) {
    if (std::string_view(argv[index]) == "--help") {
      help();

      return std::cout ? 0 : 1;
    }
  }

  Options options;

  if (!parse(argc, argv, options)) {
    std::cerr << "Invalid probe options; use --help.\n";

    return 2;
  }

  try {
    const auto facts = portals::probe::inspect_native();
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(options.timeout_ms);
    portals::probe::QueryCollector collector(deadline, options.byte_limit);
    portals::probe::RunResult result;

    if (!options.query) {
      result.stop = "disabled";
    } else if (!facts.query_eligible) {
      result.stop = "skipped: redirected or unavailable native terminal handles";
    } else {
      result = portals::probe::run_native_queries(collector, deadline, options.byte_limit);
    }

    collector.finish();
    const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    field("probe.format", "portals-terminal-probe-v1");

    for (const auto &[name, value] : facts.fields) {
      field(name, value);
    }

    environment();
    field("width.policy.selected", options.width_policy);
    field("width.policy.evidence", "advisory selection; Unicode version and actual terminal glyph widths unknown");
    field("query.timeout_ms", std::to_string(options.timeout_ms));
    field("query.max_bytes", std::to_string(options.byte_limit));
    field("query.elapsed_ms", std::to_string(elapsed.count()));
    field("query.stop", result.stop);
    field("query.error", result.error.empty() ? "none" : result.error);
    field("query.da.reply", collector.device_attributes().value_or("unknown: no matching reply observed"));
    field("query.cpr.reply", collector.cursor_position().value_or("unknown: no matching reply observed"));
    field("input.unmatched", collector.unmatched());
    field("input.bytes_consumed", std::to_string(collector.received()));
    field("input.byte_limit_hit", collector.limited() ? "true" : "false");
    field("input.non_byte_events", std::to_string(result.non_byte_events));
    field("input.event_summary", result.event_summary);
    field("query.ambiguity", "matching replies may be user input or stale replies; late replies are not drained");

    return result.error.empty() && std::cout ? 0 : 1;
  } catch (const std::exception &) {
    std::cerr << "Probe failed; native scope guards attempted to restore any acquired temporary modes.\n";

    return 1;
  }
}
