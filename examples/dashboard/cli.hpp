// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "dashboard.hpp"
#include <portals/ui_lyra.hpp>

namespace dashboard {
/** @brief Native display options, parsed before mounting the scene. */
struct DisplayOptions {
  std::uint32_t framesPerSecond{30}; ///< Maximum submitted frames per second.
  bool ansi16{};                     ///< Use the terminal's sixteen-color palette.
  std::string mode{"synthetic"};     ///< Synthetic prices or the recorded data tape.
};

/** @return Native Lyra schema; validation runs in the standard application runner. */
inline auto commandLine(Options &data, DisplayOptions &display) {
  return lyra::cli()
    | lyra::opt(data.rate, "ticks")["--rate"]("Ticks per second").choices([](std::uint32_t value) {
        return value <= 10000;
      })
    | lyra::opt(display.framesPerSecond, "fps")["--fps"]("Maximum frames per second").choices([](std::uint32_t value) {
        return value > 0 && value <= 1000;
      })
    | lyra::opt(data.rows, "count")["--rows"]("Logical instruments").choices([](std::uint64_t value) {
        return value > 0 && value <= 1000000000;
      })
    | lyra::opt(data.seed, "number")["--seed"]("Synthetic price seed")
    | lyra::opt(data.maxTicksPerPump, "count")["--pump-budget"]("Maximum tick work per callback")
        .choices([](std::size_t value) {
          return value > 0 && value <= 10000;
        })
    | lyra::opt(display.mode, "source")["--mode"]("Price source").choices("synthetic", "replay")
    | lyra::opt(data.delayMs, "ms")["--delay-ms"]("Page acquisition delay").choices([](std::uint64_t value) {
        return value <= 60000;
      })
    | lyra::opt(data.durationMs, "ms")["--duration-ms"]("Stop after this duration; zero stays interactive")
        .choices([](std::uint64_t value) {
          return value <= 3600000;
        })
    | lyra::opt(data.loading.pageRows, "count")["--page-rows"]("Rows per page").choices([](std::size_t value) {
        return value > 0;
      })
    | lyra::opt(data.loading.cachePages, "count")["--cache-pages"]("Maximum cached pages")
        .choices([](std::size_t value) {
          return value > 0;
        })
    | lyra::opt(data.loading.cacheBytes, "bytes")["--cache-bytes"]("Charged page payload budget")
        .choices([](std::size_t value) {
          return value > 0;
        })
    | lyra::opt(data.loading.backendJobs, "count")["--load-jobs"]("Maximum simultaneous page loads")
        .choices([](std::size_t value) {
          return value > 0;
        })
    | lyra::opt(data.loading.prefetchPages, "count")["--prefetch-pages"]("Adjacent pages to prefetch")
    | lyra::opt(data.ascii)["--ascii"]("Use ASCII chart glyphs")
    | lyra::opt(display.ansi16)["--ansi16"]("Use sixteen ANSI colors");
}
} // namespace dashboard
