// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/widget.hpp>

#include <atomic>
#include <concepts>
#include <coroutine>
#include <exception>
#include <functional>
#include <limits>
#include <optional>
#include <thread>

namespace portals {
namespace ui::detail {
struct MessageAccess;
struct AsyncAccess;
} // namespace ui::detail
class AsyncTimer;
template <class T> class AsyncReceive;

/** @brief Failures of the optional single-frame UI coroutine adapter. */
enum class AsyncError {
  INVALID_ARGUMENT,  ///< Empty task, callback or immutable data owner, or invalid policy.
  WRONG_THREAD,      ///< The operation or coroutine frame belongs to another thread.
  CLOSED,            ///< Owner, node scope, scheduler or completion endpoint is closed.
  CANCELLED,         ///< Explicit cancellation or replacement stopped the invocation.
  WOULD_BLOCK,       ///< Active resume, invocation, registration or queue capacity rejects the operation.
  NO_MEMORY,         ///< Allocation failed inside an adapter or coroutine boundary.
  CALLBACK_FAILED,   ///< Provider or coroutine body threw an exception other than bad_alloc.
  IDENTITY_EXHAUSTED ///< Invocation identity cannot advance without reuse.
};
/** @brief Explicit policy for repeated actions on one owner. */
enum class UiTaskRestart {
  REJECT_RUNNING, ///< Default: preserve the active invocation and reject the new task.
  CANCEL_PREVIOUS ///< Cancel a suspended invocation before adopting the new task; active resume still rejects.
};
/** @brief Retained business outcome, independent of scheduler queue acceptance. */
enum class UiTaskStatus {
  IDLE,      ///< No invocation has been adopted.
  RUNNING,   ///< One frame is executing or waiting.
  SUCCEEDED, ///< The coroutine returned success.
  CANCELLED, ///< Explicit cancellation, replacement, node removal or owner closure stopped the frame.
  FAILED     ///< The coroutine returned an error or threw.
};

/** @brief UI-readable outcome remains available after context removal. */
struct UiTaskOutcome {
  UiTaskStatus status{UiTaskStatus::IDLE}; ///< Current or most recently completed invocation.
  std::optional<AsyncError> error;         ///< Empty for idle, running or successful completion.
  std::uint64_t generation{};              ///< Monotonic adopted invocation identity; zero before the first task.
};

namespace detail {
struct AsyncControl;
struct AsyncWait;

/// @cond INTERNAL
// An optional UI-only fluent owner. Retirement releases this only after frame.destroy().
struct AsyncHooks {
  std::shared_ptr<void> owner;
  bool (*resume)(void *, std::coroutine_handle<>) noexcept {};
  void (*cancel)(void *) noexcept {};
  void (*complete)(void *, const UiTaskOutcome &) noexcept {};
};
/// @endcond

/// @cond INTERNAL
struct AsyncInvocation {
  std::weak_ptr<AsyncControl> control;
  std::uint64_t generation{};
  std::weak_ptr<AsyncWait> wait;
  Publisher<int> wake;
};

AsyncInvocation asyncInvocation(const std::weak_ptr<AsyncControl> &control) noexcept;
bool asyncInvocationCurrent(const AsyncInvocation &identity) noexcept;
void asyncCancelInvocation(const AsyncInvocation &identity) noexcept;

// Optional private adapter gate; ordinary receives have no additional admission condition.
struct AsyncReceiveAdmission {
  std::weak_ptr<const void> lifetime;
  std::weak_ptr<const void> request;
  const void *context{};
  bool (*allows)(const void *, const std::shared_ptr<const void> &) noexcept {};
};

/// @endcond

/** @brief Exact adapter awaiter classification; foreign resume contracts are rejected.
 * @tparam T Unqualified candidate awaiter type.
 */
template <class T> inline constexpr bool IS_ASYNC_AWAITER = false;
template <class T> inline constexpr bool IS_ASYNC_AWAITER<AsyncReceive<T>> = true;
template <> inline constexpr bool IS_ASYNC_AWAITER<AsyncTimer> = true;

/** @brief UI-owned wait registration; workers never retain this cancellation interface. */
struct AsyncWait {
  /** @brief Destroy on UI after registration detachment. */
  virtual ~AsyncWait() = default;
  /** @param error Reason stored when a not-yet-settled wait is cancelled on UI. */
  virtual void cancel(AsyncError error) noexcept = 0;
};

AsyncError asyncError(SchedulerError error) noexcept;
AsyncError asyncError(TreeError error) noexcept;
std::expected<void, AsyncError> asyncCheck(const std::shared_ptr<AsyncControl> &state);
std::expected<void, AsyncError> asyncInstallWait(const std::shared_ptr<AsyncControl> &state, std::uint64_t generation,
                                                 std::shared_ptr<AsyncWait> wait);
void asyncResume(const std::shared_ptr<AsyncControl> &state, std::uint64_t generation);
void asyncFinishWait(const std::shared_ptr<AsyncControl> &state, const AsyncWait *wait) noexcept;
Scheduler &asyncScheduler(const std::shared_ptr<AsyncControl> &state) noexcept;
WidgetTree &asyncTree(const std::shared_ptr<AsyncControl> &state) noexcept;
Node<Widget> asyncNode(const std::shared_ptr<AsyncControl> &state) noexcept;

/** One endpoint has at most one accepted immutable completion. UI closure is visible to workers. */
struct AsyncAdmission {
  static constexpr unsigned OPEN = 0;       ///< A producer can attempt one publication.
  static constexpr unsigned PUBLISHING = 1; ///< One producer holds the admission attempt.
  static constexpr unsigned ACCEPTED = 2;   ///< One completion was accepted; further admission rejects.
  static constexpr unsigned CLOSED = 3;     ///< UI cancellation or delivery permanently closes admission.
  std::atomic<unsigned> phase{OPEN};        ///< Only shared mutable producer state; never retains UI storage.
};

/** @brief Single immutable scheduler payload carrying the owning await result.
 * @tparam T Transitively immutable domain value with a producer-safe deleter.
 */
template <class T> struct AsyncCompletion {
  std::expected<std::shared_ptr<const T>, AsyncError> result; ///< Owning data or exact provider error.
};
} // namespace detail

/** @brief Lazy move-only coroutine frame, created and released on its creating UI thread.
 * The task owns its function parameters, never a captured coroutine lambda's closure. Direct
 * low-level use requires a free/static coroutine function with owning parameters; ui::async
 * separately retains its callable until the entire frame is destroyed. Allocation in the calling expression
 * can throw before start(); no work runs until successful adoption. Only this adapter's awaiters
 * are supported. A suspended task must not retain widget/view borrows.
 */
class UiTask {
public:
  /** @brief Compiler-facing coroutine promise; not an independent execution API. */
  struct promise_type {
    const std::thread::id owner = std::this_thread::get_id(); ///< Frame creation affinity.
    std::weak_ptr<detail::AsyncControl> control;              ///< Observes the adopting UI owner.
    std::uint64_t generation{};                               ///< Invocation to which this frame belongs.
    std::expected<void, AsyncError> result;                   ///< Returned or exception-translated outcome.
    /** @return Lazy frame owner. */
    UiTask get_return_object() noexcept;

    /** @return Suspension before any coroutine body code executes. */
    constexpr std::suspend_always initial_suspend() const noexcept {
      return {};
    }

    /** @return Suspension retaining the frame until its UI owner destroys it. */
    constexpr std::suspend_always final_suspend() const noexcept {
      return {};
    }

    /** @param value Typed business outcome retained before final suspension. */
    void return_value(std::expected<void, AsyncError> value) noexcept {
      result = value;
    }

    /** @brief Translate body exceptions to NO_MEMORY or CALLBACK_FAILED. */
    void unhandled_exception() noexcept;

    /** @brief Permit only waits whose resume and destruction belong to this UI adapter.
     * @tparam Awaiter Exactly AsyncTimer or AsyncReceive<T>, with optional reference/cv qualification.
     * @param awaiter Timer or typed data wait returned by UiAsyncContext.
     * @return The same owning awaiter, forwarded into coroutine storage.
     */
    template <class Awaiter>
      requires detail::IS_ASYNC_AWAITER<std::remove_cvref_t<Awaiter>>
    Awaiter &&await_transform(Awaiter &&awaiter) noexcept {
      return std::forward<Awaiter>(awaiter);
    }
  };

  /** @brief Empty task. */
  UiTask() = default;
  /** @brief Destroy an unadopted frame on its creating thread; foreign destruction terminates. */
  ~UiTask();
  /** @param other Lazy owner to transfer; other becomes empty. */
  UiTask(UiTask &&other) noexcept;
  /** @param other Lazy owner to transfer after releasing this frame on its creating thread.
   * @return This task; self-assignment is a no-op.
   */
  UiTask &operator=(UiTask &&other) noexcept;
  UiTask(const UiTask &) = delete;
  UiTask &operator=(const UiTask &) = delete;

private:
  friend class UiAsync;
  using Handle = std::coroutine_handle<promise_type>;

  explicit UiTask(Handle frame) : frame_(frame) {
  }

  Handle frame_{};
};

/** @brief Copyable producer endpoint owning no UI state, callbacks or coroutine frame.
 * T and its deleter must support transitively immutable shared ownership on any releasing
 * thread. One accepted completion ends admission; rejected publication leaves retry possible.
 * The producer must retain the reply and result on WOULD_BLOCK and retry when capacity returns.
 * Synchronous completion queues a value; it never resumes a coroutine inline.
 * @tparam T Immutable owning result type; raw widget/view references are unsuitable.
 */
template <class T> class AsyncReply {
public:
  /** @brief Empty closed endpoint. */
  AsyncReply() = default;

  /** @param value Non-null immutable owner; caller may retain another owner for retry.
   * @return QUEUED on admission, CLOSED after cancellation/delivery, WOULD_BLOCK for a full
   * queue or duplicate accepted completion, INVALID_ARGUMENT for null, or NO_MEMORY.
   */
  std::expected<PublishResult, AsyncError> complete(std::shared_ptr<const T> value) const {
    if (!value) {
      return std::unexpected(AsyncError::INVALID_ARGUMENT);
    }

    return publish(std::move(value));
  }

  /** @param error Typed provider failure delivered as the await result.
   * @return Same queue/admission result as complete(); failed admission remains retryable.
   */
  std::expected<PublishResult, AsyncError> fail(AsyncError error) const {
    return publish(std::unexpected(error));
  }

private:
  template <class> friend class AsyncReceive;

  AsyncReply(Publisher<detail::AsyncCompletion<T>> publisher, std::shared_ptr<detail::AsyncAdmission> admission)
      : publisher_(std::move(publisher)), admission_(std::move(admission)) {
  }

  std::expected<PublishResult, AsyncError> publish(std::expected<std::shared_ptr<const T>, AsyncError> result) const {
    // Rejected sole payload release may reset or destroy this endpoint. Retain both handles
    // before publication, and never read members after an arbitrary payload deleter can run.
    auto admission = admission_;
    auto publisher = publisher_;

    if (!admission) {
      return std::unexpected(AsyncError::CLOSED);
    }

    auto phase = admission->phase.load();

    if (phase != detail::AsyncAdmission::OPEN) {
      return std::unexpected(phase == detail::AsyncAdmission::CLOSED ? AsyncError::CLOSED : AsyncError::WOULD_BLOCK);
    }

    try {
      auto event = std::make_shared<const detail::AsyncCompletion<T>>(std::move(result));
      phase = detail::AsyncAdmission::OPEN;

      if (!admission->phase.compare_exchange_strong(phase, detail::AsyncAdmission::PUBLISHING)) {
        return std::unexpected(phase == detail::AsyncAdmission::CLOSED ? AsyncError::CLOSED : AsyncError::WOULD_BLOCK);
      }

      auto sent = publisher.publish(std::move(event));
      phase = detail::AsyncAdmission::PUBLISHING;
      admission->phase.compare_exchange_strong(phase,
                                               sent ? detail::AsyncAdmission::ACCEPTED : detail::AsyncAdmission::OPEN);

      if (!sent) {
        return std::unexpected(detail::asyncError(sent.error()));
      }

      return *sent;
    } catch (const std::bad_alloc &) {
      return std::unexpected(AsyncError::NO_MEMORY);
    }
  }

  Publisher<detail::AsyncCompletion<T>> publisher_;
  std::shared_ptr<detail::AsyncAdmission> admission_;
};

/** @brief Owning typed data awaiter; normally obtained through UiAsyncContext::receive.
 * @tparam T Transitively immutable shared result type, safe to release on producer or UI threads.
 */
template <class T> class AsyncReceive {
  struct Wait final : detail::AsyncWait {
    Subscription subscription;
    std::shared_ptr<detail::AsyncAdmission> admission = std::make_shared<detail::AsyncAdmission>();
    std::expected<std::shared_ptr<const T>, AsyncError> result = std::unexpected(AsyncError::CANCELLED);
    bool settled{};

    void cancel(AsyncError error) noexcept override {
      if (!settled) {
        result = std::unexpected(error);
        settled = true;
      }

      admission->phase.store(detail::AsyncAdmission::CLOSED);
      (void)subscription.disconnect();
    }
  };

public:
  /** @return False: all completions enter through the existing scheduler. */
  constexpr bool await_ready() const noexcept {
    return false;
  }

  /** @param frame Currently executing adopted UI coroutine.
   * @return True when suspended; false when registration/start failed or cancellation occurred.
   */
  bool await_suspend(std::coroutine_handle<UiTask::promise_type> frame) {
    auto state = control_.lock();
    auto checked = detail::asyncCheck(state);

    if (!checked) {
      fallback_ = checked.error();
      return false;
    }

    if (frame.promise().control.lock() != state) {
      fallback_ = AsyncError::INVALID_ARGUMENT;
      return false;
    }

    if (!start_) {
      fallback_ = AsyncError::INVALID_ARGUMENT;
      return false;
    }

    try {
      wait_ = std::make_shared<Wait>();
      auto installed = detail::asyncInstallWait(state, frame.promise().generation, wait_);

      if (!installed) {
        wait_->cancel(installed.error());
        return false;
      }

      std::expected<SchedulerConnection<detail::AsyncCompletion<T>>, SchedulerError> connection =
        std::unexpected(SchedulerError::CLOSED);
      auto allowed = detail::asyncTree(state).withScope(detail::asyncNode(state), [&](const UiScope &scope) {
        connection = detail::asyncScheduler(state).template connect<detail::AsyncCompletion<T>>(
          scope, [weak = std::weak_ptr(wait_), control = std::weak_ptr(state), generation = frame.promise().generation,
                  admission = admission_](std::shared_ptr<const detail::AsyncCompletion<T>> event) {
            auto wait = weak.lock();
            auto retained = control.lock();

            if (!wait || !retained || wait->settled) {
              return;
            }

            if (admission.allows) {
              const auto request = admission.request.lock();
              // UI-affine, callback-free inspection must not prolong a raw context's lifetime token
              // across frame destruction: the cancelled frame may itself destroy that context.
              if (admission.lifetime.expired() || !request || !admission.allows(admission.context, request)) {
                detail::asyncCancelInvocation({retained, generation, wait, {}});
                return;
              }
            }

            wait->result = event->result;
            wait->settled = true;
            wait->cancel(AsyncError::CANCELLED);
            detail::asyncResume(retained, generation);
          });
      });

      if (!allowed || !connection) {
        wait_->cancel(!allowed ? detail::asyncError(allowed.error()) : detail::asyncError(connection.error()));

        return false;
      }

      wait_->subscription = std::move(connection->subscription);
      start_(AsyncReply<T>(std::move(connection->publisher), wait_->admission));
      checked = detail::asyncCheck(state);

      if (!checked) {
        wait_->cancel(checked.error());
      }

      return !wait_->settled;
    } catch (const std::bad_alloc &) {
      fallback_ = AsyncError::NO_MEMORY;
    } catch (...) {
      fallback_ = AsyncError::CALLBACK_FAILED;
    }

    if (wait_) {
      wait_->cancel(fallback_);
    }

    return false;
  }

  /** @return Owning immutable result or typed registration/provider/cancellation failure. */
  std::expected<std::shared_ptr<const T>, AsyncError> await_resume() noexcept {
    if (auto state = control_.lock()) {
      detail::asyncFinishWait(state, wait_.get());
    }

    return wait_ ? wait_->result : std::expected<std::shared_ptr<const T>, AsyncError>(std::unexpected(fallback_));
  }

private:
  friend class UiAsyncContext;
  friend struct ui::detail::MessageAccess;

  AsyncReceive(std::weak_ptr<detail::AsyncControl> control, std::function<void(AsyncReply<T>)> start)
      : control_(std::move(control)), start_(std::move(start)) {
  }

  std::weak_ptr<detail::AsyncControl> control_;
  std::function<void(AsyncReply<T>)> start_;
  std::shared_ptr<Wait> wait_;
  detail::AsyncReceiveAdmission admission_;
  AsyncError fallback_{AsyncError::CLOSED};
};

/** @brief One-shot absolute millisecond wait on the existing scheduler. */
class AsyncTimer {
public:
  /** @return False, including already-due deadlines: dispatch owns timer delivery. */
  constexpr bool await_ready() const noexcept {
    return false;
  }

  /** @param frame Adopted UI coroutine to suspend.
   * @return True when registered, false on typed rejection.
   */
  bool await_suspend(std::coroutine_handle<UiTask::promise_type> frame);
  /** @return Success after the due callback or typed registration/cancellation failure. */
  std::expected<void, AsyncError> await_resume() noexcept;

private:
  friend class UiAsyncContext;
  friend struct ui::detail::AsyncAccess;
  struct Wait;

  AsyncTimer(std::weak_ptr<detail::AsyncControl> control, SchedulerTime deadline)
      : control_(std::move(control)), deadline_(deadline) {
  }

  explicit AsyncTimer(AsyncError error) : fallback_(error), rejected_(true) {
  }

  std::weak_ptr<detail::AsyncControl> control_;
  std::shared_ptr<Wait> wait_;
  SchedulerTime deadline_{};
  AsyncError fallback_{AsyncError::CLOSED};
  bool rejected_{};
};

/** @brief Weak context copied into owning coroutine parameters; never extends the UI owner's life. */
class UiAsyncContext {
public:
  /** @brief Empty closed context. */
  UiAsyncContext() = default;

  /** @param deadline Absolute monotonic milliseconds in this owner's SchedulerClock; no extra rounding.
   * @return Owning one-shot awaiter; even past deadlines wait for dispatch and its timer budget.
   */
  AsyncTimer until(SchedulerTime deadline) const {
    return AsyncTimer(control_, deadline);
  }

  /** @brief Register one completion before calling the owning UI provider-start callback.
   * @tparam T Transitively immutable owning domain result type.
   * @param start UI callback receiving only a worker-safe reply; must retain rejected results for retry.
   * Do not dispatch recursively; capture materialization may throw in the caller expression.
   * @return Owning data awaiter; provider exceptions become typed await failures.
   */
  template <class T> AsyncReceive<T> receive(std::function<void(AsyncReply<T>)> start) const {
    return AsyncReceive<T>(control_, std::move(start));
  }

private:
  friend class UiAsync;
  friend struct ui::detail::MessageAccess;

  explicit UiAsyncContext(std::weak_ptr<detail::AsyncControl> control) : control_(std::move(control)) {
  }

  std::weak_ptr<detail::AsyncControl> control_;
};

/** @brief Optional UI owner of at most one coroutine frame and one wait, with no separate runtime.
 * Uses one permanent node-scope registration and at most one timer/data registration. Existing
 * Scheduler capacities and dispatch budgets apply. Tree and scheduler must outlive this owner.
 * Destruction is UI-only, does not join workers, and defers a currently executing frame's release
 * until resume returns. No method requests redraw; coroutine application effects invalidate explicitly.
 */
class UiAsync {
public:
  /** @param scheduler Existing UI scheduler; must own the node scope and outlive this owner.
   * @param tree Existing UI tree; must outlive the owner.
   * @param context Live node whose removal cancels the frame and closes this owner.
   * @return UI owner, or typed context/thread/frame/registration/allocation failure.
   */
  static std::expected<std::unique_ptr<UiAsync>, AsyncError> create(Scheduler &scheduler, WidgetTree &tree,
                                                                    Node<Widget> context);
  /** @brief Close on UI; foreign-thread destruction terminates. */
  ~UiAsync();
  UiAsync(const UiAsync &) = delete;
  UiAsync &operator=(const UiAsync &) = delete;
  /** @return Weak context for owning parameters of a free/static coroutine function; foreign access
   * returns an empty context without reading mutable UI state.
   */
  UiAsyncContext context() const;
  /** @brief Adopt and initially resume on UI; subsequent waits resume through scheduler dispatch.
   * @param task Lazy task created on this UI thread; consumed only on successful adoption.
   * @param restart Explicit repeated-action policy, defaulting to preserving running work.
   * @return Admission success, or typed rejection. Self-close/removal during first resume returns
   * CLOSED; business success/failure is separately retained by outcome(). Rejection before adoption preserves task;
   * first-resume failure has consumed it.
   */
  std::expected<void, AsyncError> start(UiTask &&task, UiTaskRestart restart = UiTaskRestart::REJECT_RUNNING);
  /** @brief Cancel on UI; suspended frame is released now, executing frame after resume returns.
   * @return Success including idle/completed owners, or WRONG_THREAD. Outcome remains inspectable.
   */
  std::expected<void, AsyncError> cancel();
  /** @brief Permanently close on UI without joining; idempotent.
   * @return Success or WRONG_THREAD; outcome remains readable after closure/removal.
   */
  std::expected<void, AsyncError> close();
  /** @return Retained invocation outcome, including after node removal; WRONG_THREAD on foreign access. */
  std::expected<UiTaskOutcome, AsyncError> outcome() const;

private:
  friend struct ui::detail::AsyncAccess;

  std::expected<void, AsyncError> startOwned(UiTask &&task, UiTaskRestart restart, detail::AsyncHooks hooks);

  explicit UiAsync(std::shared_ptr<detail::AsyncControl> state) : state_(std::move(state)) {
  }

  std::shared_ptr<detail::AsyncControl> state_;
};
} // namespace portals
