// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "action_state.hpp"

namespace portals {
PointerCapture::PointerCapture(std::weak_ptr<detail::PointerCaptureCancellation> cancellation) noexcept
    : cancellation_(std::move(cancellation)) {
}

PointerCapture::~PointerCapture() {
  disconnect();
}

PointerCapture::PointerCapture(PointerCapture &&other) noexcept : cancellation_(std::move(other.cancellation_)) {
}

PointerCapture &PointerCapture::operator=(PointerCapture &&other) noexcept {
  if (this != &other) {
    disconnect();
    cancellation_ = std::move(other.cancellation_);
  }

  return *this;
}

void PointerCapture::disconnect() noexcept {
  if (auto cancellation = cancellation_.lock()) {
    cancellation->connected.store(false);
  }

  cancellation_.reset();
}

bool PointerCapture::connected() const noexcept {
  auto cancellation = cancellation_.lock();

  if (!cancellation || !cancellation->connected.load()) {
    return false;
  }

  auto binding = cancellation->binding.lock();

  return binding && binding->connected.load();
}

std::expected<void, TreeError> EventRouter::retirePointerCapture(bool requestPaint) {
  if (!state_->pointerCapture) {
    return {};
  }

  const auto node = state_->pointerCapture->node;

  state_->pointerCapture->cancellation->connected.store(false);
  state_->pointerCapture.reset();

  if (requestPaint) {
    auto painted = tree().invalidate(node, Invalidation::PAINT);

    if (!painted && painted.error() != TreeError::STALE_NODE && painted.error() != TreeError::CLOSED) {
      return painted;
    }
  }

  return {};
}

std::expected<Node<Widget>, TreeError> EventRouter::checkedPointerCapture() {
  if (!state_->pointerCapture) {
    return Node<Widget>{};
  }

  const auto &capture = *state_->pointerCapture;
  auto entry = capture.registration.lock();
  // The existing global eligibility stamp also detects hide/show and disable/enable
  // between UI checks. Unrelated eligibility changes conservatively end the sequence.
  const bool connected = capture.inputGeneration != 0 && capture.inputGeneration == tree().inputGeneration() &&
                         capture.cancellation->connected.load() && entry && entry->cancellation->connected.load() &&
                         entry->node == capture.node && registration(capture.node) == entry;

  if (connected) {
    auto eligible = tree().inputEligible(capture.node);

    if (!eligible && eligible.error() != TreeError::STALE_NODE && eligible.error() != TreeError::CLOSED) {
      return std::unexpected(eligible.error());
    }

    // Eligibility is independent of geometry. In particular, LAYOUT invalidation between
    // two queued drag moves must not retire the same live, enabled registration.
    if (eligible && *eligible && within(capture.focusRoot, capture.node)) {
      return capture.node;
    }
  }

  auto retired = retirePointerCapture(true);

  return retired ? std::expected<Node<Widget>, TreeError>(Node<Widget>{}) : std::unexpected(retired.error());
}

std::expected<void, TreeError> EventRouter::setPointerCaptureCapabilities(PointerCaptureCapabilities capabilities) {
  auto allowed = check();

  if (!allowed) {
    return allowed;
  }

  if (state_->dispatching || state_->deliveringFocus) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  const auto previous = state_->pointerCapabilities;

  state_->pointerCapabilities = capabilities;

  if (previous.motion != capabilities.motion || previous.release != capabilities.release ||
      previous.motionButtonIdentity != capabilities.motionButtonIdentity) {
    return retirePointerCapture(true);
  }

  return {};
}

std::expected<PointerCapture, TreeError> EventRouter::capturePointer(Node<Widget> owner, Node<Widget> focusRoot) {
  auto allowed = check();

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  auto eligible = tree().inputEligible(owner);

  if (!eligible) {
    return std::unexpected(eligible.error());
  }

  auto admitted = state_->admittedAction.lock();

  if (state_->deliveringFocus || !state_->dispatching || !admitted || !state_->admittedMouse ||
      state_->admittedMouse->action != MouseAction::PRESS || state_->admittedMouse->button != MouseButton::LEFT) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  if (!*eligible || admitted->node != owner || !admitted->cancellation->connected.load() ||
      registration(owner) != admitted || !state_->pointerCapabilities.motion || !state_->pointerCapabilities.release) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  if (focusRoot == Node<Widget>{}) {
    focusRoot = owner;
  }

  auto rootEligible = tree().inputEligible(focusRoot);

  if (!rootEligible) {
    return std::unexpected(rootEligible.error());
  }

  if (!*rootEligible || !within(focusRoot, owner)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  auto previous = checkedPointerCapture();

  if (!previous) {
    return std::unexpected(previous.error());
  }

  if (previous->id().session != 0) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  if (tree().inputGeneration() == 0) {
    return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
  }

  try {
    auto cancellation = std::make_shared<detail::PointerCaptureCancellation>();
    auto ownerLive = tree().inputEligible(owner);
    auto rootLive = tree().inputEligible(focusRoot);

    // Allocation may use a reentrant client allocator. Revalidate before publishing;
    // its side effects are not rolled back and cannot make this overwrite a new capture.
    if (!ownerLive || !rootLive) {
      return std::unexpected(!ownerLive ? ownerLive.error() : rootLive.error());
    }

    if (!*ownerLive || !*rootLive || !within(focusRoot, owner) || !admitted->cancellation->connected.load() ||
        registration(owner) != admitted || !state_->pointerCapabilities.motion ||
        !state_->pointerCapabilities.release) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    auto current = checkedPointerCapture();

    if (!current) {
      return std::unexpected(current.error());
    }

    if (current->id().session != 0) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }

    if (tree().inputGeneration() == 0) {
      return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
    }

    cancellation->binding = admitted->cancellation;
    state_->pointerCapture.emplace(
      detail::PointerCaptureRecord{owner, focusRoot, admitted, cancellation, tree().inputGeneration()});

    return PointerCapture(cancellation);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<Node<Widget>, TreeError> EventRouter::pointerCapture() {
  auto allowed = collect();

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  return checkedPointerCapture();
}

std::expected<void, TreeError> EventRouter::cancelPointerCapture(bool requestPaint) {
  auto allowed = check();

  if (!allowed) {
    if (allowed.error() == TreeError::CLOSED) {
      static_cast<void>(retirePointerCapture(false));
    }

    return allowed;
  }

  return retirePointerCapture(requestPaint);
}
} // namespace portals
