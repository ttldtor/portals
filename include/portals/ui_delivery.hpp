// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/widget.hpp>

#include <atomic>
#include <optional>

namespace portals {
namespace ui {
template <class T> class Ref;
}

/** @brief Execution outcome, independent of successful enqueue admission. */
enum class UiDeliveryState {
  PENDING,   ///< Accepted, but the UI callback has not returned yet.
  APPLIED,   ///< The UI callback returned successfully; external effects are not transactional.
  CANCELLED, ///< Scope, subscription or scheduler closed before callback admission.
  FAILED     ///< The UI callback returned a typed error or threw an exception.
};

/** @brief Owning nonblocking observation of one admitted delivery. */
struct UiDeliveryOutcome {
  UiDeliveryState state{UiDeliveryState::CANCELLED}; ///< Empty acknowledgements observe CANCELLED.
  std::optional<TreeError> error;                    ///< Original callback error, present only for FAILED.
};

class UiAcknowledgement;

namespace detail {
/// @cond INTERNAL
struct UiDeliveryAcknowledgement {
  std::atomic<UiDeliveryState> state{UiDeliveryState::PENDING};
  std::atomic<TreeError> error{};
};

struct UiDeliveryEnvelope {
  std::shared_ptr<const void> payload;
  std::shared_ptr<UiDeliveryAcknowledgement> acknowledgement;

  ~UiDeliveryEnvelope();
  void complete(std::expected<void, TreeError> result) const noexcept;
};
/// @endcond

std::expected<UiAcknowledgement, SchedulerError> postUiDelivery(const Publisher<UiDeliveryEnvelope> &publisher,
                                                                std::shared_ptr<const void> payload, bool acknowledge);
} // namespace detail

/** @brief Copyable execution receipt; polling never waits and retains no UI owner or callback. */
class UiAcknowledgement {
public:
  /** @brief Construct an empty receipt observing CANCELLED. */
  UiAcknowledgement() = default;
  /** @brief Thread-safe acquire observation; no callback or UI operation is performed.
   * @return PENDING until callback return, then the terminal execution outcome.
   */
  UiDeliveryOutcome poll() const noexcept;

private:
  template <class> friend class UiEndpoint;
  friend std::expected<UiAcknowledgement, SchedulerError>
  detail::postUiDelivery(const Publisher<detail::UiDeliveryEnvelope> &, std::shared_ptr<const void>, bool);
  explicit UiAcknowledgement(std::shared_ptr<const detail::UiDeliveryAcknowledgement> value);
  std::shared_ptr<const detail::UiDeliveryAcknowledgement> state_;
};

/** @brief Copyable worker-safe endpoint for short, always deferred UI commands.
 * The callback and its captures stay exclusively in the UI-owned Scheduler registration.
 * Uses the existing shared CONTROL queue with REJECT; commands are never coalesced.
 * Queue defaults and budgets come from SchedulerOptions (32 entries/32 per dispatch).
 * May outlive its node, scene and scheduler; later publication returns CLOSED.
 * @tparam T Transitively immutable payload owning all referenced data. Final destruction may occur
 * on UI or a producer; payload deleters must support either and can affect UI dispatch latency.
 */
template <class T> class UiEndpoint {
public:
  /** @brief Construct an empty closed endpoint. */
  UiEndpoint() = default;

  /** @brief Enqueue without observing execution; never invokes the UI callback inline.
   * @param payload Immutable owning value; null is INVALID_ARGUMENT. One envelope is allocated.
   * @return Enqueue acceptance only, or CLOSED, WOULD_BLOCK, NO_MEMORY or INVALID_ARGUMENT.
   * Rejection does not change accepted commands or retry automatically.
   */
  std::expected<void, SchedulerError> post(std::shared_ptr<const T> payload) const {
    auto result = detail::postUiDelivery(publisher_, std::move(payload), false);
    if (!result) {
      return std::unexpected(result.error());
    }

    return {};
  }

  /** @brief Enqueue with a separate nonblocking execution receipt.
   * @param payload Immutable owning value; null is INVALID_ARGUMENT. Allocates envelope and receipt.
   * @return Receipt on acceptance, or exact enqueue error; acceptance never promises execution.
   */
  std::expected<UiAcknowledgement, SchedulerError> postAcknowledged(std::shared_ptr<const T> payload) const {
    return detail::postUiDelivery(publisher_, std::move(payload), true);
  }

private:
  template <class> friend class ui::Ref;

  explicit UiEndpoint(Publisher<detail::UiDeliveryEnvelope> publisher) : publisher_(std::move(publisher)) {
  }

  Publisher<detail::UiDeliveryEnvelope> publisher_;
};
} // namespace portals
