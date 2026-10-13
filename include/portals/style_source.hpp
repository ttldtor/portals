// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/pss_data.hpp>

namespace portals {
namespace detail {
/** @brief Validate mutable document shape and scalar provenance without owning copies or allocation.
 * @param document Borrowed runtime document; no aliases are retained.
 * @return Exact structural PSS failure, or success before any linked index can be used.
 */
std::expected<void, PssError> validateStyleDocument(const PssDocument &document);
} // namespace detail

/** @brief Owning provenance snapshot for one real engine layer, including unknown typed sources.
 * The observing handle never owns the scene. Retaining document extends only immutable metadata lifetime.
 */
struct StyleLayerSource {
  /** @brief Suppress implicit noexcept moves because checked MSVC string transfers can allocate. */
  ~StyleLayerSource() = default;

  StyleLayerHandle handle; ///< UI-affine weak identity; stale after removal, whole replacement, or scene destruction.
  StyleTier tier{StyleTier::DEFAULTS}; ///< Actual cascade priority of the observed layer.
  std::string name{""};                ///< Owned engine layer identity, independent of the document source label.
  std::shared_ptr<const PssDocument> document; ///< Owned immutable metadata, or null for a low-level/unknown source.
};
} // namespace portals