// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/widget.hpp>
#include <thread>

namespace portals::detail {
// Only immutable affinity/session are read before UI admission. Mutable values are not thread-safe.
// Scene scroll state owns this scoped value; router holds a weak writer, never a scene reference.
struct FocusObservation {
  explicit FocusObservation(std::uint64_t session) : session(session) {
  }

  const std::thread::id owner{std::this_thread::get_id()};
  const std::uint64_t session;
  Node<Widget> focused;
  std::uint64_t generation{};
  bool retired{};
};

// One existing managed Entry owns each receipt. Widget code observes it weakly and never calls out.
struct ScrollIntentRecord {
  ScrollIntentRecord(NodeId node, std::weak_ptr<const FocusObservation> focus) : node(node), focus(std::move(focus)) {
  }

  const std::thread::id owner{std::this_thread::get_id()};
  const NodeId node;
  std::weak_ptr<const FocusObservation> focus;
  std::uint64_t serial{};
  NodeId focused;
  std::uint64_t focusGeneration{};
};

struct ScrollWidgetState;
struct ScrollControllerPlan;
std::expected<void, TreeError> checkScrollIntent(const ScrollWidgetState &state, const ScrollControllerPlan &plan);
void recordScrollIntent(const ScrollWidgetState &state) noexcept;
} // namespace portals::detail
