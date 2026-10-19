// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/scroll_controls.hpp>
#include <portals/widget.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace portals {
namespace detail {
enum class ScrollSnapshotAdmission;
struct ScrollSnapshotAccess;
struct ScrollSnapshotTestAccess;
} // namespace detail
/** @brief Explicit capture cost profile; dimension-only scenes keep their original path. */
enum class LayoutSnapshotProfile {
  DIMENSIONS, ///< Original size records, without optional geometry storage.
  GEOMETRY    ///< Copy geometry, visibility and pure descendant clips for scroll preparation.
};

/** @brief Owning geometry independent of later live invalidation or widget destruction.
 * descendantClip precedes ancestor intersection; an off-viewport target may have an empty
 * cumulative clip while its unclipped surface rectangle remains useful for reveal.
 */
struct LayoutGeometrySnapshot {
  Node<Widget> parent;                                ///< Captured full parent identity, without ownership.
  NodeGeometry geometry;                              ///< Full local/surface bounds and cumulative paint clip.
  CellRect descendantClip;                            ///< Pure childClip translated to surface coordinates.
  bool visible{};                                     ///< Effective visibility; visible zero differs from hidden.
  std::optional<ScrollAreaLayoutSnapshot> scrollArea; ///< Copied derived values for an initialized area.
};

/** @brief Owning immutable identities and local sizes from one completed layout pass.
 * Copies share immutable value storage, without widgets, tree owners or borrowed text.
 * Captured values remain readable after tree invalidation, removal or destruction.
 * Concurrent const reads are safe; assignment requires caller synchronization.
 */
class LayoutSnapshot {
public:
  /** @brief Copy every live node in a subtree, including effectively hidden descendants.
   * Call on the tree's UI thread after successful layout, preferably inside its frame.
   * No layout, callbacks, mutation or frame request consumption is performed.
   * @param tree Live UI tree borrowed only during this call.
   * @param node Checked root of the subtree to capture; ancestor visibility is respected.
   * @return Owning snapshot, a tree access error, NO_MEMORY, or INVALID_GEOMETRY when
   * an effectively visible node has no valid arranged geometry. Hidden nodes need no
   * valid geometry and receive zero size; disabled nodes retain their arranged size.
   */
  [[nodiscard]] static std::expected<LayoutSnapshot, TreeError> capture(WidgetTree &tree, Node<Widget> node);
  /** @brief Opt into geometry capture without changing the original overload's allocation path.
   * @param tree UI tree after successful complete layout, inside its frame.
   * @param node Root identity; ancestor visibility is respected.
   * @param profile DIMENSIONS delegates the old overload; GEOMETRY owns optional parallel storage.
   * @return Owning snapshot or original tree/allocation/geometry/clip error; unknown profile is INVALID_ARGUMENT.
   * Extra O(nodes) work/storage applies only to GEOMETRY. Existing pure childClip runs under
   * tree.read; no controller publication, invalidation or user notification occurs.
   */
  [[nodiscard]] static std::expected<LayoutSnapshot, TreeError> capture(WidgetTree &tree, Node<Widget> node,
                                                                        LayoutSnapshotProfile profile);
  /** @brief Read a captured local extent without consulting the current tree.
   * @param node Observer identifying the captured session, slot and generation.
   * @return Captured width/height in cells, zero for an effectively hidden node,
   * WRONG_SESSION for another tree, or STALE_NODE for an empty/absent identity or
   * a moved-from snapshot. Removal after capture does not invalidate captured identities.
   * @note No allocation, tree affinity, mutation or exceptions; clipping does not reduce this extent.
   */
  [[nodiscard]] std::expected<CellSize, TreeError> viewport(Node<Widget> node) const noexcept;
  /** @param node Captured full session/slot/generation identity.
   * @return Owning geometry, INVALID_ARGUMENT without GEOMETRY, or WRONG_SESSION/STALE_NODE.
   * No live tree is consulted; copies and concurrent const reads are safe.
   */
  [[nodiscard]] std::expected<LayoutGeometrySnapshot, TreeError> geometry(Node<Widget> node) const noexcept;
  /** @return Nonzero tree revision at capture, or INVALID_ARGUMENT without GEOMETRY profile.
   * A completed-pass identity does not grant permission to read later live geometry.
   */
  [[nodiscard]] std::expected<std::uint64_t, TreeError> geometryRevision() const noexcept;

private:
  friend struct detail::ScrollSnapshotAccess;
  friend struct detail::ScrollSnapshotTestAccess;
  std::expected<detail::ScrollSnapshotAdmission, TreeError> admitScrollSnapshot(WidgetTree &tree) const;

  struct Record {
    NodeId id;
    CellSize size;
  };

  struct GeometryRecord {
    NodeId id;
    LayoutGeometrySnapshot value;
  };

  LayoutSnapshot(std::uint64_t session, std::shared_ptr<const std::vector<Record>> records);
  LayoutSnapshot(std::uint64_t session, std::shared_ptr<const std::vector<Record>> records,
                 std::shared_ptr<const std::vector<GeometryRecord>> geometry, std::uint64_t revision,
                 std::uint64_t inputGeneration);
  std::uint64_t session_;
  std::shared_ptr<const std::vector<Record>> records_;
  std::shared_ptr<const std::vector<GeometryRecord>> geometry_;
  std::uint64_t geometryRevision_{};
  std::uint64_t inputGeneration_{};
};
} // namespace portals
