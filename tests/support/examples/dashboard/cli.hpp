// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include "../gallery/application_report.hpp"
#include "../replay/terminal.hpp"
#include "dashboard.hpp"
#include <charconv>
#include <fstream>

namespace dashboard_test {
inline int run(int argc, char **argv, bool replayDefault = false) {
  try {
    Options options;
    ApplicationOptions application;
    options.replay = replayDefault;
    options.stress = false;
    bool check{};
    std::string trace;

    for (int i = 1; i < argc; ++i) {
      const std::string_view argument = argv[i];

      if (argument == "--check") {
        check = true;

        continue;
      }

      if (argument == "--ascii") {
        options.ascii = true;

        continue;
      }

      if (argument == "--stress") {
        options.stress = true;

        continue;
      }

      if (argument == "--ansi16") {
        application.writer.colorMode = ColorMode::ANSI16;

        continue;
      }

      if (argument == "--help") {
        std::cout
          << "Usage: portals_dashboard [--rate 0..10000] [--fps 1..1000] [--rows 1..1000000000]\n"
             "  [--seed N] [--pump-budget 1..10000] [--mode synthetic|replay] [--delay-ms N] [--duration-ms N]\n"
             "  [--page-rows N] [--cache-pages N] [--cache-bytes N] [--load-jobs N] [--prefetch-pages N]\n"
             "  [--ascii] [--ansi16] [--stress] [--trace FILE] [--check]\n"
             "The default view is compact; --stress keeps the detailed diagnostic layout.\n"
             "Loading defaults: 256 rows/page, 8 pages, 262144 charged bytes, 4 jobs, 1 prefetch page.\n"
             "Keys: Tab changes focus, arrows/Home/End select and scroll the table,\n"
             "Enter commits the note, F2 changes theme, F10/Ctrl+C quits.\n";

        return 0;
      }

      if (++i == argc) {
        throw std::runtime_error("missing option value; use --help");
      }

      const std::string_view value = argv[i];

      if (argument == "--trace") {
        trace = value;

        continue;
      }

      if (argument == "--mode") {
        if (value != "synthetic" && value != "replay") {
          throw std::runtime_error("unknown data mode");
        }

        options.replay = value == "replay";

        continue;
      }

      std::uint64_t number{};
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);

      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
        throw std::runtime_error("invalid numeric option");
      }

      if (argument == "--rate" && number <= 10000) {
        options.rate = static_cast<std::uint32_t>(number);
      } else if (argument == "--fps" && number && number <= 1000) {
        application.framesPerSecond = static_cast<std::uint32_t>(number);
      } else if (argument == "--rows" && number && number <= 1000000000) {
        options.rows = number;
      } else if (argument == "--pump-budget" && number && number <= 10000) {
        options.maxTicksPerPump = static_cast<std::size_t>(number);
      } else if (argument == "--seed") {
        options.seed = number;
      } else if (argument == "--delay-ms" && number <= 60000) {
        options.delayMs = number;
      } else if (argument == "--duration-ms" && number <= 3600000) {
        options.durationMs = number;
      } else if (argument == "--page-rows" && number && number <= std::numeric_limits<std::size_t>::max()) {
        options.loading.pageRows = static_cast<std::size_t>(number);
      } else if (argument == "--cache-pages" && number && number <= std::numeric_limits<std::size_t>::max()) {
        options.loading.cachePages = static_cast<std::size_t>(number);
      } else if (argument == "--cache-bytes" && number && number <= std::numeric_limits<std::size_t>::max()) {
        options.loading.cacheBytes = static_cast<std::size_t>(number);
      } else if (argument == "--load-jobs" && number && number <= std::numeric_limits<std::size_t>::max()) {
        options.loading.backendJobs = static_cast<std::size_t>(number);
      } else if (argument == "--prefetch-pages" && number <= std::numeric_limits<std::size_t>::max()) {
        options.loading.prefetchPages = static_cast<std::size_t>(number);
      } else {
        throw std::runtime_error("unknown option or unsupported value; use --help");
      }
    }

    std::shared_ptr<Diagnostics> diagnostics;

    if (!trace.empty()) {
      diagnostics = std::make_shared<Diagnostics>();
      take(diagnostics->enable({8192}));
      application.scheduler.diagnostics = diagnostics;
    }

    example::ReplayTerminal terminal(
      {{100, "\x1bOQ", {}}, {200, "\t\x01Replay note\r", {}}, {300, "\t\r", {}}, {600, "", CellSize{100, 30}}}, 1000);
    auto app = take(Application::create(application, check ? terminal.clock() : SchedulerClock{}));
    Scene scene(*app, options);

    const auto result = check ? app->run(scene, terminal) : app->run(scene);
    // Both native and custom run have explicitly restored/closed their transport here.
    const int status = example::report(result);

    if (diagnostics) {
      std::ofstream file(trace, std::ios::binary | std::ios::trunc);
      file << exportDiagnostics(diagnostics->snapshot());
      file.close();

      if (!file) {
        std::cerr << "Cannot export diagnostics to " << trace << '\n';

        return 1;
      }
    }

    if (status) {
      return status;
    }

    if (check) {
      const auto metrics = app->scheduler().metrics();
      std::cout << std::format("frames={} ticks={} bytes={} coherent={} control_high_water={} data_high_water={}\n",
                               result->frames, scene.tickCount(), result->bytes, scene.coherent(),
                               metrics.control.high_water, metrics.data.high_water);

      const auto loading = scene.loadingMetrics();

      std::cout << std::format("cache_hits={} cache_misses={} useful_loads={} cancelled_loads={} "
                               "peak_pages={} peak_jobs={} peak_charged_bytes={}\n",
                               loading.cacheHits, loading.cacheMisses, loading.usefulLoads, loading.cancelledLoads,
                               loading.peakPages, loading.peakJobs, loading.peakBytes);

      if (!terminal.closed || !scene.coherent() || result->frames > application.framesPerSecond + 1) {
        return 1;
      }
    }

    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';

    return 1;
  }
}
} // namespace dashboard_test
