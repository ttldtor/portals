// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <vector>
#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
// Windows SDK declarations require this order, after Portals headers.
// clang-format off
#  include <windows.h>
#  include <psapi.h>
// clang-format on
#else
#  include <cerrno>
#  include <cstdio>
#  include <cstring>
#  include <sys/resource.h>
#  include <time.h>
#endif

// Internal qualification helpers; include Portals headers before this header on Windows.
namespace document_probe {
#ifdef _WIN32
inline constexpr auto PRIVATE_MEMORY_LABEL = "Windows PrivateUsage process private commit bytes";
inline constexpr auto PEAK_PRIVATE_MEMORY_LABEL = "Windows PeakPagefileUsage process peak commit bytes";
#else
inline constexpr auto PRIVATE_MEMORY_LABEL = "unavailable: Linux has no process private commit equivalent";
inline constexpr auto PEAK_PRIVATE_MEMORY_LABEL = "unavailable: Linux has no peak process private commit equivalent";
#endif
/** @brief Process counters in bytes and CPU nanoseconds; unavailable counters remain empty. */
struct ProcessMemory {
  std::optional<std::uint64_t> privateBytes, residentBytes, peakPrivateBytes, peakResidentBytes, cpuNs;
  int error{};
};

/** @return OS process counters; Linux has no equivalent of Windows private commit/peak private commit. */
inline ProcessMemory processMemory() noexcept {
  ProcessMemory result;
#ifdef _WIN32
  PROCESS_MEMORY_COUNTERS_EX memory{};
  memory.cb = sizeof(memory);

  if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory))) {
    result.privateBytes = memory.PrivateUsage;
    result.residentBytes = memory.WorkingSetSize;
    result.peakPrivateBytes = memory.PeakPagefileUsage;
    result.peakResidentBytes = memory.PeakWorkingSetSize;
  } else {
    result.error = static_cast<int>(GetLastError());
  }

  FILETIME creation{}, exit{}, kernel{}, user{};

  if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
    const auto ticks = [](FILETIME value) {
      return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
    };

    result.cpuNs = (ticks(kernel) + ticks(user)) * 100;
  } else {
    result.error = static_cast<int>(GetLastError());
  }
#else
  if (auto *status = std::fopen("/proc/self/status", "r")) {
    char line[256];
    unsigned long long kib{};

    while (std::fgets(line, sizeof(line), status)) {
      if (std::sscanf(line, "VmRSS: %llu kB", &kib) == 1) {
        result.residentBytes = kib * 1024;
      } else if (std::sscanf(line, "VmHWM: %llu kB", &kib) == 1) {
        result.peakResidentBytes = kib * 1024;
      }
    }

    std::fclose(status);
  } else {
    result.error = errno;
  }

  rusage usage{};

  if (getrusage(RUSAGE_SELF, &usage) == 0) {
    result.peakResidentBytes = static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
  }

  timespec cpu{};

  if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu) == 0) {
    result.cpuNs = static_cast<std::uint64_t>(cpu.tv_sec) * 1000000000 + cpu.tv_nsec;
  } else {
    result.error = errno;
  }
#endif
  return result;
}

/** @brief Fixed-capacity prefix samples; totals include observations discarded after capacity. */
class Samples {
public:
  /** @param capacity Maximum retained observations; reserve before timed work begins. */
  explicit Samples(std::size_t capacity) {
    values_.reserve(capacity);
  }

  /** @param value Observation in the caller's stated units. No allocation after construction. */
  void add(std::uint64_t value) {
    ++count_;

    if (values_.size() < values_.capacity()) {
      values_.push_back(value);
    }
  }

  /** @return Total observations, including discarded ones. */
  std::uint64_t count() const noexcept {
    return count_;
  }

  /** @return Number of retained prefix observations. */
  std::size_t retained() const noexcept {
    return values_.size();
  }

  /** @return True when no observation was discarded; an empty set still has no percentile. */
  bool complete() const noexcept {
    return count_ == values_.size();
  }

  /** @param fraction Quantile in [0,1]. @return Nearest-rank value of retained samples, or zero if empty. */
  std::uint64_t percentile(double fraction) const {
    if (values_.empty()) {
      return 0;
    }

    auto sorted = values_;
    std::sort(sorted.begin(), sorted.end());
    const auto rank = std::ceil(std::clamp(fraction, 0.0, 1.0) * static_cast<double>(sorted.size()));
    const auto index = rank > 0 ? static_cast<std::size_t>(rank) - 1 : 0;

    return sorted[std::min(index, sorted.size() - 1)];
  }

private:
  std::vector<std::uint64_t> values_;
  std::uint64_t count_{};
};
} // namespace document_probe
