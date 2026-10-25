// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstddef>

namespace portals {
class BackgroundExecutor;

/** @brief Executor admission and startup failures; domain errors stay in the result value. */
enum class BackgroundError {
  INVALID_ARGUMENT, ///< Null job or zero worker/pending capacity.
  UNAVAILABLE,      ///< A hosted session requires a borrowed executor.
  CLOSED,           ///< Executor no longer accepts work.
  WOULD_BLOCK,      ///< Pending capacity is full; ownership has not transferred.
  NO_MEMORY,        ///< An executor allocation failed.
  START_FAILED,     ///< A worker thread could not be created.
  WRONG_THREAD,     ///< Worker storage was accessed outside this executor's run extent.
  LIMIT_EXCEEDED,   ///< A storage request cannot fit its configured aggregate budget.
  CALLBACK_FAILED  ///< A worker storage callback threw an exception.
};

/** @brief Count bounds, independent of the transitive byte size of inputs and results. */
struct BackgroundOptions {
  std::size_t workers{2};  ///< Positive fixed worker count; unpublished results retain these reservations.
  std::size_t pending{16}; ///< Positive pending count, excluding running or unpublished work.
  std::size_t storageSlots{16}; ///< Light registered-plus-retiring worker cache slots.
  std::size_t storageBytes{536870912}; ///< Aggregate resident/build reservation bytes; not per slot.
};
} // namespace portals
