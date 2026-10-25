// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/worker_storage.hpp>
#include <portals/detail/worker_storage_wake.hpp>

#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace portals {
namespace detail {
struct WorkerStorageControl {
  std::atomic<bool> retired{};
  std::weak_ptr<WorkerStorageWake> wake;
};
}
namespace {
thread_local const void *executingStorage{};

template <class Callback, class... Args>
std::expected<void, BackgroundError> invokeStorage(const Callback &callback, Args &&...args) noexcept {
  try {
    return callback(std::forward<Args>(args)...);
  } catch (const std::bad_alloc &) {
    return std::unexpected(BackgroundError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(BackgroundError::CALLBACK_FAILED);
  }
}
}

WorkerStorageLease::~WorkerStorageLease() { retire(); }
WorkerStorageLease::WorkerStorageLease(WorkerStorageLease &&other) noexcept
    : id_(std::exchange(other.id_, {})), control_(std::move(other.control_)) {}
WorkerStorageLease &WorkerStorageLease::operator=(WorkerStorageLease &&other) noexcept {
  if (this != &other) {
    retire();
    id_ = std::exchange(other.id_, {});
    control_ = std::move(other.control_);
  }

  return *this;
}
void WorkerStorageLease::retire() noexcept {
  if (auto control = control_.lock(); control && !control->retired.exchange(true)) {
    if (auto wake = control->wake.lock()) {
      wake->retired.store(true);
      wake->notify();
    }
  }
  control_.reset();
  id_ = {};
}

std::expected<void, BackgroundError> WorkerStorageBuild::commit(std::unique_ptr<WorkerStorageValue> &value,
                                                               std::size_t bytes) {
  if (!value || value_ || !bytes) {
    return std::unexpected(BackgroundError::INVALID_ARGUMENT);
  }
  if (bytes > residentCap_) {
    return std::unexpected(BackgroundError::LIMIT_EXCEEDED);
  }
  value_ = std::move(value);
  bytes_ = bytes;

  return {};
}

struct BackgroundWorkerStorage::State {
  struct Slot {
    std::shared_ptr<detail::WorkerStorageControl> control;
    std::unique_ptr<WorkerStorageValue> value;
    std::uint64_t generation{}, touched{};
    std::size_t bytes{}, readers{};
    bool building{}, reclaiming{};
  };
  mutable std::mutex mutex;
  std::shared_ptr<detail::WorkerStorageWake> wake;
  std::vector<Slot> slots;
  std::size_t limit{}, used{}, peak{};
  std::uint64_t generation{}, epoch{}, clock{}, evictions{};
  bool closed{};

  State(std::size_t count, std::size_t bytes, std::shared_ptr<detail::WorkerStorageWake> signal)
      : wake(std::move(signal)), slots(count), limit(bytes) {}
  Slot *find(WorkerStorageId id) noexcept {
    if (!id.value || id.value > slots.size()) {
      return nullptr;
    }
    auto &slot = slots[static_cast<std::size_t>(id.value - 1)];

    return slot.control && slot.generation == id.generation ? &slot : nullptr;
  }
  void changed() noexcept {
    ++epoch;
    wake->notify();
  }
};

BackgroundWorkerStorage::BackgroundWorkerStorage(std::size_t slots, std::size_t bytes,
                                                 std::shared_ptr<detail::WorkerStorageWake> wake)
    : state_(std::make_unique<State>(slots, bytes, std::move(wake))) {}
BackgroundWorkerStorage::~BackgroundWorkerStorage() { close(); }

std::expected<WorkerStorageLease, BackgroundError> BackgroundWorkerStorage::registerSlot() {
  try {
    auto control = std::make_shared<detail::WorkerStorageControl>();
    control->wake = state_->wake;
    std::lock_guard lock(state_->mutex);

    if (state_->closed) {
      return std::unexpected(BackgroundError::CLOSED);
    }
    if (state_->generation == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(BackgroundError::LIMIT_EXCEEDED);
    }
    for (std::size_t index = 0; index < state_->slots.size(); ++index) {
      auto &slot = state_->slots[index];

      if (!slot.control) {
        slot.control = std::move(control);
        slot.generation = ++state_->generation;

        return WorkerStorageLease({index + 1, slot.generation}, slot.control);
      }
    }

    return std::unexpected(BackgroundError::WOULD_BLOCK);
  } catch (const std::bad_alloc &) {
    return std::unexpected(BackgroundError::NO_MEMORY);
  }
}

std::uint64_t BackgroundWorkerStorage::changeEpoch() const noexcept {
  std::lock_guard lock(state_->mutex);

  return state_->epoch;
}
WorkerStorageStats BackgroundWorkerStorage::stats() const noexcept {
  std::lock_guard lock(state_->mutex);
  std::size_t count{};

  for (const auto &slot : state_->slots) {
    count += bool(slot.control);
  }

  return {state_->used, state_->peak, count, state_->evictions};
}

void BackgroundWorkerStorage::execute(void *context, void (*run)(void *) noexcept) noexcept {
  const auto previous = std::exchange(executingStorage, state_.get());

  run(context);
  executingStorage = previous;
}

std::expected<void, BackgroundError> BackgroundWorkerStorage::withRead(
    WorkerStorageId id,
    const std::function<std::expected<void, BackgroundError>(const WorkerStorageValue &)> &read) {
  if (executingStorage != state_.get()) {
    return std::unexpected(BackgroundError::WRONG_THREAD);
  }
  WorkerStorageValue *value{};
  {
    std::lock_guard lock(state_->mutex);
    auto slot = state_->find(id);

    if (state_->closed || !slot || slot->control->retired.load()) {
      return std::unexpected(BackgroundError::CLOSED);
    }
    if (!slot->value || slot->building || slot->reclaiming) {
      return std::unexpected(BackgroundError::WOULD_BLOCK);
    }
    ++slot->readers;
    slot->touched = ++state_->clock;
    value = slot->value.get();
  }
  const auto result = invokeStorage(read, *value);
  {
    std::lock_guard lock(state_->mutex);
    --state_->find(id)->readers;
    state_->changed();
  }
  drain();

  return result;
}

std::expected<void, BackgroundError> BackgroundWorkerStorage::withBuild(
    WorkerStorageId id, std::size_t residentCap, std::size_t scratchCap,
    const std::function<std::expected<void, BackgroundError>(WorkerStorageBuild &)> &build) {
  if (executingStorage != state_.get()) {
    return std::unexpected(BackgroundError::WRONG_THREAD);
  }
  if (!residentCap || residentCap > state_->limit || scratchCap > state_->limit - residentCap) {
    return std::unexpected(BackgroundError::LIMIT_EXCEEDED);
  }
  const auto reservation = residentCap + scratchCap;
  // Reclaim current/idle values one at a time. Charges survive detach until destruction completes.
  for (;;) {
    std::unique_ptr<WorkerStorageValue> victim;
    State::Slot *reclaim{};
    std::size_t bytes{};
    {
      std::lock_guard lock(state_->mutex);
      auto slot = state_->find(id);

      if (state_->closed || !slot || slot->control->retired.load()) {
        return std::unexpected(BackgroundError::CLOSED);
      }
      if (slot->readers || slot->building || slot->reclaiming) {
        return std::unexpected(BackgroundError::WOULD_BLOCK);
      }
      if (slot->value) {
        reclaim = slot;
      } else if (reservation <= state_->limit - state_->used) {
        state_->used += reservation;
        state_->peak = std::max(state_->peak, state_->used);
        slot->building = true;

        break;
      } else {
        for (auto &candidate : state_->slots) {
          if (candidate.value && !candidate.readers && !candidate.building && !candidate.reclaiming &&
              (!reclaim || candidate.touched < reclaim->touched)) {
            reclaim = &candidate;
          }
        }
        if (!reclaim) {
          return std::unexpected(BackgroundError::WOULD_BLOCK);
        }
        ++state_->evictions;
      }
      reclaim->reclaiming = true;
      bytes = reclaim->bytes;
      victim = std::move(reclaim->value);
    }
    victim.reset();
    {
      std::lock_guard lock(state_->mutex);
      reclaim->bytes = 0;
      reclaim->reclaiming = false;
      state_->used -= bytes;
      state_->changed();
    }
  }

  WorkerStorageBuild prepared(residentCap, scratchCap);
  auto result = invokeStorage(build, prepared);
  // Callback locals (including scratch) have already died before any reservation shrinks.
  {
    std::lock_guard lock(state_->mutex);
    auto slot = state_->find(id);

    if (result && !prepared.value_) {
      result = std::unexpected(BackgroundError::INVALID_ARGUMENT);
    }
    if (state_->closed || slot->control->retired.load()) {
      result = std::unexpected(BackgroundError::CLOSED);
    }
    if (result) {
      slot->value = std::move(prepared.value_);
      slot->bytes = prepared.bytes_;
      slot->touched = ++state_->clock;
    }
  }
  // A rejected commit stays charged and is destroyed on this worker outside the mutex.
  prepared.value_.reset();
  {
    std::lock_guard lock(state_->mutex);
    auto slot = state_->find(id);
    state_->used -= reservation - slot->bytes;
    slot->building = false;
    state_->changed();
  }
  drain();

  return result;
}

bool BackgroundWorkerStorage::retirementPending() const noexcept { return state_->wake->retired.load(); }
void BackgroundWorkerStorage::drain() noexcept {
  state_->wake->retired.store(false);

  for (auto &slot : state_->slots) {
    std::unique_ptr<WorkerStorageValue> victim;
    std::size_t bytes{};
    {
      std::lock_guard lock(state_->mutex);

      if (!slot.control || !slot.control->retired.load() || slot.readers || slot.building || slot.reclaiming) {
        continue;
      }
      slot.reclaiming = true;
      bytes = slot.bytes;
      victim = std::move(slot.value);
    }
    victim.reset();
    {
      std::lock_guard lock(state_->mutex);
      state_->used -= bytes;
      slot.bytes = 0;
      slot.control.reset();
      slot.reclaiming = false;
      state_->changed();
    }
  }
}
void BackgroundWorkerStorage::close() noexcept {
  std::lock_guard lock(state_->mutex);
  state_->closed = true;
  state_->wake->closed.store(true);
  state_->changed();
}
} // namespace portals
