// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/layout.hpp>

namespace portals {
/** @brief Owning explicit placement, keyed by full identity rather than visible sibling index. */
struct GridCell {
  NodeId node;                ///< Owning identity; hidden/removed entries never bind a replacement node.
  std::size_t row{};          ///< Zero-based first row.
  std::size_t column{};       ///< Zero-based first column.
  std::size_t row_span{1};    ///< Positive span wholly within the explicit rows.
  std::size_t column_span{1}; ///< Positive span wholly within the explicit columns.
  LayoutAlign horizontal{LayoutAlign::stretch}; ///< Child alignment within its columns.
  LayoutAlign vertical{LayoutAlign::stretch};   ///< Child alignment within its rows.
};
/** @brief Owning sparse grid configuration; no implicit tracks or automatic placement. */
struct GridOptions {
  std::vector<AxisSize> columns{AxisSize{}};              ///< Explicit horizontal tracks using shared size policies.
  std::vector<AxisSize> rows{AxisSize{}};                 ///< Explicit vertical tracks using shared size policies.
  std::int32_t column_gap{};                              ///< Nonnegative gap, included inside a spanning cell.
  std::int32_t row_gap{};                                 ///< Nonnegative gap, included inside a spanning cell.
  LayoutPadding padding;                                  ///< Insets retained during shortage.
  std::vector<GridCell> cells = std::vector<GridCell>(0); ///< Unique identities; overlap is allowed in sibling order.
  std::size_t max_tracks_per_axis{4096};                  ///< Positive work budget for each explicit axis.
  std::size_t max_placements{4096};                       ///< Positive work budget, including inactive placements.
};
/** @brief UI-owned Grid implementing the existing pure Layout contract.
 * Every visible child needs a placement. All spans are checked, including hidden/stale entries.
 * Later siblings paint/hit above earlier siblings; placement order does not change stacking.
 * Intrinsic tracks absorb child demand, then Row's allocation rules apply independently on each axis.
 * Constructors own settings; invalid settings are reported by Layout methods. Layout errors are
 * invalid_argument, node_limit, invalid_geometry or no_memory. No I/O, retained spans or tree traversal.
 */
class Grid : public SizedWidget, public Layout {
public:
  explicit Grid(const GridOptions &options = {}, SizePolicy policy = {}); ///< Own settings and size policies.
  GridOptions options() const; ///< Owning UI-thread copy; allocation may throw bad_alloc.
  std::expected<CellSize, TreeError> child_available(CellSize available) const override; ///< Checked inset bounds.
  std::expected<CellSize, TreeError>
  measure_children(std::span<const MeasuredChild> children) const override; ///< Aggregate tracks.
  std::expected<std::vector<CellRect>, TreeError>
  arrange_children(std::span<const MeasuredChild> children,
                   CellSize available) const override; ///< Child-order local rectangles.
private:
  friend std::expected<void, TreeError> set_grid_options(WidgetTree &, Node<Grid>, const GridOptions &);
  GridOptions options_;
};
/** @brief Owning overlay alignment and insets; all children share the content rectangle. */
struct OverlayOptions {
  LayoutPadding padding;                        ///< Insets retained during shortage.
  LayoutAlign horizontal{LayoutAlign::stretch}; ///< Alignment of each child across columns.
  LayoutAlign vertical{LayoutAlign::stretch};   ///< Alignment of each child across rows.
};
/** @brief UI-owned overlay using the existing traversal, geometry and sibling stacking.
 * Later siblings are above earlier siblings for normal paint order and EventRouter hit testing.
 * Layout methods are pure and follow Layout's borrowed-span contract. Invalid settings/policies
 * return invalid_argument, checked overflow invalid_geometry, allocation failure no_memory.
 */
class Overlay : public SizedWidget, public Layout {
public:
  explicit Overlay(OverlayOptions options = {}, SizePolicy policy = {}); ///< Own settings and size policies.
  OverlayOptions options() const;                                        ///< Owning UI-thread copy.
  std::expected<CellSize, TreeError> child_available(CellSize available) const override; ///< Checked inset bounds.
  std::expected<CellSize, TreeError>
  measure_children(std::span<const MeasuredChild> children) const override; ///< Maximum child extent on each axis.
  std::expected<std::vector<CellRect>, TreeError>
  arrange_children(std::span<const MeasuredChild> children,
                   CellSize available) const override; ///< Child-order local rectangles.
private:
  friend std::expected<void, TreeError> set_overlay_options(WidgetTree &, Node<Overlay>, OverlayOptions);
  OverlayOptions options_;
};
/** @brief Validate and atomically copy settings on the UI owner between frames, then invalidate layout.
 * Tree errors propagate; invalid configuration leaves the previous settings unchanged.
 * Input is borrowed only for this call. Allocation failure returns no_memory.
 */
std::expected<void, TreeError> set_grid_options(WidgetTree &tree, Node<Grid> node, const GridOptions &options);
/** @brief Validate and replace settings on the UI owner between frames, then invalidate layout. */
std::expected<void, TreeError> set_overlay_options(WidgetTree &tree, Node<Overlay> node, OverlayOptions options);
} // namespace portals
