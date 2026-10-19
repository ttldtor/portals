// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "action_state.hpp"
#include "scroll_controls_state.hpp"

#include <limits>

namespace portals::detail {
std::expected<std::shared_ptr<const FocusObservation>, TreeError> EventRouterAccess::observeFocus(EventRouter &router) {
  auto allowed = router.check();
  if (!allowed) {
    return std::unexpected(allowed.error());
  }
  auto root = router.tree().root();
  if (!root) {
    return std::unexpected(root.error());
  }
  if (auto existing = router.state_->focusObservation.lock()) {
    if (!existing->retired && existing->session == root->id().session) {
      return std::shared_ptr<const FocusObservation>(std::move(existing));
    }
  }
  try {
    auto created = std::make_shared<FocusObservation>(root->id().session);
    // Allocation may admit another scene first. Preserve that winning scoped value.
    auto checked = router.check();
    if (!checked) {
      return std::unexpected(checked.error());
    }
    if (auto existing = router.state_->focusObservation.lock()) {
      if (existing->retired || existing->session != root->id().session) {
        return std::unexpected(TreeError::WRONG_SESSION);
      }
      return std::shared_ptr<const FocusObservation>(std::move(existing));
    }
    created->focused = router.state_->focus;
    created->generation = router.state_->focusGeneration;
    router.state_->focusObservation = created;
    return std::shared_ptr<const FocusObservation>(std::move(created));
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

namespace {
std::shared_ptr<ScrollIntentRecord> admittedIntent(const ScrollWidgetState &state) noexcept {
  auto intent = state.intent.lock();
  if (!intent || intent->owner != std::this_thread::get_id() || intent->node != state.node) {
    return {};
  }
  auto focus = intent->focus.lock();
  if (!focus || focus->owner != std::this_thread::get_id() || focus->session != state.node.session || focus->retired) {
    return {};
  }
  return intent;
}
} // namespace

std::expected<void, TreeError> checkScrollIntent(const ScrollWidgetState &state, const ScrollControllerPlan &plan) {
  auto intent = admittedIntent(state);
  if (!intent || intent->serial != std::numeric_limits<std::uint64_t>::max()) {
    return {};
  }
  auto current = state.controller->snapshot();
  if (!current) {
    return std::unexpected(current.error());
  }
  const bool logicalChanged = current->horizontal.offset.raster != plan.horizontal.rasterOffset() ||
                              current->horizontal.offset.fraction != plan.horizontal.fraction() ||
                              current->vertical.offset.raster != plan.vertical.rasterOffset() ||
                              current->vertical.offset.fraction != plan.vertical.fraction();
  return logicalChanged ? std::expected<void, TreeError>(std::unexpected(TreeError::IDENTITY_EXHAUSTED))
                        : std::expected<void, TreeError>{};
}

void recordScrollIntent(const ScrollWidgetState &state) noexcept {
  auto intent = admittedIntent(state);
  if (!intent) {
    return;
  }
  auto focus = intent->focus.lock();
  if (!focus || intent->serial == std::numeric_limits<std::uint64_t>::max()) {
    return;
  }
  ++intent->serial;
  intent->focused = focus->focused.id();
  intent->focusGeneration = focus->generation;
}
} // namespace portals::detail
