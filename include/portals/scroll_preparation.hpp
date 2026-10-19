// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/layout_snapshot.hpp>

namespace portals {
/** @brief Owning one-shot reveal input for an explicit low-level scene preparation.
 * Managed scenes retain their own focus/explicit serial and completion watermark. Custom scenes
 * must likewise pass this value only while their request is pending; scrolling alone is not a request.
 */
struct ScrollRevealTarget {
  Node<Widget> target; ///< Full live identity whose available footprint should become visible.
  Node<Widget> root;   ///< Optional ancestor limiting reveal; empty includes all captured ancestors.
};

/** @brief Owning result; successful preparation does not imply successful final rendering. */
struct ScrollPreparationResult {
  bool layoutChanged{}; ///< Metrics/raster adoption requested another existing layout pass.
  bool revealMoved{};   ///< At least one integral reveal position differed from the clamped plan.
};

/** @brief Prepare all ordinary scroll areas against one immutable geometry profile.
 * @param tree Live UI tree, between frames and outside callbacks that mutate this same registration set.
 * @param areas Initialized unique area identities; order does not affect nested reveal.
 * @param snapshot GEOMETRY profile captured after the complete preliminary layout, before invalidations.
 * @param reveal Optional pending one-shot target; readOnly does not prohibit this programmatic reveal.
 * @return Adoption result or checked tree/identity/geometry/allocation/overflow error. Hidden areas preserve
 * values; visible zero viewports clamp. All allocations and plan validation precede publication.
 * A later invalidation error reports already published effects honestly. Custom scenes consume a request
 * only after successful final layout/paint/render, never merely on this function's success.
 * Eligibility/removal history changed since capture returns WOULD_BLOCK without publishing metrics or offsets.
 * Exhausted input-history identity returns IDENTITY_EXHAUSTED for this checked geometry adoption only;
 * focus-generation exhaustion is separate and still permits explicit reveal.
 * @note Uses existing controller arithmetic and tree invalidation, without callbacks, timers or a second loop.
 */
std::expected<ScrollPreparationResult, TreeError> prepareScrollAreas(WidgetTree &tree,
                                                                     std::span<const Node<ScrollArea>> areas,
                                                                     const LayoutSnapshot &snapshot,
                                                                     std::optional<ScrollRevealTarget> reveal = {});
} // namespace portals
