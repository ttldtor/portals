// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_ref.hpp>

namespace portals::ui::detail {
std::expected<void, ManagedError> scrollOffset(const std::shared_ptr<RefTarget> &target, ScrollAxis axis,
                                               ScrollOffset offset, std::source_location origin) {
  auto owner = eventScene(target);
  if (!owner) {
    return std::unexpected(owner.error());
  }
  auto result = (*owner)->setScrollOffset(target->node, axis, offset);
  if (!result) {
    reportEventError(target, result.error(), origin);
  }
  return result;
}

std::expected<void, ManagedError> ensureVisible(const std::shared_ptr<RefTarget> &target, std::source_location origin) {
  auto owner = eventScene(target);
  if (!owner) {
    return std::unexpected(owner.error());
  }
  auto result = (*owner)->ensureVisible(target->node);
  if (!result) {
    reportEventError(target, result.error(), origin);
  }
  return result;
}
} // namespace portals::ui::detail
