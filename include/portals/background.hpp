// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/async.hpp>
#include <portals/background_options.hpp>

#include <stop_token>
#include <type_traits>

namespace portals {
class BackgroundWorkerStorage;

/** @brief Worker-safe work item; never retain Application, UI references, callbacks or coroutine frames.
 * Inputs, callable captures and deleters must be safe on any releasing worker. The owner must
 * impose domain byte limits separately. Cooperative cancellation cannot interrupt arbitrary blocking code.
 */
class BackgroundJob {
public:
  virtual ~BackgroundJob() = default; ///< Release worker-safe owning input and prepared result.
  /** @param stop This job's cooperative cancellation signal.
   * Run once outside the UI. Overrides must contain their exceptions and prepare a typed result.
   */
  virtual void run(std::stop_token stop) noexcept = 0;
  /** @return Success after publication; WOULD_BLOCK/NO_MEMORY retain the same prepared result for retry.
   * CLOSED/CANCELLED retire it. Called on a worker, never under the pool mutex.
   */
  virtual std::expected<void, AsyncError> publish() = 0;
  /** @brief Request cancellation without joining. Registered stop callbacks run synchronously and must be short. */
  void requestStop() noexcept;
  /** @return This item's cooperative cancellation signal, safe to copy across threads. */
  std::stop_token stopToken() const noexcept;

private:
  std::stop_source stop_;
};

/** @brief Move-only cancellation ownership; destruction requests cancellation but never joins. */
class BackgroundTicket {
public:
  BackgroundTicket() = default; ///< Empty inactive ticket.
  ~BackgroundTicket();          ///< Request cancellation; never wait for a worker.
  /** @param other Cancellation owner to transfer and empty. */
  BackgroundTicket(BackgroundTicket &&other) noexcept;
  /** @param other Cancellation owner replacing this one after cancellation.
   * @return This owner; self-assignment is a no-op.
   */
  BackgroundTicket &operator=(BackgroundTicket &&other) noexcept;
  BackgroundTicket(const BackgroundTicket &) = delete;
  BackgroundTicket &operator=(const BackgroundTicket &) = delete;

  /** @param job Accepted worker-safe owner, observed without prolonging its completed lifetime.
   * Host executors may construct the same cancellation owner after their own admission.
   */
  explicit BackgroundTicket(std::weak_ptr<BackgroundJob> job) : job_(std::move(job)) {
  }

  /** @brief Request cooperative cancellation; stop callbacks must return promptly.
   * This retires work without publishing a completion. Its UI owner must also cancel/retire the waiter.
   */
  void cancel() noexcept;

private:
  std::weak_ptr<BackgroundJob> job_;
};

/** @brief Borrowed host service. A scene never owns, stops or joins this executor.
 * UI integration calls service once per existing application cycle, after scheduler dispatch.
 * A ready host must keep scheduling steps while work is pending; wake only signals the existing
 * scheduler condition and does not interrupt native terminal reads or schedule a foreign event loop.
 */
class BackgroundExecutor {
public:
  virtual ~BackgroundExecutor() = default; ///< Concrete owning executors define their final join boundary.
  /** @param job Owning worker-safe item; preserved on any rejected admission.
   * @return Cancellation owner on acceptance, or explicit admission failure.
   */
  virtual std::expected<BackgroundTicket, BackgroundError> submit(std::unique_ptr<BackgroundJob> &job) = 0;
  /** @brief Permit one publication retry per blocked worker; does not wait or invoke UI callbacks. */
  virtual void service() noexcept = 0;
  /** @return Advisory capacity generation, sampled before submit to avoid a missed release race. */
  virtual std::uint64_t capacityEpoch() const noexcept = 0;
  /** @return Optional opaque worker-cache capability, borrowed for this executor's lifetime.
   * Unsupported hosts return nullptr; no synchronous UI-owned cache fallback is permitted.
   */
  virtual BackgroundWorkerStorage *workerStorage() noexcept { return nullptr; }
};

/** @brief Explicit fixed worker owner for a synchronous run or host-owned lifetime.
 * requestStop never joins; join is the explicit final synchronous boundary after UI retirement
 * and terminal close attempt. Destruction also joins as a final safety net, so this type must
 * never be owned by a scene, node, coroutine frame, or host callback.
 * Inputs and cancelled pending jobs can be released on the calling host thread at final retirement;
 * worker-safe deletion does not imply a bounded transitive payload destruction cost.
 */
class BackgroundPool final : public BackgroundExecutor {
public:
  /** @param options Positive worker and pending count bounds.
   * @param wake Optional worker-safe readiness endpoint; only its wake operation is used.
   * @return Fully started owner, or typed allocation/thread startup failure after partial workers stop.
   */
  static std::expected<std::unique_ptr<BackgroundPool>, BackgroundError> create(BackgroundOptions options = {},
                                                                                Publisher<int> wake = {});
  ~BackgroundPool() override;
  BackgroundPool(const BackgroundPool &) = delete;
  BackgroundPool &operator=(const BackgroundPool &) = delete;
  std::expected<BackgroundTicket, BackgroundError> submit(std::unique_ptr<BackgroundJob> &job) override;
  void service() noexcept override;
  std::uint64_t capacityEpoch() const noexcept override;
  /** @return Optional opaque worker-cache capability, borrowed for this executor's lifetime.
   * Unsupported hosts return nullptr; no synchronous UI-owned cache fallback is permitted.
   */
  BackgroundWorkerStorage *workerStorage() noexcept override;
  /** @brief Close admission, cancel owned jobs and wake workers independently of scheduler progress. */
  void requestStop() noexcept;
  /** @brief Stop and join owned workers; may wait for cooperative user code, never services UI.
   * Call from the owning synchronous host, never from one of this pool's workers.
   */
  void join() noexcept;

private:
  struct State;
  explicit BackgroundPool(std::unique_ptr<State> state);
  std::unique_ptr<State> state_;
};

/** @brief Whether the newest source intent was admitted or retained for a future capacity change. */
enum class BackgroundAdmission {
  STARTED, ///< No unadmitted intent remains; an accepted job may still be running.
  RETAINED ///< Exactly one latest unadmitted owning intent remains.
};

/** @brief UI-owned latest source; the executor and its host outlive this holder.
 * Existing scene integration services this holder after dispatch. Replacement cancels the previous
 * ticket and replaces only this source's rejected intent. No self-post or inline UI continuation occurs.
 * This bounds count to one unadmitted intent per source; callers enforce their input byte budgets.
 */
class BackgroundLatest {
public:
  /** @param executor Borrowed service, retained until this holder is destroyed. */
  explicit BackgroundLatest(BackgroundExecutor &executor) : executor_(executor) {
  }

  ~BackgroundLatest(); ///< Cancel this source's admitted work without stopping the borrowed executor.
  BackgroundLatest(const BackgroundLatest &) = delete;
  BackgroundLatest &operator=(const BackgroundLatest &) = delete;
  /** @param job Owning new intent; cancellation/replacement destroys only worker-safe captures.
   * @return STARTED, RETAINED on bounded admission refusal, or the exact terminal admission error.
   */
  std::expected<BackgroundAdmission, BackgroundError> replace(std::unique_ptr<BackgroundJob> job);
  /** @return Retry after capacity changed, or STARTED when no unadmitted intent remains. */
  std::expected<BackgroundAdmission, BackgroundError> service();
  /** @brief Cancel admitted work and release the latest unadmitted intent; no worker join. */
  void cancel() noexcept;

private:
  std::expected<BackgroundAdmission, BackgroundError> admit();
  BackgroundExecutor &executor_;
  BackgroundTicket ticket_;
  std::unique_ptr<BackgroundJob> pending_;
  std::uint64_t observed_{};
};

namespace detail {
/// @cond INTERNAL
template <class Input, class Output, class Work> class BackgroundTransform final : public BackgroundJob {
public:
  BackgroundTransform(std::shared_ptr<const Input> input, AsyncReply<Output> reply, Work work)
      : input_(std::move(input)), reply_(std::move(reply)), work_(std::move(work)) {
  }

  void run(std::stop_token stop) noexcept override {
    try {
      result_ = std::invoke(work_, *input_, stop);

      if (stop.stop_requested()) {
        result_ = std::unexpected(AsyncError::CANCELLED);
      } else if (result_ && !*result_) {
        result_ = std::unexpected(AsyncError::INVALID_ARGUMENT);
      }
    } catch (const std::bad_alloc &) {
      result_ = std::unexpected(AsyncError::NO_MEMORY);
    } catch (...) {
      result_ = std::unexpected(AsyncError::CALLBACK_FAILED);
    }
  }

  std::expected<void, AsyncError> publish() override {
    auto sent = result_ ? reply_.complete(*result_) : reply_.fail(result_.error());

    if (!sent) {
      return std::unexpected(sent.error());
    }

    return {};
  }

private:
  std::shared_ptr<const Input> input_;
  AsyncReply<Output> reply_;
  Work work_;
  std::expected<std::shared_ptr<const Output>, AsyncError> result_{std::unexpected(AsyncError::CANCELLED)};
};
/// @endcond
} // namespace detail

/** @brief Prepare owning immutable work without running it on the caller's UI thread.
 * @tparam Input Transitively immutable input with worker-safe ownership/deletion.
 * @tparam Output Transitively immutable result; may itself contain expected<DomainValue, DomainError>.
 * @tparam Work Nothrow-movable worker-safe callable accepting const Input& and stop_token, returning
 * shared_ptr&lt;const Output&gt; or expected&lt;shared_ptr&lt;const Output&gt;, AsyncError&gt;. Captures must not contain UI objects or coroutine storage.
 * @param input Non-null immutable input owner; mutable aliases must not change it.
 * @param reply Worker-safe endpoint registered by the existing UI awaiter before admission.
 * @param work Owning callable invoked once after admission; exceptions use existing async classifications.
 * @return Prepared item, INVALID_ARGUMENT for null input, or NO_MEMORY for adapter allocation.
 * Caller argument construction can throw before this boundary.
 */
template <class Input, class Output, class Work>
  requires std::is_nothrow_move_constructible_v<Work>
std::expected<std::unique_ptr<BackgroundJob>, BackgroundError> makeBackgroundJob(std::shared_ptr<const Input> input,
                                                                                 AsyncReply<Output> reply, Work work) {
  if (!input) {
    return std::unexpected(BackgroundError::INVALID_ARGUMENT);
  }

  try {
    return std::make_unique<detail::BackgroundTransform<Input, Output, Work>>(std::move(input), std::move(reply),
                                                                              std::move(work));
  } catch (const std::bad_alloc &) {
    return std::unexpected(BackgroundError::NO_MEMORY);
  }
}
} // namespace portals
