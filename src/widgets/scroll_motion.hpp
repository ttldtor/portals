// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "scroll_controls_state.hpp"

namespace portals::detail {
// Node-scoped input services own this optional record; session/callback links are weak.
struct ScrollMotion {
  std::weak_ptr<ScrollRepeatState> input;
  std::weak_ptr<ScrollMountSession> session;
  Subscription timer;
  std::array<ScrollOffset, 2> from;
  std::array<ScrollOffset, 2> target;
  SchedulerTime started{};
  std::uint64_t revision{};
  std::uint16_t duration{};
  bool active{};

  void cancel() noexcept;
};

bool smoothScrollEnabled(const ScrollWidgetState &state) noexcept;
void cancelScrollMotion(ScrollWidgetState &state) noexcept;
void cancelOwnedScrollMotion(ScrollWidgetState &state) noexcept;
// A relative input starts from the pending destination, including another projection's motion.
std::expected<ScrollControllerPlan, TreeError> stageScrollInput(ScrollWidgetState &state, bool accumulate);
std::expected<EventReply, TreeError> publishScrollInput(WidgetTree &tree, Node<Widget> node, ScrollWidgetState &state,
                                                        ScrollControllerPlan plan, bool smooth);
} // namespace portals::detail
