// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <optional>
#include <portals/layout.hpp>
#include <portals/scroll_controller.hpp>
#include <portals/scroll_options.hpp>

namespace portals {
namespace detail {
struct ScrollWidgetState;
struct ScrollWidgetAccess;
struct GridLayoutState;
/** @param policies Copied track values borrowed only for validation.
 * @return Success or INVALID_ARGUMENT without allocation, mutation or callbacks.
 */
std::expected<void, TreeError> validateGridTrackSizes(std::span<const AxisSize> policies) noexcept;
} // namespace detail

/** @brief Independent local row coordinate space sharing the grid's columns. */
enum class GridRegion {
  BODY,  ///< Ordinary content rows, optionally below a header.
  HEADER ///< Independent heading rows sharing the horizontal tracks.
};

/** @brief Optional owning row backgrounds; logical row zero is even. */
struct GridRowStyles {
  std::optional<CellStyle> even; ///< Background across an even body row, including column gaps.
  std::optional<CellStyle> odd;  ///< Background across an odd body row, including column gaps.
  bool operator==(const GridRowStyles &) const = default; ///< @return Whether both row backgrounds match.
};

/** @brief Backgrounds only; these values never inherit into child control state styles. */
struct GridPresentation {
  std::optional<CellStyle> header;  ///< Complete header background, below real controls.
  std::optional<CellStyle> body;    ///< Body background, including row gaps.
  GridRowStyles rows;               ///< Optional logical body row backgrounds, independent of visibility.
  std::size_t maxPaintCells{16384}; ///< Positive total background plus scroll chrome paint-work budget.
  bool operator==(const GridPresentation &) const = default; ///< @return Whether all presentation values match.
};

/** @brief Owning opt-in to the existing scroll-controller lifecycle, fixed before mount. */
struct GridScrollOptions {
  ScrollAreaOptions area = [] {
    ScrollAreaOptions value;
    value.focusable = false;
    return value;
  }(); ///< Existing area options; a grid adds no focus stop by default.
  std::shared_ptr<ScrollController> controller; ///< Shared owning values; empty creates an independent controller.
};
/** @brief Direction in which declarative placement searches for the next free origin. */
enum class GridFlow {
  ROW_MAJOR,   ///< Advance through columns before moving to the next body row.
  COLUMN_MAJOR ///< Advance through body rows before moving to the next column.
};

/** @brief Construction-time placement mode; explicit grids retain their overlap semantics. */
enum class GridPlacementMode {
  EXPLICIT,   ///< Preserve low-level overlapping cells in sibling paint/input order.
  DECLARATIVE ///< Reject overlapping cells within each independent region.
};

/** @brief Owning parent-child relationship; omitted coordinates request automatic placement. */
struct GridItemOptions {
  std::optional<std::size_t> row;               ///< Local zero-based row; must be supplied together with column.
  std::optional<std::size_t> column;            ///< Local zero-based column; must be supplied together with row.
  std::size_t rowSpan{1};                       ///< Positive number of occupied rows.
  std::size_t columnSpan{1};                    ///< Positive number of occupied columns.
  LayoutAlign horizontal{LayoutAlign::STRETCH}; ///< Horizontal alignment inside the complete span.
  LayoutAlign vertical{LayoutAlign::STRETCH};   ///< Vertical alignment inside the complete span.
  GridRegion region{GridRegion::BODY};          ///< Independent region; HEADER requires declarative regions.
};

/** @brief Owning automatic-placement settings; counts are distinct from GridOptions track policies. */
struct DeclarativeGridOptions {
  std::optional<std::size_t> columns;  ///< Required for row-major flow; otherwise inferred within the budget.
  std::optional<std::size_t> rows;     ///< Required for column-major flow; otherwise inferred within the budget.
  GridFlow flow{GridFlow::ROW_MAJOR};  ///< Forward-only search; explicit rectangles are reserved first.
  std::int32_t columnGap{};            ///< Nonnegative terminal cells between columns.
  std::int32_t rowGap{};               ///< Nonnegative terminal cells between rows.
  LayoutPadding padding;               ///< Nonnegative content insets.
  std::size_t maxTracksPerAxis{4096};  ///< Positive bound on either resolved axis, including empty tracks.
  std::size_t maxPlacements{4096};     ///< Positive bound on structural children, including hidden children.
  std::size_t maxPlacementWork{65536}; ///< Positive total bound on candidates, occupancy probes and reservations.
  bool regions{};                      ///< Header/body have separate row occupancy and require explicit columns.
  bool stickyHeader{};                 ///< Header remains fixed vertically when scrolling is enabled.
};

/** @brief Resolved local origin in the same order as the input relationships. */
struct GridPosition {
  std::size_t row{};    ///< First occupied row.
  std::size_t column{}; ///< First occupied column.
};

/** @brief Owning preflight result independent of any tree identity or visibility. */
struct GridPlacementPlan {
  ~GridPlacementPlan() = default; ///< Keep checked-STL vector transfers fallible instead of implicit noexcept moves.
  std::size_t columns{};          ///< Complete horizontal track count.
  std::size_t rows{};             ///< Complete vertical track count, including explicit trailing tracks.
  std::vector<GridPosition> positions = std::vector<GridPosition>(0); ///< One origin per input item.
  std::size_t headerRows{}; ///< Inferred independent header rows; empty header has zero tracks.
};

/** @brief Resolve sparse placement without factories, tree access or retained input views.
 * @param options Counts, direction, gaps and finite work budgets, borrowed only for this call.
 * @param items Complete structural order, including hidden children; explicit rectangles reserve first.
 * @return Owning origins or INVALID_ARGUMENT for conflicts/bounds, NODE_LIMIT for budgets,
 * INVALID_GEOMETRY for arithmetic overflow, or NO_MEMORY. Automatic search never returns to skipped holes.
 */
std::expected<GridPlacementPlan, TreeError> resolveGridPlacements(const DeclarativeGridOptions &options,
                                                                  std::span<const GridItemOptions> items);

/** @brief Owning explicit placement, keyed by full identity rather than visible sibling index. */
struct GridCell {
  NodeId node;                ///< Owning identity; hidden/removed entries never bind a replacement node.
  std::size_t row{};          ///< Zero-based first row.
  std::size_t column{};       ///< Zero-based first column.
  std::size_t row_span{1};    ///< Positive span wholly within the explicit rows.
  std::size_t column_span{1}; ///< Positive span wholly within the explicit columns.
  LayoutAlign horizontal{LayoutAlign::STRETCH}; ///< Child alignment within its columns.
  LayoutAlign vertical{LayoutAlign::STRETCH};   ///< Child alignment within its rows.
  GridRegion region{GridRegion::BODY};          ///< Region-local row index; columns remain shared.
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
  std::vector<AxisSize> header_rows = std::vector<AxisSize>(0); ///< Independent header rows sharing columns.
  bool sticky_header{};          ///< Fix header vertically when this grid owns scrolling.
  bool regions{};                ///< Header/body presence fixed before mount, including empty regions.
  GridPresentation presentation; ///< Owning backgrounds; does not alter child styles.
};

/** @brief UI-owned Grid implementing the existing pure Layout contract.
 * Every visible child needs a placement. All spans are checked, including hidden/stale entries.
 * Later siblings paint/hit above earlier siblings; placement order does not change stacking.
 * Intrinsic tracks absorb child demand, then Row's allocation rules apply independently on each axis.
 * Constructors own settings; invalid settings are reported by Layout methods. Layout errors are
 * INVALID_ARGUMENT, NODE_LIMIT, INVALID_GEOMETRY or NO_MEMORY. No I/O, retained spans or tree traversal.
 */
class Grid : public SizedWidget, public Layout {
public:
  /** @brief Own settings and size policies.
   * @param options Tracks, sparse placements and limits copied into independent storage.
   * @param policy Size policy of this grid in its parent.
   * @throws std::bad_alloc If copying the configuration cannot allocate storage.
   */
  explicit Grid(const GridOptions &options = {}, SizePolicy policy = {});
  /** @brief Own resolved settings in a fixed construction-time placement mode.
   * @param options Complete resolved tracks and identities, copied into independent storage.
   * @param mode EXPLICIT permits overlap; DECLARATIVE rejects overlapping cell rectangles.
   * @param policy Size policy of this grid in its parent.
   * @param scroll Optional existing scroll owner, fixed before mounting; no additional widget is created.
   * @param width Existing terminal width policy used to validate scroll chrome glyphs.
   * @throws std::bad_alloc If the configuration cannot be copied.
   */
  Grid(const GridOptions &options, GridPlacementMode mode, SizePolicy policy = {},
       std::optional<GridScrollOptions> scroll = {}, WidthPolicy width = {});
  /** @brief Release owning caches without callbacks or terminal operations. */
  ~Grid() override;
  /** @brief Retire an optional scroll lease, pointer capture and intent before destruction. */
  void unmount() override;
  /** @param local Parent-local allocation, used only to derive owning regional geometry.
   * @return Success or checked geometry/controller/settings error; never publishes controller values.
   */
  std::expected<void, TreeError> arrange(CellRect local) override;
  /** @param available Normalized complete allocation.
   * @return Cached full content envelope inside frame, padding and owned scrollbar strips.
   */
  std::expected<CellRect, TreeError> childClip(CellSize available) const override;
  /** @param child Full identity of one measured direct child. @param available Complete allocation.
   * @return Cached owning regional clip/reveal/input policy, or an unchanged layout error.
   */
  std::expected<LayoutChildPolicy, TreeError> childLayoutFor(NodeId child, CellSize available) const override;
  /** @param context Synchronous sink borrowing grid-owned chrome text only through the enclosing frame.
   * @return Success, WOULD_BLOCK before submission above the whole paint budget, or sink/geometry error.
   */
  std::expected<void, TreeError> paint(PaintContext &context) override;
  /** @return Existing scroll defaults when enabled; backgrounds never alter child state styles. */
  ComputedStyle styleDefaults() const override;
  /** @brief Owning UI-thread copy; allocation may throw bad_alloc.
   * @return An independent copy of the complete sparse configuration.
   * @throws std::bad_alloc If the owning copy cannot be allocated.
   */
  GridOptions options() const;
  /** @return Whether this grid declared header/body regions before mount; no allocation or mutation. */
  bool usesRegions() const noexcept;
  /** @brief Checked inset bounds.
   * @param available Container extent offered by its parent, in cells.
   * @return Normalized inner extent after padding, or invalid settings/coordinate overflow.
   */
  std::expected<CellSize, TreeError> childAvailable(CellSize available) const override;
  /** @brief Aggregate tracks.
   * @param children Visible child measurements and policies, borrowed only for this calculation.
   * @return Aggregate intrinsic extent including gaps/padding, or a validation, geometry or allocation error.
   */
  std::expected<CellSize, TreeError> measureChildren(std::span<const MeasuredChild> children) const override;
  /** @brief Child-order local rectangles.
   * @param children Visible child measurements, policies and identities in sibling order.
   * @param available Container-local extent in terminal cells.
   * @return Owning rectangles in child order, or INVALID_ARGUMENT, NODE_LIMIT, INVALID_GEOMETRY or NO_MEMORY as
   * applicable.
   */
  std::expected<std::vector<CellRect>, TreeError> arrangeChildren(std::span<const MeasuredChild> children,
                                                                  CellSize available) const override;

private:
  friend std::expected<void, TreeError> setGridOptions(WidgetTree &, Node<Grid>, const GridOptions &);
  friend std::expected<void, TreeError> setDeclarativeGridOptions(WidgetTree &, Node<Grid>, const GridOptions &);
  friend std::expected<void, TreeError> setGridPresentation(WidgetTree &, Node<Grid>, const GridPresentation &);
  friend struct detail::ScrollWidgetAccess;
  GridOptions options_;
  GridPlacementMode mode_{GridPlacementMode::EXPLICIT};
  std::unique_ptr<detail::ScrollWidgetState> scroll_;
  mutable std::unique_ptr<detail::GridLayoutState> layout_;
};

/** @brief Owning overlay alignment and insets; all children share the content rectangle. */
struct OverlayOptions {
  LayoutPadding padding;                        ///< Insets retained during shortage.
  LayoutAlign horizontal{LayoutAlign::STRETCH}; ///< Alignment of each child across columns.
  LayoutAlign vertical{LayoutAlign::STRETCH};   ///< Alignment of each child across rows.
};

/** @brief UI-owned overlay using the existing traversal, geometry and sibling stacking.
 * Later siblings are above earlier siblings for normal paint order and EventRouter hit testing.
 * Layout methods are pure and follow Layout's borrowed-span contract. Invalid settings/policies
 * return INVALID_ARGUMENT, checked overflow INVALID_GEOMETRY, allocation failure NO_MEMORY.
 */
class Overlay : public SizedWidget, public Layout {
public:
  /** @brief Own settings and size policies.
   * @param options Owning padding and alignment applied independently to every child.
   * @param policy Size policy of this overlay in its parent.
   */
  explicit Overlay(OverlayOptions options = {}, SizePolicy policy = {});
  /** @brief Owning UI-thread copy.
   * @return A value copy of the current padding and alignment.
   */
  OverlayOptions options() const;
  /** @brief Checked inset bounds.
   * @param available Container extent offered by its parent, in cells.
   * @return Normalized inner extent after padding, or invalid settings/coordinate overflow.
   */
  std::expected<CellSize, TreeError> childAvailable(CellSize available) const override;
  /** @brief Maximum child extent on each axis.
   * @param children Visible child measurements and policies, borrowed only for this calculation.
   * @return Aggregate intrinsic extent including gaps/padding, or a validation, geometry or allocation error.
   */
  std::expected<CellSize, TreeError> measureChildren(std::span<const MeasuredChild> children) const override;
  /** @brief Child-order local rectangles.
   * @param children Visible child measurements, policies and identities in sibling order.
   * @param available Container-local extent in terminal cells.
   * @return Owning rectangles in child order, or INVALID_ARGUMENT, NODE_LIMIT, INVALID_GEOMETRY or NO_MEMORY as
   * applicable.
   */
  std::expected<std::vector<CellRect>, TreeError> arrangeChildren(std::span<const MeasuredChild> children,
                                                                  CellSize available) const override;

private:
  friend std::expected<void, TreeError> setOverlayOptions(WidgetTree &, Node<Overlay>, OverlayOptions);
  OverlayOptions options_;
};

/** @brief Validate and atomically copy settings on the UI owner between frames, then invalidate layout.
 * Tree errors propagate; invalid configuration leaves the previous settings unchanged.
 * Input is borrowed only for this call. Allocation failure returns NO_MEMORY.
 * @param tree Owning tree, borrowed for the UI-only update.
 * @param node Checked observer of the grid being updated.
 * @param options Complete replacement configuration; borrowed only while making an independent copy.
 * @return Success after replacement and layout invalidation, or a validation/access/allocation error.
 */
std::expected<void, TreeError> setGridOptions(WidgetTree &tree, Node<Grid> node, const GridOptions &options);
/** @brief Atomically replace resolved declarative settings between frames on the UI owner.
 * @param tree Owning tree, borrowed for checked access and layout invalidation.
 * @param node Declarative grid; an explicit grid returns INVALID_ARGUMENT without changing mode.
 * @param options Complete owning replacement; overlap is rejected and full identities remain authoritative.
 * @return Success, or validation/access/allocation error with old settings retained before publication.
 * Input is borrowed only during this call. Layout callbacks cannot call this setter.
 */
std::expected<void, TreeError> setDeclarativeGridOptions(WidgetTree &tree, Node<Grid> node, const GridOptions &options);
/** @brief Replace owning backgrounds between frames on the UI owner.
 * @param tree Owning tree borrowed for this checked update. @param node Grid receiving backgrounds.
 * @param presentation Complete replacement with positive paint budget; borrowed only during this call.
 * @return Success or access/validation error; equal values request no extra paint.
 */
std::expected<void, TreeError> setGridPresentation(WidgetTree &tree, Node<Grid> node,
                                                   const GridPresentation &presentation);
/** @brief Validate and replace settings on the UI owner between frames, then invalidate layout.
 * @param tree Owning tree, borrowed for the UI-only update.
 * @param node Checked observer of the overlay being updated.
 * @param options Replacement padding and alignment, copied by value.
 * @return Success after replacement and layout invalidation, or a validation/access error with old settings retained.
 */
std::expected<void, TreeError> setOverlayOptions(WidgetTree &tree, Node<Overlay> node, OverlayOptions options);
} // namespace portals
