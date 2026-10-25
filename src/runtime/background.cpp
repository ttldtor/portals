// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/background.hpp>
#include <portals/worker_storage.hpp>
#include <portals/detail/worker_storage_wake.hpp>

#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace portals {

void BackgroundJob::requestStop() noexcept {
  stop_.request_stop();
}

std::stop_token BackgroundJob::stopToken() const noexcept {
  return stop_.get_token();
}

BackgroundTicket::~BackgroundTicket() {
  cancel();
}

BackgroundTicket::BackgroundTicket(BackgroundTicket &&other) noexcept : job_(std::move(other.job_)) {
}

BackgroundTicket &BackgroundTicket::operator=(BackgroundTicket &&other) noexcept {
  if (this != &other) {
    cancel();
    job_ = std::move(other.job_);
  }

  return *this;
}

void BackgroundTicket::cancel() noexcept {
  if (auto job = job_.lock()) {
    job->requestStop();
  }

  job_.reset();
}

struct BackgroundPool::State {
  mutable std::mutex mutex;
  std::shared_ptr<detail::WorkerStorageWake> signal;
  BackgroundWorkerStorage storage;
  std::condition_variable_any &changed;
  Publisher<int> wake;
  std::vector<std::shared_ptr<BackgroundJob>> pending;
  std::vector<std::shared_ptr<BackgroundJob>> active;
  std::vector<std::jthread> workers;
  std::size_t head{}, count{};
  std::uint64_t capacity{}, retry{};
  bool closed{};

  State(BackgroundOptions options, Publisher<int> readiness)
      : signal(std::make_shared<detail::WorkerStorageWake>(readiness)),
        storage(options.storageSlots, options.storageBytes, signal), changed(signal->changed),
        wake(std::move(readiness)), pending(options.pending), active(options.workers) {
    workers.reserve(options.workers);
  }

  void work(std::size_t index) noexcept {
    for (;;) {
      const auto observed = signal->observe();
      storage.drain();
      std::shared_ptr<BackgroundJob> job;

      // The light epoch is sampled before readiness. Its mutex closes the
      // predicate-to-wait window without granting a lease ownership of State.
      signal->wait(observed, [&] {
        std::lock_guard lock(mutex);
        return closed || count != 0 || storage.retirementPending();
      });
      {
        std::unique_lock lock(mutex);

        if (closed) {
          return;
        }
        if (!count) {
          continue;
        }

        job = std::move(pending[head]);
        head = (head + 1) % pending.size();
        --count;
        active[index] = job;
        ++capacity;
      }

      // This advisory signal carries no callback, input, frame or UI owner.
      wake.wake();

      if (!job->stopToken().stop_requested()) {
        storage.execute(job.get(), [](void *context) noexcept {
          auto &running = *static_cast<BackgroundJob *>(context);
          running.run(running.stopToken());
        });
      }

      publish(*job);

      {
        std::lock_guard lock(mutex);

        active[index].reset();
      }
    }
  }

  void publish(BackgroundJob &job) noexcept {
    for (;;) {
      std::uint64_t observed{};

      {
        std::lock_guard lock(mutex);

        if (closed || job.stopToken().stop_requested()) {
          return;
        }

        observed = retry;
      }

      std::expected<void, AsyncError> result;

      try {
        result = job.publish();
      } catch (const std::bad_alloc &) {
        result = std::unexpected(AsyncError::NO_MEMORY);
      } catch (...) {
        // Custom jobs must contain publication exceptions. Never allow one to
        // terminate the process or repeatedly invoke an invalid producer.
        return;
      }

      if (result || (result.error() != AsyncError::WOULD_BLOCK && result.error() != AsyncError::NO_MEMORY)) {
        return;
      }

      std::unique_lock lock(mutex);

      // Sample before publication: a service concurrent with a rejected publish
      // cannot be lost between that rejection and this condition registration.
      changed.wait(lock, job.stopToken(), [&] {
        return closed || retry != observed;
      });
    }
  }
};

BackgroundPool::BackgroundPool(std::unique_ptr<State> state) : state_(std::move(state)) {
}

BackgroundPool::~BackgroundPool() {
  join();
}

std::expected<std::unique_ptr<BackgroundPool>, BackgroundError> BackgroundPool::create(BackgroundOptions options,
                                                                                       Publisher<int> wake) {
  if (!options.workers || !options.pending || !options.storageSlots || !options.storageBytes) {
    return std::unexpected(BackgroundError::INVALID_ARGUMENT);
  }

  try {
    auto pool = std::unique_ptr<BackgroundPool>(new BackgroundPool(std::make_unique<State>(options, std::move(wake))));

    for (std::size_t index = 0; index < options.workers; ++index) {
      pool->state_->workers.emplace_back([state = pool->state_.get(), index] {
        state->work(index);
      });
    }

    return pool;
  } catch (const std::bad_alloc &) {
    return std::unexpected(BackgroundError::NO_MEMORY);
  } catch (const std::system_error &) {
    return std::unexpected(BackgroundError::START_FAILED);
  } catch (const std::length_error &) {
    return std::unexpected(BackgroundError::INVALID_ARGUMENT);
  }
}

std::expected<BackgroundTicket, BackgroundError> BackgroundPool::submit(std::unique_ptr<BackgroundJob> &job) {
  if (!job) {
    return std::unexpected(BackgroundError::INVALID_ARGUMENT);
  }

  std::unique_lock lock(state_->mutex);

  if (state_->closed) {
    return std::unexpected(BackgroundError::CLOSED);
  }

  if (state_->count == state_->pending.size()) {
    return std::unexpected(BackgroundError::WOULD_BLOCK);
  }

  try {
    auto retained = std::shared_ptr<BackgroundJob>(std::move(job));

    state_->pending[(state_->head + state_->count) % state_->pending.size()] = retained;
    ++state_->count;

    lock.unlock();
    // The same condition also serves publication waiters with a different
    // predicate. Wake all so a blocked publisher cannot consume the sole signal.
    state_->signal->notify(false);

    return BackgroundTicket(retained);
  } catch (const std::bad_alloc &) {
    return std::unexpected(BackgroundError::NO_MEMORY);
  }
}

void BackgroundPool::service() noexcept {
  {
    std::lock_guard lock(state_->mutex);

    ++state_->retry;
  }

  state_->signal->notify(false);
}

std::uint64_t BackgroundPool::capacityEpoch() const noexcept {
  std::lock_guard lock(state_->mutex);

  return state_->capacity;
}

BackgroundWorkerStorage *BackgroundPool::workerStorage() noexcept {
  return &state_->storage;
}

void BackgroundPool::requestStop() noexcept {
  state_->storage.close();
  {
    std::lock_guard lock(state_->mutex);

    if (!state_->closed) {
      state_->closed = true;
      ++state_->capacity;
    }
  }

  state_->signal->notify(false);

  // Stop callbacks may reenter host code. Retain one job at a time and invoke
  // cancellation outside the mutex, without allocating a shutdown snapshot.
  for (std::size_t index = 0; index < state_->active.size() + state_->pending.size(); ++index) {
    std::shared_ptr<BackgroundJob> job;

    {
      std::lock_guard lock(state_->mutex);

      job = index < state_->active.size() ? state_->active[index] : state_->pending[index - state_->active.size()];
    }

    if (job) {
      job->requestStop();
    }
  }
}

void BackgroundPool::join() noexcept {
  requestStop();

  for (auto &worker : state_->workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

BackgroundLatest::~BackgroundLatest() {
  cancel();
}

std::expected<BackgroundAdmission, BackgroundError> BackgroundLatest::replace(std::unique_ptr<BackgroundJob> job) {
  if (!job) {
    return std::unexpected(BackgroundError::INVALID_ARGUMENT);
  }

  ticket_.cancel();
  pending_ = std::move(job);

  return admit();
}

std::expected<BackgroundAdmission, BackgroundError> BackgroundLatest::service() {
  if (!pending_) {
    return BackgroundAdmission::STARTED;
  }

  if (observed_ == executor_.capacityEpoch()) {
    return BackgroundAdmission::RETAINED;
  }

  return admit();
}

std::expected<BackgroundAdmission, BackgroundError> BackgroundLatest::admit() {
  observed_ = executor_.capacityEpoch();
  auto accepted = executor_.submit(pending_);

  if (accepted) {
    ticket_ = std::move(*accepted);

    return BackgroundAdmission::STARTED;
  }

  if (accepted.error() == BackgroundError::WOULD_BLOCK) {
    return BackgroundAdmission::RETAINED;
  }

  return std::unexpected(accepted.error());
}

void BackgroundLatest::cancel() noexcept {
  ticket_.cancel();
  pending_.reset();
}
} // namespace portals
