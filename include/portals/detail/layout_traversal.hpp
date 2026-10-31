// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <algorithm>
#include <portals/layout.hpp>

namespace portals {
class LayoutSnapshot;
}

namespace portals::detail {
/** @brief Intersect a copied widget policy with proposed state-independent style constraints.
 * @param policy Owning widget policy to validate without mutation.
 * @param styleMinWidth Nonnegative hard width bound from the staged style.
 * @param sizing Additional constraints borrowed only during this call, or null.
 * @return Complete validated policy, or INVALID_ARGUMENT for inconsistent bounds or modes.
 * No style borrow escapes this synchronous call.
 */
inline std::expected<SizePolicy, TreeError> effectiveSizePolicy(SizePolicy policy, std::int32_t styleMinWidth,
                                                                const StyleSizing *sizing) {
  const auto valid = [](AxisSize axis) {
    return axis.minimum >= 0 && axis.maximum >= axis.minimum && axis.preferred >= INTRINSIC_EXTENT && axis.weight > 0 &&
           (axis.mode == SizeMode::AUTOMATIC || axis.mode == SizeMode::FIXED || axis.mode == SizeMode::FLEX ||
            axis.mode == SizeMode::CONTENT);
  };

  if (styleMinWidth < 0 || !valid(policy.width) || !valid(policy.height)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  policy.width.minimum = std::max(policy.width.minimum, styleMinWidth);

  const auto intersect = [](AxisSize &axis, const std::optional<StyleAxisSize> &request,
                            std::optional<std::int32_t> minimum, std::optional<std::int32_t> maximum) {
    if ((minimum && *minimum < 0) || (maximum && *maximum < 0)) {
      return false;
    }

    if (minimum) {
      axis.minimum = std::max(axis.minimum, *minimum);
    }

    if (maximum) {
      axis.maximum = std::min(axis.maximum, *maximum);
    }

    if (request) {
      if (request->weight == 0 || request->extent < 0) {
        return false;
      }

      axis.preferred = INTRINSIC_EXTENT;
      axis.weight = 1;

      switch (request->mode) {
      case StyleSizeMode::FIXED:
        axis.minimum = std::max(axis.minimum, request->extent);
        axis.maximum = std::min(axis.maximum, request->extent);
        axis.preferred = request->extent;
        axis.mode = SizeMode::FIXED;

        break;
      case StyleSizeMode::AUTOMATIC:
        axis.mode = SizeMode::AUTOMATIC;

        break;
      case StyleSizeMode::CONTENT:
        axis.mode = SizeMode::CONTENT;

        break;
      case StyleSizeMode::FLEX:
        axis.mode = SizeMode::FLEX;
        axis.preferred = 0;
        axis.weight = request->weight;

        break;
      default:
        return false;
      }
    }

    return axis.maximum >= axis.minimum;
  };

  if (sizing && (!intersect(policy.width, sizing->width, {}, sizing->maxWidth) ||
                 !intersect(policy.height, sizing->height, sizing->minHeight, sizing->maxHeight))) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  if (policy.width.maximum < policy.width.minimum) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  return policy;
}

/** @param widget Borrowed policy owner; no reference escapes.
 * @param styleMinWidth Proposed nonnegative hard column minimum.
 * @param sizing Proposed additional style constraints, borrowed synchronously.
 * @return Validated intersection, or INVALID_ARGUMENT without mutation.
 */
inline std::expected<SizePolicy, TreeError> effectiveSizePolicy(const Widget &widget, std::int32_t styleMinWidth,
                                                                const StyleSizing *sizing = nullptr) {
  const auto *item = dynamic_cast<const LayoutItem *>(&widget);

  return effectiveSizePolicy(item ? item->sizePolicy() : SizePolicy{}, styleMinWidth, sizing);
}

/** @param widget Borrowed widget with its last atomically published style.
 * @return Validated effective policy with the installed style minimum.
 */
inline std::expected<SizePolicy, TreeError> effectiveSizePolicy(const Widget &widget) {
  const auto &style = widget.computedStyle();

  return effectiveSizePolicy(widget, style ? style->minWidth : 0, style ? style->sizing() : nullptr);
}

/** @brief One fresh edge proposal in an owner's complete desired set.
 * An absent child clears only that owner's previous gate.
 */
struct RegionalInputProposal {
  NodeId child;                  ///< Full direct-child identity.
  bool open{};                   ///< Whether the current region admits input.
  std::uint64_t geometryEpoch{}; ///< Nonreused arrangement stamp.
};

/** @brief Complete borrowed proposals for one owner, consumed synchronously. */
struct RegionalInputOwnerProposal {
  Node<Widget> owner;                              ///< Checked identity whose direct children are governed.
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
  static std::expected<void, TreeError> arrange(WidgetTree &tree, Node<Widget> node, CellRect local, CellRect viewport,
                                                const ParentLayoutEdge &edge) {
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
