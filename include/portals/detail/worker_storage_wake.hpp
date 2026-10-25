// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/scheduler.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>

namespace portals::detail {
/// @cond INTERNAL
// This shared light object has no path back to payloads or the uniquely owned host State.
struct WorkerStorageWake {
  std::condition_variable_any changed;
  std::atomic<bool> retired{};
  std::atomic<bool> closed{};
  Publisher<int> ui;

  explicit WorkerStorageWake(Publisher<int> readiness) : ui(std::move(readiness)) {
  }

  std::uint64_t observe() noexcept {
    std::lock_guard lock(mutex_);
    return epoch_;
  }

  template <class Predicate> void wait(std::uint64_t observed, Predicate ready) {
    std::unique_lock lock(mutex_);
    changed.wait(lock, [&] {
      return epoch_ != observed || ready();
    });
  }

  void notify(bool wakeUi = true) noexcept {
    {
      std::lock_guard lock(mutex_);
      ++epoch_;
    }
    changed.notify_all();
    if (wakeUi) {
      ui.wake();
    }
  }

private:
  std::mutex mutex_;
  std::uint64_t epoch_{};
};

/// @endcond
} // namespace portals::detail
