// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <limits>
#include <portals/widget.hpp>
#include <span>

namespace portals {
/** @brief Main-axis allocation policy, in integer terminal cells. */
enum class SizeMode {
  automatic,
  fixed,
  flex
};
/** @brief Sentinel requesting the widget's intrinsic measurement. */
inline constexpr std::int32_t intrinsic_extent = -1;
/** @brief Owning per-axis bounds; preferred=-1 uses intrinsic measurement. */
struct AxisSize {
  std::int32_t minimum{};                                         ///< Nonnegative hard minimum.
  std::int32_t preferred{intrinsic_extent};                       ///< Intrinsic when -1, otherwise requested size.
  std::int32_t maximum{std::numeric_limits<std::int32_t>::max()}; ///< Inclusive cap.
  SizeMode mode{SizeMode::automatic};                             ///< Fixed does not shrink; flex shares spare cells.
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
  virtual ~LayoutItem() = default;            ///< Destroy with the implementing widget.
  virtual SizePolicy size_policy() const = 0; ///< Return an owning policy; no tree/model mutation or I/O.
};
/** @brief Widget with owning policies, changed only through set_size_policy on UI. */
class SizedWidget : public Widget, public LayoutItem {
public:
  explicit SizedWidget(SizePolicy policy = {}); ///< Own the supplied policies; checked by layout.
  SizePolicy size_policy() const override;      ///< Copy current policy without borrowing storage.
private:
  friend std::expected<void, TreeError> set_size_policy(WidgetTree &, Node<SizedWidget>, SizePolicy);
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
  virtual std::expected<CellSize, TreeError> child_available(CellSize available) const = 0; ///< Measurement bounds.
  virtual std::expected<CellSize, TreeError>
  measure_children(std::span<const MeasuredChild> children) const = 0; ///< Own aggregate.
  virtual std::expected<std::vector<CellRect>, TreeError>
  arrange_children(std::span<const MeasuredChild> children,
                   CellSize available) const = 0; ///< Own parent-local rectangles in child order.
};
/** @brief Alignment; center assigns floor(free/2) before the child/group. */
enum class LayoutAlign {
  start,
  center,
  end,
  stretch
};
/** @brief Minimum/fixed sizes survive shortage and are clipped by ancestor geometry. */
enum class LayoutOverflow {
  clip
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
  LayoutAlign main_align{LayoutAlign::start};    ///< Group alignment after allocation; stretch is start on this axis.
  LayoutAlign cross_align{LayoutAlign::stretch}; ///< Child alignment across the linear direction.
  LayoutOverflow overflow{LayoutOverflow::clip}; ///< Explicit shortage policy.
};
/** @brief Shared Row/Column implementation; UI-owned and mutated via set_linear_options. */
class LinearLayout : public SizedWidget, public Layout {
public:
  std::expected<CellSize, TreeError> child_available(CellSize available) const override; ///< Checked inset extent.
  std::expected<CellSize, TreeError>
  measure_children(std::span<const MeasuredChild> children) const override; ///< Checked sum/max.
  std::expected<std::vector<CellRect>, TreeError>
  arrange_children(std::span<const MeasuredChild> children,
                   CellSize available) const override; ///< Deterministic bounded solve.
  LinearOptions options() const;                       ///< Owning copy; no invalidation.
protected:
  LinearLayout(bool horizontal, LinearOptions options, SizePolicy policy); ///< Initialize axis and owning settings.
private:
  friend std::expected<void, TreeError> set_linear_options(WidgetTree &, Node<LinearLayout>, LinearOptions);
  bool horizontal_;
  LinearOptions options_;
};
/** @brief Lay visible children from left to right. */
class Row : public LinearLayout {
public:
  explicit Row(LinearOptions options = {}, SizePolicy policy = {}); ///< Own settings and policies.
};
/** @brief Lay visible children from top to bottom. */
class Column : public LinearLayout {
public:
  explicit Column(LinearOptions options = {}, SizePolicy policy = {}); ///< Own settings and policies.
};
/** @brief Validate, edit and invalidate layout between frames on the tree's UI owner; tree errors propagate. */
std::expected<void, TreeError> set_size_policy(WidgetTree &tree, Node<SizedWidget> node, SizePolicy policy);
/** @brief Validate, edit and invalidate layout between frames on the tree's UI owner; tree errors propagate. */
std::expected<void, TreeError> set_linear_options(WidgetTree &tree, Node<LinearLayout> node, LinearOptions options);
/** @brief External two-phase traversal; call inside the frame that also paints/renders.
 * Tree owns nodes; all inspect/policy/measurement/placement snapshots are local owning values.
 * Call invalidate(layout) between frames before changed geometry. On any failure abort use of this pass.
 * Hidden subtrees are skipped, enabled has no effect. Plain parents allow at most one child (viewport passthrough).
 * Errors propagate as TreeError; arithmetic failures are invalid_geometry, allocation failures no_memory.
 * Reentrant passes return would_block. No implicit frame, I/O, model edits or retained secondary tree.
 */
std::expected<void, TreeError> layout_tree(WidgetTree &tree, Node<Widget> node, CellRect local, CellRect viewport);
} // namespace portals