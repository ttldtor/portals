// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/widget.hpp>

namespace portals {
class LayoutSnapshot;
}

namespace portals::detail {
/** @brief One fresh edge proposal in an owner's complete desired set.
 * An absent child clears only that owner's previous gate.
 */
struct RegionalInputProposal {
  NodeId child; ///< Full direct-child identity.
  bool open{}; ///< Whether the current region admits input.
  std::uint64_t geometryEpoch{}; ///< Nonreused arrangement stamp.
};

/** @brief Complete borrowed proposals for one owner, consumed synchronously. */
struct RegionalInputOwnerProposal {
  Node<Widget> owner; ///< Checked identity whose direct children are governed.
  std::span<const RegionalInputProposal> children; ///< Borrow ends when adoption returns.
};

/** @brief Internal checked bridge for owning layout edges and regional input.
 * Public arrange retains its ordinary clipping fallback.
 */
struct LayoutTraversalAccess {
  /** @param tree UI-affine tree borrowed during arrangement.
   * @param node Checked child identity.
   * @param local Parent-local allocation.
   * @param viewport Complete surface viewport.
   * @param edge Fresh edge copied synchronously; no widget borrow escapes.
   * @return Success or the tree's unchanged arrangement/geometry/allocation error.
   */
  static std::expected<void, TreeError> arrange(WidgetTree &tree, Node<Widget> node, CellRect local,
                                               CellRect viewport, const ParentLayoutEdge &edge) {
    return tree.arrangeImpl(node, local, viewport, edge.surfaceClip, &edge);
  }

  /** @brief Validate and allocate the whole batch before publishing any gate.
   * @param tree UI-affine tree, between frames and outside guarded callbacks.
   * @param snapshot Fresh owning GEOMETRY snapshot with current input history.
   * @param owners Complete direct-child proposals, borrowed only during this call.
   * @return Whether admission changed, or a typed identity/geometry/allocation error.
   * No callback runs during validation or publication.
   */
  static std::expected<bool, TreeError> adoptRegionalInput(WidgetTree &tree, const LayoutSnapshot &snapshot,
                                                          std::span<const RegionalInputOwnerProposal> owners);
};
} // namespace portals::detail
