// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <limits>
#include <portals/widget.hpp>
#include <span>

namespace portals {
/** @brief Main-axis allocation policy, in integer terminal cells. */
enum class SizeMode {
  AUTOMATIC,
  FIXED,
  FLEX
};
/** @brief Sentinel requesting the widget's intrinsic measurement. */
inline constexpr std::int32_t INTRINSIC_EXTENT = -1;

/** @brief Owning per-axis bounds; preferred=-1 uses intrinsic measurement. */
struct AxisSize {
  std::int32_t minimum{};                                         ///< Nonnegative hard minimum.
  std::int32_t preferred{INTRINSIC_EXTENT};                       ///< Intrinsic when -1, otherwise requested size.
  std::int32_t maximum{std::numeric_limits<std::int32_t>::max()}; ///< Inclusive cap.
  SizeMode mode{SizeMode::AUTOMATIC};                             ///< Fixed does not shrink; flex shares spare cells.
  std::uint32_t weight{1};                                        ///< Positive flex weight.
};

/** @brief Owning independent policies for width and height. */
struct SizePolicy {
  AxisSize width;  ///< Horizontal policy.
  AxisSize height; ///< Vertical policy.
};

/** @brief Optional const sizing interface, read only within tree.read. */
class LayoutItem {
public:
  virtual ~LayoutItem() = default; ///< Destroy with the implementing widget.
  /** @brief Return an owning policy; no tree/model mutation or I/O.
   * @return Independent width/height policies to validate during layout; no widget reference escapes.
   */
  virtual SizePolicy sizePolicy() const = 0;
};

/** @brief Widget with owning policies, changed only through setSizePolicy on UI. */
class SizedWidget : public Widget, public LayoutItem {
public:
  /** @brief Own the supplied policies; checked by layout.
   * @param policy Owning width/height constraints; layout validates them before placement.
   */
  explicit SizedWidget(SizePolicy policy = {});
  /** @brief Copy current policy without borrowing storage.
   * @return A value copy of the current width/height constraints; obtain it only inside tree.read.
   */
  SizePolicy sizePolicy() const override;

private:
  friend std::expected<void, TreeError> setSizePolicy(WidgetTree &, Node<SizedWidget>, SizePolicy);
  SizePolicy policy_;
};

/** @brief Owning child input for a pure layout computation. */
struct MeasuredChild {
  CellSize desired;  ///< Intrinsic or aggregated nonnegative measurement.
  SizePolicy policy; ///< Copied policies; no widget references.
  NodeId node{};     ///< Stable owning identity, including when siblings are hidden.
};

/** @brief Pure container interface; callbacks run inside tree.read and an enclosing tree.frame.
 * No callback may call tree operations, recurse into layout, mutate models, retain spans, or perform I/O.
 * Independent algorithms for Grid/Overlay can implement this interface without changing Widget.
 */
class Layout {
public:
  virtual ~Layout() = default; ///< Destroy with the implementing widget.
  /** @brief Measurement bounds.
   * @param available Normalized cell extent offered to this container.
   * @return Bounds to pass to child measurement, or a validation/arithmetic failure.
   */
  virtual std::expected<CellSize, TreeError> childAvailable(CellSize available) const = 0;
  /** @brief Own aggregate.
   * @param children Borrowed visible children in sibling order, including stable identities and copied policies.
   * @return The owning aggregate intrinsic extent, or a layout error.
   */
  virtual std::expected<CellSize, TreeError> measureChildren(std::span<const MeasuredChild> children) const = 0;
  /** @brief Own parent-local rectangles in child order.
   * @param children Borrowed measured children in the same order as the resulting rectangles.
   * @param available Container-local extent available for arrangement.
   * @return One parent-local rectangle per child, or validation, arithmetic or allocation failure.
   */
  virtual std::expected<std::vector<CellRect>, TreeError> arrangeChildren(std::span<const MeasuredChild> children,
                                                                          CellSize available) const = 0;
};
/** @brief Alignment; center assigns floor(free/2) before the child/group. */
enum class LayoutAlign {
  START,
  CENTER,
  END,
  STRETCH
};
/** @brief Minimum/fixed sizes survive shortage and are clipped by ancestor geometry. */
enum class LayoutOverflow {
  CLIP
};

/** @brief Owning nonnegative padding in terminal cells. */
struct LayoutPadding {
  std::int32_t left{};   ///< Leading columns.
  std::int32_t top{};    ///< Leading rows.
  std::int32_t right{};  ///< Trailing columns.
  std::int32_t bottom{}; ///< Trailing rows.
};

/** @brief Owning linear container options. */
struct LinearOptions {
  std::int32_t gap{};                            ///< Cells between visible children only.
  LayoutPadding padding;                         ///< Insets, retained during shortage.
  LayoutAlign main_align{LayoutAlign::START};    ///< Group alignment after allocation; stretch is start on this axis.
  LayoutAlign cross_align{LayoutAlign::STRETCH}; ///< Child alignment across the linear direction.
  LayoutOverflow overflow{LayoutOverflow::CLIP}; ///< Explicit shortage policy.
};

/** @brief Shared Row/Column implementation; UI-owned and mutated via setLinearOptions. */
class LinearLayout : public SizedWidget, public Layout {
public:
  /** @brief Checked inset extent.
   * @param available Container extent offered by its parent, in cells.
   * @return Normalized inner extent after padding, or invalid settings/coordinate overflow.
   */
  std::expected<CellSize, TreeError> childAvailable(CellSize available) const override;
  /** @brief Checked sum/max.
   * @param children Visible child measurements and policies, borrowed only for this calculation.
   * @return Aggregate intrinsic extent including gaps/padding, or a validation, geometry or allocation error.
   */
  std::expected<CellSize, TreeError> measureChildren(std::span<const MeasuredChild> children) const override;
  /** @brief Deterministic bounded solve.
   * @param children Visible child measurements, policies and identities in sibling order.
   * @param available Container-local extent in terminal cells.
   * @return Owning rectangles in child order, or INVALID_ARGUMENT, NODE_LIMIT, INVALID_GEOMETRY or NO_MEMORY as
   * applicable.
   */
  std::expected<std::vector<CellRect>, TreeError> arrangeChildren(std::span<const MeasuredChild> children,
                                                                  CellSize available) const override;
  /** @brief Owning copy; no invalidation.
   * @return A value copy of gaps, padding, alignment and overflow policy.
   */
  LinearOptions options() const;

protected:
  /** @brief Initialize axis and owning settings.
   * @param horizontal True for a row, false for a column.
   * @param options Owning gaps, insets and alignment settings.
   * @param policy Owning constraints for this container within its parent.
   */
  LinearLayout(bool horizontal, LinearOptions options, SizePolicy policy);

private:
  friend std::expected<void, TreeError> setLinearOptions(WidgetTree &, Node<LinearLayout>, LinearOptions);
  bool horizontal_;
  LinearOptions options_;
};

/** @brief Lay visible children from left to right. */
class Row : public LinearLayout {
public:
  /** @brief Own settings and policies.
   * @param options Owning gap, padding and alignment settings, validated by layout.
   * @param policy Owning size constraints for the container itself.
   */
  explicit Row(LinearOptions options = {}, SizePolicy policy = {});
};

/** @brief Lay visible children from top to bottom. */
class Column : public LinearLayout {
public:
  /** @brief Own settings and policies.
   * @param options Owning gap, padding and alignment settings, validated by layout.
   * @param policy Owning size constraints for the container itself.
   */
  explicit Column(LinearOptions options = {}, SizePolicy policy = {});
};

/** @brief Validate, edit and invalidate layout between frames on the tree's UI owner; tree errors propagate.
 * @param tree Owning UI tree, borrowed for this call.
 * @param node Checked observer of the SizedWidget to update.
 * @param policy Replacement width/height constraints; invalid values are rejected before mutation.
 * @return Success after requesting layout, or the validation/access/invalidation error.
 */
std::expected<void, TreeError> setSizePolicy(WidgetTree &tree, Node<SizedWidget> node, SizePolicy policy);
/** @brief Validate, edit and invalidate layout between frames on the tree's UI owner; tree errors propagate.
 * @param tree Owning UI tree, borrowed for this call.
 * @param node Checked observer of the Row or Column to update.
 * @param options Replacement gap, padding and alignment settings.
 * @return Success after requesting layout, or an error without replacing the previous settings.
 */
std::expected<void, TreeError> setLinearOptions(WidgetTree &tree, Node<LinearLayout> node, LinearOptions options);
/** @brief External two-phase traversal; call inside the frame that also paints/renders.
 * Tree owns nodes; all inspect/policy/measurement/placement snapshots are local owning values.
 * Call invalidate(layout) between frames before changed geometry. On any failure abort use of this pass.
 * Hidden subtrees are skipped, enabled has no effect. Plain parents allow at most one child (viewport passthrough).
 * Errors propagate as TreeError; arithmetic failures are INVALID_GEOMETRY, allocation failures NO_MEMORY.
 * Reentrant passes return WOULD_BLOCK. No implicit frame, I/O, model edits or retained secondary tree.
 * @param tree Tree borrowed on its UI owner inside an active frame.
 * @param node Root of the subtree to measure and arrange.
 * @param local Root placement relative to its parent; descendants receive parent-local rectangles.
 * @param viewport Surface-cell clip applied to the whole pass.
 * @return Success when all participating geometry is valid, or an error requiring the caller to discard this pass.
 */
std::expected<void, TreeError> layoutTree(WidgetTree &tree, Node<Widget> node, CellRect local, CellRect viewport);
} // namespace portals