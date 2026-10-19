// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "scroll_intent.hpp"
#include <portals/scroll_preparation.hpp>

namespace portals::detail {
enum class ScrollSnapshotAdmission {
  CURRENT,
  STALE
};

struct ScrollSnapshotAccess {
  static std::expected<ScrollSnapshotAdmission, TreeError> admit(WidgetTree &tree, const LayoutSnapshot &snapshot) {
    return snapshot.admitScrollSnapshot(tree);
  }
};

struct ScrollPreparationAdoption {
  ScrollSnapshotAdmission admission{ScrollSnapshotAdmission::CURRENT};
  ScrollPreparationResult result;
};
struct ScrollPreparationPlan;

struct ScrollIntentWatermark {
  NodeId node;
  std::uint64_t serial{};
};

struct ScrollRequest {
  ScrollRevealTarget reveal;
  Node<Widget> focused;
  std::uint64_t focusGeneration{};
  std::uint64_t serial{};
  bool explicitRequest{};
  bool settled{};
  std::vector<Node<Widget>> chain = std::vector<Node<Widget>>(0);
  std::vector<ScrollIntentWatermark> intents = std::vector<ScrollIntentWatermark>(0);
};

struct ManagedScrollState {
  explicit ManagedScrollState(std::shared_ptr<const FocusObservation> focus) : focus(std::move(focus)) {
  }

  std::shared_ptr<const FocusObservation> focus;
  Node<Widget> observedFocus;
  std::uint64_t observedGeneration{};
  bool automaticExhausted{};
  std::uint64_t explicitSerial{};
  std::unique_ptr<ScrollRequest> pending;
  Node<Widget> consumedFocus;
  std::uint64_t consumedGeneration{};
  std::uint64_t consumedExplicit{};
};

// Build owns every geometry/chain value before the caller rechecks focus and command receipts.
// Refresh and adoption allocate nothing and invoke no user code; stale requests can be discarded
// without discarding newer canonical offsets committed during a build allocation.
std::expected<std::shared_ptr<ScrollPreparationPlan>, TreeError>
planScrollAreas(WidgetTree &tree, std::span<const Node<ScrollArea>> areas, const LayoutSnapshot &snapshot,
                std::optional<ScrollRevealTarget> reveal);
std::expected<ScrollSnapshotAdmission, TreeError> admitScrollPlan(WidgetTree &tree, const ScrollPreparationPlan &plan);
std::expected<ScrollSnapshotAdmission, TreeError> refreshScrollPlans(WidgetTree &tree, ScrollPreparationPlan &plan,
                                                                     bool reveal);
std::expected<ScrollPreparationAdoption, TreeError> adoptScrollAreas(WidgetTree &tree, ScrollPreparationPlan &plan);
} // namespace portals::detail
