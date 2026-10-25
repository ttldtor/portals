// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_delivery.hpp>

namespace portals {
detail::UiDeliveryEnvelope::~UiDeliveryEnvelope() {
  if (acknowledgement) {
    auto pending = UiDeliveryState::PENDING;
    acknowledgement->state.compare_exchange_strong(pending, UiDeliveryState::CANCELLED, std::memory_order_release);
  }
}

void detail::UiDeliveryEnvelope::complete(std::expected<void, TreeError> result) const noexcept {
  if (acknowledgement) {
    if (!result) {
      acknowledgement->error.store(result.error(), std::memory_order_relaxed);
    }

    acknowledgement->state.store(result ? UiDeliveryState::APPLIED : UiDeliveryState::FAILED,
                                 std::memory_order_release);
  }
}

UiAcknowledgement::UiAcknowledgement(std::shared_ptr<const detail::UiDeliveryAcknowledgement> value)
    : state_(std::move(value)) {
}

UiDeliveryOutcome UiAcknowledgement::poll() const noexcept {
  if (!state_) {
    return {};
  }

  UiDeliveryOutcome result{state_->state.load(std::memory_order_acquire), {}};
  if (result.state == UiDeliveryState::FAILED) {
    result.error = state_->error.load(std::memory_order_relaxed);
  }

  return result;
}

std::expected<UiAcknowledgement, SchedulerError> detail::postUiDelivery(const Publisher<UiDeliveryEnvelope> &publisher,
                                                                        std::shared_ptr<const void> payload,
                                                                        bool acknowledge) {
  if (!payload) {
    return std::unexpected(SchedulerError::INVALID_ARGUMENT);
  }

  try {
    auto envelope = std::make_shared<detail::UiDeliveryEnvelope>();
    envelope->payload = std::move(payload);
    if (acknowledge) {
      envelope->acknowledgement = std::make_shared<detail::UiDeliveryAcknowledgement>();
    }

    UiAcknowledgement receipt(envelope->acknowledgement);
    auto accepted = publisher.publish(std::move(envelope));
    if (!accepted) {
      return std::unexpected(accepted.error());
    }

    return receipt;
  } catch (const std::bad_alloc &) {
    return std::unexpected(SchedulerError::NO_MEMORY);
  }
}
} // namespace portals
