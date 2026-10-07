// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/scheduler.hpp>

#include <algorithm>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace portals {
namespace {
void increment(std::uint64_t &value, std::uint64_t amount = 1) {
  value += std::min(amount, std::numeric_limits<std::uint64_t>::max() - value);
}
SchedulerTime steady_time(void *) noexcept {
  return static_cast<SchedulerTime>(
    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace
namespace detail {
struct SchedulerRegistration {
  std::uint64_t id{};
  std::uint64_t scope{};
  std::function<void(std::shared_ptr<const void>)> callback;
  SchedulerLane lane{};
  SchedulerOverflow overflow{};
  bool timer{};
  SchedulerTime deadline{};
  bool active{true};
};
struct SchedulerPublication {
  std::uint64_t id{};
  std::shared_ptr<const void> value;
  SchedulerTime queued_at{};
};
struct SchedulerQueue {
  std::vector<SchedulerPublication> values;
  SchedulerQueueMetrics metrics;
  std::size_t capacity{};
};
struct SchedulerState {
  explicit SchedulerState(SchedulerOptions limits, SchedulerClock service, Scheduler::ErrorSink error_sink,
                          void *context)
      : options(limits), clock(service), sink(error_sink), sink_context(context) {
    if (!clock.read) {
      clock.read = steady_time;
    }
    data.capacity = options.data_capacity;
    control.capacity = options.control_capacity;
    data.values.reserve(data.capacity);
    control.values.reserve(control.capacity);
    registrations.reserve(options.subscription_capacity);
  }
  bool on_owner() const {
    return owner == std::this_thread::get_id();
  }
  SchedulerTime now() const {
    return clock.read(clock.context);
  }
  auto find(std::uint64_t id) {
    return std::find_if(registrations.begin(), registrations.end(), [id](const auto &entry) {
      return entry->id == id;
    });
  }
  std::expected<std::uint64_t, SchedulerError> next_identity() {
    if (next_id == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(SchedulerError::identity_exhausted);
    }

    return ++next_id;
  }
  void discard(SchedulerQueue &queue, std::uint64_t id) {
    for (;;) {
      SchedulerPublication removed;
      {
        std::lock_guard lock(mutex);
        auto found = std::find_if(queue.values.begin(), queue.values.end(), [id](const auto &entry) {
          return entry.id == id;
        });

        if (found == queue.values.end()) {
          return;
        }
        removed = std::move(*found);
        queue.values.erase(found);
        increment(queue.metrics.cancelled);
        queue.metrics.pending = queue.values.size();
      }
      // User-supplied snapshot deleters are never called while holding the lock.
    }
  }
  void disconnect(std::uint64_t id) {
    std::shared_ptr<SchedulerRegistration> removed;
    {
      std::lock_guard lock(mutex);
      auto found = find(id);

      if (found == registrations.end()) {
        return;
      }
      removed = std::move(*found);
      registrations.erase(found);
    }
    discard(data, id);
    discard(control, id);
    // The dispatcher may retain removed until its active invocation returns.
  }
  void close_scope(std::uint64_t id) {
    // Disable the entire scope before releasing any user storage. A destructor may reenter
    // publication while individual registrations are being reclaimed, but cannot revive delivery.
    {
      std::lock_guard lock(mutex);

      for (const auto &entry : registrations) {
        if (entry->scope == id) {
          entry->active = false;
        }
      }
    }

    for (;;) {
      std::uint64_t target{};
      {
        std::lock_guard lock(mutex);
        auto found = std::find_if(registrations.begin(), registrations.end(), [id](const auto &entry) {
          return entry->scope == id;
        });

        if (found == registrations.end()) {
          return;
        }
        target = (*found)->id;
      }
      disconnect(target);
    }
  }
  void reclaim() {
    // Detach all owners under the mutex, then destroy them on UI after unlock. Snapshot
    // deleters and callback captures can reenter without observing half-cleared queue bookkeeping.
    std::vector<std::shared_ptr<SchedulerRegistration>> removed;
    std::vector<SchedulerPublication> old_data;
    std::vector<SchedulerPublication> old_control;
    {
      std::lock_guard lock(mutex);
      removed.swap(registrations);
      old_data.swap(data.values);
      old_control.swap(control.values);
      increment(data.metrics.cancelled, old_data.size());
      increment(control.metrics.cancelled, old_control.size());
      data.metrics.pending = control.metrics.pending = 0;
    }
  }
  std::optional<SchedulerTime> next_timer() const {
    std::optional<SchedulerTime> result;

    for (const auto &entry : registrations) {
      if (entry->active && entry->timer && (!result || entry->deadline < *result)) {
        result = entry->deadline;
      }
    }

    return result;
  }
  SchedulerOptions options;
  SchedulerClock clock;
  Scheduler::ErrorSink sink;
  void *sink_context;
  const std::thread::id owner{std::this_thread::get_id()};
  mutable std::mutex mutex;
  std::condition_variable condition;
  SchedulerQueue data;
  SchedulerQueue control;
  std::vector<std::shared_ptr<SchedulerRegistration>> registrations;
  SchedulerMetrics totals;
  std::uint64_t next_id{};
  bool stopped{};
  bool wake_pending{};
  bool dispatching{}; // UI-only; never read by producers.
};
std::expected<PublishResult, SchedulerError> scheduler_publish(const std::shared_ptr<SchedulerState> &state,
                                                               std::uint64_t id, std::shared_ptr<const void> value) {
  if (!state) {
    return std::unexpected(SchedulerError::closed);
  }
  // value also holds any displaced snapshot until after unlock.
  std::unique_lock lock(state->mutex);

  if (state->stopped) {
    return std::unexpected(SchedulerError::closed);
  }
  auto found = state->find(id);

  if (found == state->registrations.end() || !(*found)->active) {
    return std::unexpected(SchedulerError::closed);
  }

  if (!value) {
    return std::unexpected(SchedulerError::invalid_argument);
  }
  const auto &registration = **found;
  auto &queue = registration.lane == SchedulerLane::control ? state->control : state->data;
  PublishResult result = PublishResult::queued;
  auto pending = queue.values.end();

  if (registration.overflow == SchedulerOverflow::replace_latest) {
    pending = std::find_if(queue.values.begin(), queue.values.end(), [id](const auto &entry) {
      return entry.id == id;
    });
  }

  // Replacement preserves this sender's FIFO position, including when the queue is full;
  // only its latency origin changes to the newly accepted snapshot.
  if (pending != queue.values.end()) {
    pending->value.swap(value);
    pending->queued_at = state->now();
    increment(queue.metrics.replaced);
    result = PublishResult::replaced;
  } else {
    if (queue.values.size() == queue.capacity) {
      increment(queue.metrics.rejected);

      return std::unexpected(SchedulerError::would_block);
    }
    queue.values.push_back({id, std::move(value), state->now()});
    queue.metrics.pending = queue.values.size();
    queue.metrics.high_water = std::max(queue.metrics.high_water, queue.metrics.pending);
  }
  increment(queue.metrics.accepted);
  lock.unlock();
  state->condition.notify_one();

  return result;
}
void scheduler_wake(const std::shared_ptr<SchedulerState> &state) noexcept {
  if (state) {
    {
      std::lock_guard lock(state->mutex);
      state->wake_pending = true;
    }
    state->condition.notify_one();
  }
}
void scheduler_request_stop(const std::shared_ptr<SchedulerState> &state) noexcept {
  if (state) {
    {
      std::lock_guard lock(state->mutex);
      state->stopped = true;
    }
    state->condition.notify_one();
  }
}
} // namespace detail

Subscription::Subscription(std::shared_ptr<detail::SchedulerState> state, std::uint64_t id)
    : state_(std::move(state)), id_(id) {
}
Subscription::~Subscription() {
  if (!disconnect()) {
    std::terminate();
  }
}
Subscription::Subscription(Subscription &&other) noexcept
    : state_(std::move(other.state_)), id_(std::exchange(other.id_, 0)) {
}
Subscription &Subscription::operator=(Subscription &&other) noexcept {
  if (this != &other) {
    if (!disconnect()) {
      std::terminate();
    }
    state_ = std::move(other.state_);
    id_ = std::exchange(other.id_, 0);
  }

  return *this;
}
std::expected<void, SchedulerError> Subscription::disconnect() {
  if (!state_ || !id_) {
    return {};
  }

  if (!state_->on_owner()) {
    return std::unexpected(SchedulerError::wrong_thread);
  }
  state_->disconnect(std::exchange(id_, 0));

  return {};
}
UiScope::UiScope(std::shared_ptr<detail::SchedulerState> state, std::uint64_t id) : state_(std::move(state)), id_(id) {
}
UiScope::~UiScope() {
  if (!close()) {
    std::terminate();
  }
}
UiScope::UiScope(UiScope &&other) noexcept : state_(std::move(other.state_)), id_(std::exchange(other.id_, 0)) {
}
UiScope &UiScope::operator=(UiScope &&other) noexcept {
  if (this != &other) {
    if (!close()) {
      std::terminate();
    }
    state_ = std::move(other.state_);
    id_ = std::exchange(other.id_, 0);
  }

  return *this;
}
std::expected<void, SchedulerError> UiScope::close() {
  if (!state_ || !id_) {
    return {};
  }

  if (!state_->on_owner()) {
    return std::unexpected(SchedulerError::wrong_thread);
  }
  state_->close_scope(std::exchange(id_, 0));

  return {};
}
Scheduler::Scheduler(std::shared_ptr<detail::SchedulerState> state) : state_(std::move(state)) {
}
Scheduler::~Scheduler() {
  if (!state_->on_owner() || state_->dispatching) {
    std::terminate();
  }
  (void)stop();
}
std::expected<std::unique_ptr<Scheduler>, SchedulerError>
Scheduler::create(SchedulerOptions options, SchedulerClock clock, ErrorSink sink, void *sink_context) {
  if (!options.data_capacity || !options.control_capacity || !options.data_budget || !options.control_budget ||
      !options.timer_budget || !options.subscription_capacity) {
    return std::unexpected(SchedulerError::invalid_argument);
  }
  try {
    auto state = std::make_shared<detail::SchedulerState>(options, clock, sink, sink_context);

    return std::unique_ptr<Scheduler>(new Scheduler(std::move(state)));
  } catch (const std::bad_alloc &) {
    return std::unexpected(SchedulerError::no_memory);
  } catch (const std::length_error &) {
    return std::unexpected(SchedulerError::invalid_argument);
  }
}
std::expected<UiScope, SchedulerError> Scheduler::make_scope() {
  if (!state_->on_owner()) {
    return std::unexpected(SchedulerError::wrong_thread);
  }
  std::lock_guard lock(state_->mutex);

  if (state_->stopped) {
    return std::unexpected(SchedulerError::closed);
  }
  auto id = state_->next_identity();

  if (!id) {
    return std::unexpected(id.error());
  }

  return UiScope(state_, *id);
}
std::expected<std::uint64_t, SchedulerError> Scheduler::add(const UiScope &scope,
                                                            std::function<void(std::shared_ptr<const void>)> callback,
                                                            SchedulerLane lane, SchedulerOverflow overflow, bool timer,
                                                            SchedulerTime deadline) {
  if (!state_->on_owner()) {
    return std::unexpected(SchedulerError::wrong_thread);
  }

  if (scope.state_ != state_) {
    return std::unexpected(SchedulerError::wrong_scheduler);
  }

  if (!scope.id_) {
    return std::unexpected(SchedulerError::closed);
  }

  if ((lane != SchedulerLane::data && lane != SchedulerLane::control) ||
      (overflow != SchedulerOverflow::reject && overflow != SchedulerOverflow::replace_latest) ||
      (lane == SchedulerLane::control && overflow == SchedulerOverflow::replace_latest)) {
    return std::unexpected(SchedulerError::invalid_argument);
  }
  auto registration = std::make_shared<detail::SchedulerRegistration>();
  registration->callback = std::move(callback);
  registration->scope = scope.id_;
  registration->lane = lane;
  registration->overflow = overflow;
  registration->timer = timer;
  registration->deadline = deadline;
  std::lock_guard lock(state_->mutex);

  if (state_->stopped) {
    return std::unexpected(SchedulerError::closed);
  }

  if (state_->registrations.size() == state_->options.subscription_capacity) {
    return std::unexpected(SchedulerError::would_block);
  }
  auto id = state_->next_identity();

  if (!id) {
    return std::unexpected(id.error());
  }
  registration->id = *id;
  state_->registrations.push_back(std::move(registration));

  return *id;
}
std::expected<SchedulerDispatch, SchedulerError> Scheduler::dispatch() {
  if (!state_->on_owner()) {
    return std::unexpected(SchedulerError::wrong_thread);
  }

  if (state_->dispatching) {
    return std::unexpected(SchedulerError::reentrant_dispatch);
  }
  state_->dispatching = true;
  struct DispatchGuard {
    bool &active;
    ~DispatchGuard() {
      active = false;
    }
  } guard{state_->dispatching};
  SchedulerDispatch result;
  auto invoke = [&](const std::shared_ptr<detail::SchedulerRegistration> &registration,
                    std::shared_ptr<const void> value) {
    try {
      registration->callback(std::move(value));
    } catch (...) {
      state_->disconnect(registration->id);
      ++result.callback_failures;
      {
        std::lock_guard lock(state_->mutex);
        increment(state_->totals.callback_failures);
      }

      if (state_->sink) {
        state_->sink(state_->sink_context, SchedulerError::callback_failed);
      }
    }
  };
  // Selecting a strong registration under the mutex is callback admission. Invocation and
  // owner release happen outside it, so self-disconnect/stop cannot destroy active captures.
  auto drain = [&](detail::SchedulerQueue &queue, std::size_t budget, std::size_t &count) {
    for (std::size_t step = 0; step < budget; ++step) {
      detail::SchedulerPublication publication;
      std::shared_ptr<detail::SchedulerRegistration> registration;
      {
        std::lock_guard lock(state_->mutex);

        if (state_->stopped || queue.values.empty()) {
          break;
        }
        publication = std::move(queue.values.front());
        queue.values.erase(queue.values.begin());
        queue.metrics.pending = queue.values.size();
        auto found = state_->find(publication.id);

        if (found != state_->registrations.end() && (*found)->active) {
          registration = *found;
          const auto now = state_->now();
          queue.metrics.last_latency = now >= publication.queued_at ? now - publication.queued_at : 0;
          queue.metrics.max_latency = std::max(queue.metrics.max_latency, queue.metrics.last_latency);
          increment(queue.metrics.total_latency, queue.metrics.last_latency);
          increment(queue.metrics.delivered);
        } else {
          increment(queue.metrics.cancelled);
        }
      }

      if (registration) {
        ++count;
        invoke(registration, std::move(publication.value));
      }
    }
  };
  drain(state_->control, state_->options.control_budget, result.control);

  // The registration vector preserves insertion order. Strict deadline comparison therefore
  // selects the earliest timer and breaks equal-deadline ties by registration order. Removing
  // it before invocation makes the timer one-shot even when its callback reenters registration.
  for (std::size_t step = 0; step < state_->options.timer_budget; ++step) {
    std::shared_ptr<detail::SchedulerRegistration> registration;
    {
      std::lock_guard lock(state_->mutex);

      if (state_->stopped) {
        break;
      }
      const auto now = state_->now();
      auto earliest = state_->registrations.end();

      for (auto it = state_->registrations.begin(); it != state_->registrations.end(); ++it) {
        if ((*it)->active && (*it)->timer && (*it)->deadline <= now &&
            (earliest == state_->registrations.end() || (*it)->deadline < (*earliest)->deadline)) {
          earliest = it;
        }
      }

      if (earliest == state_->registrations.end()) {
        break;
      }
      registration = std::move(*earliest);
      state_->registrations.erase(earliest);
      increment(state_->totals.timer_callbacks);
    }
    ++result.timers;
    invoke(registration, {});
  }
  drain(state_->data, state_->options.data_budget, result.data);
  {
    std::lock_guard lock(state_->mutex);
    increment(state_->totals.cycles);
    result.stopped = state_->stopped;
  }

  if (result.stopped) {
    state_->reclaim();
  }

  return result;
}
std::expected<SchedulerWake, SchedulerError> Scheduler::wait_for(std::chrono::milliseconds timeout) {
  if (!state_->on_owner()) {
    return std::unexpected(SchedulerError::wrong_thread);
  }

  if (state_->dispatching) {
    return std::unexpected(SchedulerError::reentrant_dispatch);
  }

  if (timeout.count() < 0) {
    return std::unexpected(SchedulerError::invalid_argument);
  }
  // Clamp before addition to avoid overflowing the native steady-clock deadline.
  const auto start = std::chrono::steady_clock::now();
  const auto remaining =
    std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::time_point::max() - start);
  const auto deadline = start + std::min(timeout, remaining);
  std::unique_lock lock(state_->mutex);

  for (;;) {
    if (state_->stopped) {
      return SchedulerWake::stopped;
    }
    const auto next = state_->next_timer();
    const auto now = state_->now();

    if (state_->wake_pending || !state_->control.values.empty() || !state_->data.values.empty() ||
        (next && *next <= now)) {
      state_->wake_pending = false;

      return SchedulerWake::ready;
    }
    auto native_now = std::chrono::steady_clock::now();

    if (native_now >= deadline) {
      return SchedulerWake::timeout;
    }
    // The injected clock determines readiness; the native steady clock bounds real waiting.
    // Recheck both after wakeups, including spurious wakes and explicit fake-clock advances.
    auto until = deadline;

    if (next) {
      auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - native_now).count();

      if (*next - now <= static_cast<SchedulerTime>(maximum)) {
        until = native_now + std::chrono::milliseconds(*next - now);
      }
    }
    state_->condition.wait_until(lock, until);
  }
}
void Scheduler::request_stop() noexcept {
  detail::scheduler_request_stop(state_);
}
std::expected<void, SchedulerError> Scheduler::stop() {
  if (!state_->on_owner()) {
    return std::unexpected(SchedulerError::wrong_thread);
  }
  request_stop();
  state_->reclaim();

  return {};
}
SchedulerMetrics Scheduler::metrics() const {
  std::lock_guard lock(state_->mutex);
  auto result = state_->totals;
  result.data = state_->data.metrics;
  result.control = state_->control.metrics;

  return result;
}
} // namespace portals
