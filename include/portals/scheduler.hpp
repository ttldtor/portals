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
  invalid_argument,   ///< Invalid option, null snapshot or unsupported lane/overflow combination.
  wrong_thread,       ///< Operation requires the creating UI thread.
  wrong_scheduler,    ///< The supplied scope belongs to another scheduler.
  closed,             ///< Scheduler, scope, registration or endpoint is closed.
  would_block,        ///< A bounded queue or registration budget has no available slot.
  no_memory,          ///< Allocation failed inside the operation boundary.
  reentrant_dispatch, ///< Dispatch or blocking wait was called from active dispatch.
  identity_exhausted, ///< The monotonic identity counter cannot issue another ID.
  callback_failed     ///< A user callback threw; dispatch disconnects its registration.
};
/** @brief Independent finite queues; control is serviced before data each cycle. */
enum class SchedulerLane {
  data,   ///< Ordinary immutable data publications.
  control ///< Control input serviced before timers and data.
};
/** @brief Replace only this sender's pending display snapshot; never raw domain events. */
enum class SchedulerOverflow {
  reject,        ///< Reject publication when the lane has no free queue slot.
  replace_latest ///< Replace only this sender's pending data snapshot, preserving its FIFO position.
};
/** @brief Whether a publication appended a value or replaced its sender's pending value. */
enum class PublishResult {
  queued,  ///< A new queue entry was accepted.
  replaced ///< This sender's existing pending snapshot was replaced.
};
/** @brief A finite wait ended by work, explicit wake, stop or timeout. */
enum class SchedulerWake {
  ready,   ///< Work, a due timer or an explicit wake is available.
  stopped, ///< Stop was requested; no further publications are accepted.
  timeout  ///< The bounded real-time wait expired.
};
/** @brief Borrowed clock service, invoked under the scheduler mutex on UI or producer threads.
 * read must be thread-safe, nonblocking and non-reentrant. Context must outlive Scheduler;
 * stopped publishers never read it. Null read selects the steady clock.
 */
struct SchedulerClock {
  /** @brief Read monotonic milliseconds from the borrowed context; cannot throw or reenter the scheduler. */
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
  /** @brief Empty disconnected token. */
  Subscription() = default;
  /** @brief Disconnect on UI; never joins a worker. */
  ~Subscription();
  /** @brief Transfer ownership on UI.
   * @param other UI-owned token to empty and transfer into this token.
   */
  Subscription(Subscription &&other) noexcept;
  /** @brief Disconnect old token, then transfer.
   * @param other UI-owned token to empty and transfer; self-assignment is a no-op.
   * @return This token after replacing its previous ownership.
   */
  Subscription &operator=(Subscription &&other) noexcept;
  Subscription(const Subscription &) = delete;
  Subscription &operator=(const Subscription &) = delete;
  /** @brief UI-only, idempotent; wrong_thread leaves unchanged.
   * @return Success for an already empty/disconnected token; wrong_thread leaves a live token unchanged.
   */
  std::expected<void, SchedulerError> disconnect();

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
  /** @brief Empty closed scope. */
  UiScope() = default;
  /** @brief Close on UI; wrong-thread destruction terminates. */
  ~UiScope();
  /** @brief Transfer scope on UI.
   * @param other UI-owned token to empty and transfer into this token.
   */
  UiScope(UiScope &&other) noexcept;
  /** @brief Close old scope, then transfer.
   * @param other UI-owned token to empty and transfer; self-assignment is a no-op.
   * @return This token after replacing its previous ownership.
   */
  UiScope &operator=(UiScope &&other) noexcept;
  UiScope(const UiScope &) = delete;
  UiScope &operator=(const UiScope &) = delete;
  /** @brief UI-only, idempotent; no worker join.
   * @return Success after logical closure, including an empty scope; wrong_thread leaves a live scope unchanged.
   */
  std::expected<void, SchedulerError> close();

private:
  friend class Scheduler;
  UiScope(std::shared_ptr<detail::SchedulerState> state, std::uint64_t id);
  std::shared_ptr<detail::SchedulerState> state_;
  std::uint64_t id_{};
};
/** @brief Copyable, worker-safe endpoint. It owns the closed channel state, never UI captures.
 * May outlive Scheduler: publish then returns closed. T must own its transitively immutable
 * data; mutable aliases must not modify it. Its deleter may run on UI or any producer thread.
 * @tparam T Transitively immutable payload type owned by shared_ptr<const T>; its deleter must support any releasing
 * thread.
 */
template <class T> class Publisher {
public:
  /** @brief Empty closed endpoint. */
  Publisher() = default;
  /** @brief Transfer a strong immutable owner; null is invalid_argument, full is would_block.
   * No allocation occurs here. Failure changes no queue; the caller retains any other owner.
   * @param value Non-null immutable snapshot owner transferred into the bounded queue.
   * @return queued or replaced on acceptance; closed for an unavailable endpoint, invalid_argument for null,
   * or would_block when no queue position can be used.
   */
  std::expected<PublishResult, SchedulerError> publish(std::shared_ptr<const T> value) const {
    return detail::scheduler_publish(state_, id_, std::move(value));
  }
  /** @brief Wake a waiter without queueing work. */
  void wake() const noexcept {
    detail::scheduler_wake(state_);
  }
  /** @brief Close all publications; UI reclaims callbacks. */
  void request_stop() const noexcept {
    detail::scheduler_request_stop(state_);
  }

private:
  friend class Scheduler;
  Publisher(std::shared_ptr<detail::SchedulerState> state, std::uint64_t id) : state_(std::move(state)), id_(id) {
  }
  std::shared_ptr<detail::SchedulerState> state_;
  std::uint64_t id_{};
};
/** @brief UI owns the subscription; workers receive copies of only the publisher.
 * @tparam T Transitively immutable snapshot type shared by the publisher and its callback.
 */
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
   * @param options Positive queue, registration and dispatch limits; storage is reserved up front.
   * @param clock Borrowed monotonic clock; a null reader selects steady_clock.
   * @param sink Optional nonthrowing callback-failure sink, invoked on the creating UI thread.
   * @param sink_context Borrowed sink state that must outlive this scheduler.
   * @return New UI-owned scheduler, invalid_argument for invalid/unrepresentable limits, or no_memory.
   */
  static std::expected<std::unique_ptr<Scheduler>, SchedulerError> create(SchedulerOptions options = {},
                                                                          SchedulerClock clock = {},
                                                                          ErrorSink sink = nullptr,
                                                                          void *sink_context = nullptr);
  /** @brief Stop/reclaim on owner; no joins; destroying from a callback is prohibited. */
  ~Scheduler();
  Scheduler(const Scheduler &) = delete;
  Scheduler &operator=(const Scheduler &) = delete;
  /** @brief UI-only; closed/identity exhaustion are explicit.
   * @return New UI-owned scope, or wrong_thread, closed, or identity_exhausted.
   */
  std::expected<UiScope, SchedulerError> make_scope();
  /** @brief UI-only typed registration; copies/moves and owns callback, translating bad_alloc.
   * Callback receives an independent snapshot owner. Foreign/closed scope, invalid enum,
   * registration capacity or allocation failure rejects the operation without registration.
   * replace_latest is data-only and keeps at most one pending value per sender.
   * @tparam T Transitively immutable payload type; final ownership may be released on UI or a producer.
   * @tparam Callback Copy-constructible callable storable in std::function and invocable as an lvalue
   * with std::shared_ptr<const T>; its return value is ignored.
   * @param scope Live scope belonging to this scheduler; it controls callback lifetime.
   * @param callback Callable copied/moved into UI-owned registration storage.
   * @param lane Queue whose capacity and dispatch budget apply.
   * @param overflow Reject on capacity or replace this sender's pending data snapshot.
   * @return Subscription and publisher, or wrong_thread, wrong_scheduler, closed, invalid_argument,
   * would_block, identity_exhausted, or no_memory.
   * @note Exceptions other than std::bad_alloc propagate when materializing the callback.
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
   * @tparam Callback Copy-constructible callable storable in std::function and invocable as an lvalue
   * without arguments; its return value is ignored.
   * @param scope Live scope belonging to this scheduler; closing it cancels the timer.
   * @param deadline Absolute millisecond deadline in the configured monotonic clock.
   * @param callback Callable copied/moved into UI-owned registration storage.
   * @return Cancellation token, or wrong_thread, wrong_scheduler, closed, would_block,
   * identity_exhausted, or no_memory.
   * @note Exceptions other than std::bad_alloc propagate when materializing the callback.
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
  /** @brief UI-only; nested dispatch rejected, callback exceptions contained.
   * @return Counts for this control/timer/data cycle, including caught callback failures and stop;
   * wrong_thread or reentrant_dispatch rejects before any callback starts.
   */
  std::expected<SchedulerDispatch, SchedulerError> dispatch();
  /** @brief UI-only bounded blocking wait; negative timeout invalid, dispatch reentry rejected.
   * Real timeout is independent of injected time. Advance fake time then wake explicitly.
   * @param timeout Nonnegative maximum real elapsed wait; zero only checks readiness.
   * @return ready for queued/due work or explicit wake, stopped after stop, timeout after the wait,
   * or wrong_thread, reentrant_dispatch, or invalid_argument on rejection.
   */
  std::expected<SchedulerWake, SchedulerError> wait_for(std::chrono::milliseconds timeout);
  /** @brief Worker-safe while this object lives; sender variant can outlive it. */
  void request_stop() noexcept;
  /** @brief UI-only close and reclamation, including self-stop.
   * @return Success after closure and UI reclamation, including repeated stop; wrong_thread otherwise.
   */
  std::expected<void, SchedulerError> stop();
  /** @brief Thread-safe copy, usable after stop.
   * @return Owning snapshot of both queue counters and aggregate dispatch counters.
   */
  SchedulerMetrics metrics() const;

private:
  explicit Scheduler(std::shared_ptr<detail::SchedulerState> state);
  std::expected<std::uint64_t, SchedulerError> add(const UiScope &, std::function<void(std::shared_ptr<const void>)>,
                                                   SchedulerLane, SchedulerOverflow, bool timer,
                                                   SchedulerTime deadline);
  std::shared_ptr<detail::SchedulerState> state_;
};
} // namespace portals
