// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/geometry.hpp>
#include <portals/render.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace portals {
/** @brief Owning identity, independent of addresses; zero is never a live identity. */
struct NodeId {
  std::uint64_t session{};                                  ///< Process-unique tree session.
  std::size_t slot{};                                       ///< Registry position, not sibling order.
  std::uint64_t generation{};                               ///< Reuse generation; exhausted slots retire.
  bool operator==(const NodeId &) const noexcept = default; ///< Compare identity.
};
/** @brief Checked tree failures; rejected operations never start the requested callback. */
enum class TreeError {
  invalid_argument,
  wrong_thread,
  wrong_session,
  stale_node,
  closed,
  would_block,
  no_memory,
  identity_exhausted,
  node_limit,
  depth_limit,
  callback_failed,
  invalid_geometry
};
/** @brief Accumulated work; layout always includes painting. */
enum class Invalidation {
  none,
  paint,
  layout
};
/** @brief Owning arranged geometry; invalid geometry must never be used for hit-testing. */
struct NodeGeometry {
  CellRect local;   ///< Parent-relative rectangle.
  CellRect surface; ///< Explicitly translated surface rectangle.
  CellRect clip;    ///< Surface intersection with every ancestor and viewport.
  bool valid{};     ///< False before arrangement or after layout invalidation/failure.
};
/** @brief Finite storage and recursive traversal limits, including the root node. */
struct WidgetTreeOptions {
  std::size_t max_nodes{4096}; ///< Live and not-yet-reclaimed nodes share this budget.
  std::size_t max_depth{128};  ///< Root depth is one; values above 256 are rejected.
};
/** @brief Borrowed synchronous command sink supplied by the frame owner.
 * submit must consume commands immediately or retain their text only until the enclosing
 * WidgetTree::frame callback returns. The frame owner must render before leaving that
 * callback. No command/text/context borrow may survive the frame. No terminal I/O here.
 */
class PaintContext {
public:
  virtual ~PaintContext() = default; ///< Destroy the caller-owned sink.
  virtual std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) = 0; ///< Borrow commands.
};
} // namespace portals
