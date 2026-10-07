// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <new>
#include <utility>

namespace portals {
/** @brief Monotonic milliseconds; the clock must not move backwards. */
using SchedulerTime = std::uint64_t;
/** @brief Observable operation failures; no callback is started on rejection. */
enum class SchedulerError {
  invalid_argument,
  wrong_thread,
  wrong_scheduler,
  closed,
  would_block,
  no_memory,
  reentrant_dispatch,
  identity_exhausted,
  callback_failed
};
/** @brief Independent finite queues; control is serviced before data each cycle. */
enum class SchedulerLane {
  data,
  control
};
/** @brief Replace only this sender's pending display snapshot; never raw domain events. */
enum class SchedulerOverflow {
  reject,
  replace_latest
};
/** @brief Whether a publication appended a value or replaced its sender's pending value. */
enum class PublishResult {
  queued,
  replaced
};
/** @brief A finite wait ended by work, explicit wake, stop or timeout. */
enum class SchedulerWake {
  ready,
  stopped,
  timeout
};
/** @brief Borrowed clock service, invoked under the scheduler mutex on UI or producer threads.
 * read must be thread-safe, nonblocking and non-reentrant. Context must outlive Scheduler;
 * stopped publishers never read it. Null read selects the steady clock.
 */
struct SchedulerClock {
  /** @brief Reader signature; callbacks cannot throw or reenter the scheduler. */
  using Reader = SchedulerTime (*)(void *) noexcept;
  Reader read{};   ///< Monotonic millisecond reader.
  void *context{}; ///< Borrowed reader context.
};
/** @brief Finite resource limits and maximum work per dispatch; all fields must be positive. */
struct SchedulerOptions {
  std::size_t data_capacity{256};         ///< Maximum queued data owners.
  std::size_t control_capacity{32};       ///< Maximum queued keyboard/resize/quit owners.
  std::size_t data_budget{32};            ///< Maximum data deliveries per cycle.
  std::size_t control_budget{32};         ///< Maximum control deliveries per cycle.
  std::size_t timer_budget{32};           ///< Maximum due one-shot timers per cycle.
  std::size_t subscription_capacity{256}; ///< Maximum live subscriptions and timers together.
};
/** @brief Queue counters saturate at UINT64_MAX; latency is measured in clock milliseconds. */
struct SchedulerQueueMetrics {
  std::size_t pending{};         ///< Current occupancy.
  std::size_t high_water{};      ///< Largest occupancy.
  std::uint64_t accepted{};      ///< Successful publications, including replacements.
  std::uint64_t rejected{};      ///< Publications rejected for capacity.
  std::uint64_t replaced{};      ///< Pending display values replaced.
  std::uint64_t delivered{};     ///< Callback invocations, including throwing callbacks.
  std::uint64_t cancelled{};     ///< Queued values discarded by disconnect/stop.
  SchedulerTime last_latency{};  ///< Delay from most recent accepted value to invocation.
  SchedulerTime max_latency{};   ///< Maximum observed queue delay.
  SchedulerTime total_latency{}; ///< Saturating sum of delivered queue delays.
};
/** @brief Thread-safe, copied metrics; no retained samples or unbounded history. */
struct SchedulerMetrics {
  SchedulerQueueMetrics data;        ///< Data queue evidence.
  SchedulerQueueMetrics control;     ///< Control queue evidence.
  std::uint64_t timer_callbacks{};   ///< Invoked one-shot timers.
  std::uint64_t callback_failures{}; ///< Exceptions caught at dispatch.
  std::uint64_t cycles{};            ///< Complete or stop-interrupted dispatch calls.
};
/** @brief One bounded dispatch result; stop can interrupt the cycle after the current callback. */
struct SchedulerDispatch {
  std::size_t control{};           ///< Control callbacks started.
  std::size_t timers{};            ///< Timer callbacks started.
  std::size_t data{};              ///< Data callbacks started.
  std::size_t callback_failures{}; ///< Exceptions reported to the error sink.
  bool stopped{};                  ///< Stop was requested; resources were reclaimed on UI.
};
namespace detail {
struct SchedulerState;
std::expected<PublishResult, SchedulerError> scheduler_publish(const std::shared_ptr<SchedulerState> &, std::uint64_t,
                                                               std::shared_ptr<const void>);
void scheduler_wake(const std::shared_ptr<SchedulerState> &) noexcept;
void scheduler_request_stop(const std::shared_ptr<SchedulerState> &) noexcept;
} // namespace detail
class Scheduler;
/** @brief UI-owned move-only cancellation token. Release before Scheduler.
 * Disconnect is immediate logically; an executing callback owns its captures until return.
 * Destruction/move assignment on a foreign thread terminates (affinity contract violation).
 */
class Subscription {
public:
  Subscription() = default;                               ///< Empty disconnected token.
  ~Subscription();                                        ///< Disconnect on UI; never joins a worker.
  Subscription(Subscription &&other) noexcept;            ///< Transfer ownership on UI.
  Subscription &operator=(Subscription &&other) noexcept; ///< Disconnect old token, then transfer.
  Subscription(const Subscription &) = delete;
  Subscription &operator=(const Subscription &) = delete;
  std::expected<void, SchedulerError> disconnect(); ///< UI-only, idempotent; wrong_thread leaves unchanged.
private:
  friend class Scheduler;
  Subscription(std::shared_ptr<detail::SchedulerState> state, std::uint64_t id);
  std::shared_ptr<detail::SchedulerState> state_;
  std::uint64_t id_{};
};
/** @brief UI-owned scope disconnecting all its subscriptions and timers, in any token order.
 * Owned callback captures survive self-close until the active dispatch returns. Arbitrary
 * reference captures remain the caller's responsibility. Release before Scheduler.
 */
class UiScope {
public:
  UiScope() = default;                          ///< Empty closed scope.
  ~UiScope();                                   ///< Close on UI; wrong-thread destruction terminates.
  UiScope(UiScope &&other) noexcept;            ///< Transfer scope on UI.
  UiScope &operator=(UiScope &&other) noexcept; ///< Close old scope, then transfer.
  UiScope(const UiScope &) = delete;
  UiScope &operator=(const UiScope &) = delete;
  std::expected<void, SchedulerError> close(); ///< UI-only, idempotent; no worker join.
private:
  friend class Scheduler;
  UiScope(std::shared_ptr<detail::SchedulerState> state, std::uint64_t id);
  std::shared_ptr<detail::SchedulerState> state_;
  std::uint64_t id_{};
};
/** @brief Copyable, worker-safe endpoint. It owns the closed channel state, never UI captures.
 * May outlive Scheduler: publish then returns closed. T must own its transitively immutable
 * data; mutable aliases must not modify it. Its deleter may run on UI or any producer thread.
 */
template <class T> class Publisher {
public:
  Publisher() = default; ///< Empty closed endpoint.
  /** @brief Transfer a strong immutable owner; null is invalid_argument, full is would_block.
   * No allocation occurs here. Failure changes no queue; the caller retains any other owner.
   */
  std::expected<PublishResult, SchedulerError> publish(std::shared_ptr<const T> value) const {
    return detail::scheduler_publish(state_, id_, std::move(value));
  }
  void wake() const noexcept {
    detail::scheduler_wake(state_);
  } ///< Wake a waiter without queueing work.
  void request_stop() const noexcept {
    detail::scheduler_request_stop(state_);
  } ///< Close all publications; UI reclaims callbacks.
private:
  friend class Scheduler;
  Publisher(std::shared_ptr<detail::SchedulerState> state, std::uint64_t id) : state_(std::move(state)), id_(id) {
  }
  std::shared_ptr<detail::SchedulerState> state_;
  std::uint64_t id_{};
};
/** @brief UI owns the subscription; workers receive copies of only the publisher. */
template <class T> struct SchedulerConnection {
  Subscription subscription; ///< Scoped callback owner.
  Publisher<T> publisher;    ///< Worker-safe publication endpoint.
};
/** @brief One UI executor; call dispatch only between frames, never during layout/paint.
 * create captures its calling thread. Callbacks run there without the queue lock. No method
 * mutates a widget or requests a frame: downstream callbacks own invalidation. Destruction
 * must occur on UI, outside dispatch, after UI tokens; publishers may survive it. Callbacks
 * must terminate promptly: budgets bound counts, not application callback wall time.
 */
class Scheduler {
public:
  /** @brief UI error sink; borrowed context must outlive Scheduler, sink must not throw. */
  using ErrorSink = void (*)(void *, SchedulerError) noexcept;
  /** @brief Allocate bounded storage; invalid limits/no memory return errors without partial state.
   * Allocation in caller expressions is outside this factory's boundary.
   */
  static std::expected<std::unique_ptr<Scheduler>, SchedulerError> create(SchedulerOptions options = {},
                                                                          SchedulerClock clock = {},
                                                                          ErrorSink sink = nullptr,
                                                                          void *sink_context = nullptr);
  ~Scheduler(); ///< Stop/reclaim on owner; no joins; destroying from a callback is prohibited.
  Scheduler(const Scheduler &) = delete;
  Scheduler &operator=(const Scheduler &) = delete;
  std::expected<UiScope, SchedulerError> make_scope(); ///< UI-only; closed/identity exhaustion are explicit.
  /** @brief UI-only typed registration; copies/moves and owns callback, translating bad_alloc.
   * Callback receives an independent snapshot owner. Foreign/closed scope, invalid enum,
   * registration capacity or allocation failure rejects the operation without registration.
   * replace_latest is data-only and keeps at most one pending value per sender.
   */
  template <class T, class Callback>
  std::expected<SchedulerConnection<T>, SchedulerError>
  connect(const UiScope &scope, Callback &&callback, SchedulerLane lane = SchedulerLane::data,
          SchedulerOverflow overflow = SchedulerOverflow::reject) {
    try {
      auto id = add(
        scope,
        [function = std::forward<Callback>(callback)](std::shared_ptr<const void> value) mutable {
          std::invoke(function, std::static_pointer_cast<const T>(std::move(value)));
        },
        lane, overflow, false, 0);

      if (!id) {
        return std::unexpected(id.error());
      }

      return SchedulerConnection<T>{Subscription(state_, *id), Publisher<T>(state_, *id)};
    } catch (const std::bad_alloc &) {
      return std::unexpected(SchedulerError::no_memory);
    }
  }
  /** @brief Register a cancellable one-shot timer at an absolute clock millisecond.
   * Due timers run in deadline/registration order, within timer_budget; no catch-up loss.
   */
  template <class Callback>
  std::expected<Subscription, SchedulerError> at(const UiScope &scope, SchedulerTime deadline, Callback &&callback) {
    try {
      auto id = add(
        scope,
        [function = std::forward<Callback>(callback)](std::shared_ptr<const void>) mutable {
          std::invoke(function);
        },
        SchedulerLane::data, SchedulerOverflow::reject, true, deadline);

      if (!id) {
        return std::unexpected(id.error());
      }

      return Subscription(state_, *id);
    } catch (const std::bad_alloc &) {
      return std::unexpected(SchedulerError::no_memory);
    }
  }
  std::expected<SchedulerDispatch, SchedulerError>
  dispatch(); ///< UI-only; nested dispatch rejected, callback exceptions contained.
  /** @brief UI-only bounded blocking wait; negative timeout invalid, dispatch reentry rejected.
   * Real timeout is independent of injected time. Advance fake time then wake explicitly.
   */
  std::expected<SchedulerWake, SchedulerError> wait_for(std::chrono::milliseconds timeout);
  void request_stop() noexcept;               ///< Worker-safe while this object lives; sender variant can outlive it.
  std::expected<void, SchedulerError> stop(); ///< UI-only close and reclamation, including self-stop.
  SchedulerMetrics metrics() const;           ///< Thread-safe copy, usable after stop.
private:
  explicit Scheduler(std::shared_ptr<detail::SchedulerState> state);
  std::expected<std::uint64_t, SchedulerError> add(const UiScope &, std::function<void(std::shared_ptr<const void>)>,
                                                   SchedulerLane, SchedulerOverflow, bool timer,
                                                   SchedulerTime deadline);
  std::shared_ptr<detail::SchedulerState> state_;
};
} // namespace portals
