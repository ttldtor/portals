// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/action.hpp>
#include <portals/scroll_controller.hpp>
#include <portals/scroll_options.hpp>

namespace portals {
namespace detail {
struct ScrollWidgetState;
struct ScrollWidgetAccess;
} // namespace detail

/** @brief Owning observation captured after a complete low-level layout pass.
 * Capture every area's observation before preparation invalidates geometry. The future managed
 * snapshot adapter performs this automatically; this value itself retains no tree or widget.
 */
struct ScrollAreaLayoutSnapshot {
  NodeId node;                    ///< Full source identity, including session and generation.
  NodeGeometry geometry;          ///< Copied arranged outer geometry; invalid for a hidden owner.
  ScrollExtent extent;            ///< Intrinsic ordinary content extent, bounded by int32 layout.
  ScrollAreaGeometry placement;   ///< Copied local viewport, clip and one-cell tracks.
  std::uint64_t layoutRevision{}; ///< Local completed-layout identity; stale observations cannot be adopted.
  bool visible{};                 ///< Effective visibility, distinct from a visible zero allocation.
  ScrollOffset horizontalOffset;  ///< Old effective local position used by this completed layout.
  ScrollOffset verticalOffset;    ///< Old effective position independent of later controller commands.
};

/** @brief Retained ordinary content owner with optional one-cell frame and embedded bars.
 * Add one content root (a Row/Column/Grid for multiple controls). This is not virtualization.
 * Initialize between frames before commands/preparation/binding. Before initialization paint is
 * safe and empty; checked commands fail with INVALID_ARGUMENT. No callbacks or publication occur
 * in layout or paint. Construct/mutate on the tree's UI thread; tree owns final widget destruction.
 */
class ScrollArea : public SizedWidget, public Layout {
public:
  /** @param controller Owning shared values; empty creates a default controller on this UI thread.
   * @param width Complete owning text policy for all glyphs.
   * @param options Owning placement/input settings, validated by initialization and measurement.
   * @param style Construction-only defaults beneath the existing theme resolver.
   * @param policy Owning layout constraints, validated by the layout solver.
   * @throws std::bad_alloc On internal allocation; tree emplacement translates it to NO_MEMORY.
   * Other text-policy constructor errors use the existing tree emplacement exception boundary.
   */
  explicit ScrollArea(std::shared_ptr<ScrollController> controller = {}, WidthPolicy width = {},
                      ScrollAreaOptions options = {}, ControlStyle style = {}, SizePolicy policy = {});
  /** @brief Construct an area with an owning advanced frame, compiled before initialization.
   * @param controller Owning shared values; empty creates a default controller on this UI thread.
   * @param width Complete owning text policy for frame and scrollbar glyphs.
   * @param options Owning placement/input settings; its legacy frame is superseded by frame.
   * @param style Construction-only defaults beneath the existing theme resolver.
   * @param policy Owning layout constraints, validated by the layout solver.
   * @param frame Owning frame settings; retained text is independent of caller storage.
   * @param advanced Required construction tag keeping legacy brace calls unambiguous.
   * @throws std::bad_alloc On allocation; tree emplacement translates it to NO_MEMORY.
   * Semantic frame errors are retained and returned by initialization and measurement before a lease is acquired.
   */
  ScrollArea(std::shared_ptr<ScrollController> controller, WidthPolicy width, ScrollAreaOptions options,
             ControlStyle style, SizePolicy policy, FrameOptions frame, std::in_place_t advanced);
  /** @brief Release owning values; no user callback or terminal operation occurs. */
  ~ScrollArea() override;
  /** @brief Retire this node's lease and pointer token before destruction. */
  void unmount() override;
  /** @param available Offered allocation, not an intrinsic content limit.
   * @return Empty/frame minimum, or invalid settings/controller affinity error.
   */
  std::expected<CellSize, TreeError> measure(CellSize available) override;
  /** @param available Offered outer extent, normalized for descendant measurement.
   * @return Offered extent minus optional frame inset, or invalid settings.
   */
  std::expected<CellSize, TreeError> childAvailable(CellSize available) const override;
  /** @param children Visible content roots; at most one is permitted.
   * @return Intrinsic content plus frame inset, or INVALID_ARGUMENT/INVALID_GEOMETRY.
   */
  std::expected<CellSize, TreeError> measureChildren(std::span<const MeasuredChild> children) const override;
  /** @param local Parent-local allocation; only the normalized extent is cached.
   * @return Success or unchanged settings/controller/geometry error.
   */
  std::expected<void, TreeError> arrange(CellRect local) override;
  /** @param available Normalized allocated extent.
   * @return Viewport clip excluding owned bar cells, or a checked geometry error.
   */
  std::expected<CellRect, TreeError> childClip(CellSize available) const override;
  /** @param children Visible single content root, borrowed only during placement.
   * @param available Normalized outer allocation.
   * @return Full intrinsic content rectangle translated by the locally clamped raster offset.
   * Returns INVALID_ARGUMENT for multiple roots or INVALID_GEOMETRY on overflow.
   */
  std::expected<std::vector<CellRect>, TreeError> arrangeChildren(std::span<const MeasuredChild> children,
                                                                  CellSize available) const override;
  /** @param context Synchronous sink borrowing widget-owned text through the enclosing frame.
   * @return Success, WOULD_BLOCK before submission above the whole paint budget, or sink error.
   * Sink exceptions use WidgetTree::paint's existing exception boundary.
   */
  std::expected<void, TreeError> paint(PaintContext &context) override;
  /** @return Owning manual normal/focused/disabled defaults beneath theme declarations. */
  ComputedStyle styleDefaults() const override;

private:
  friend struct detail::ScrollWidgetAccess;
  std::unique_ptr<detail::ScrollWidgetState> state_;
};

/** @brief Independent one-axis projection of an owning controller, without a content offset.
 * Initialization chooses a sole standalone metrics writer or an area-owned read projection.
 * Before initialization paint is empty and checked commands fail with INVALID_ARGUMENT.
 * No timers, virtual-view adapters, tree ownership or observer runtime are retained.
 */
class ScrollBar : public SizedWidget {
public:
  /** @param controller Shared values; empty creates a default controller on this UI thread.
   * @param axis HORIZONTAL or VERTICAL; invalid values fail initialization/measurement.
   * @param width Complete owning glyph policy.
   * @param options Owning input/paint settings.
   * @param style Construction-only defaults beneath theme declarations.
   * @param policy Owning layout constraints.
   * @throws std::bad_alloc On allocation; tree emplacement translates internal exceptions.
   */
  explicit ScrollBar(std::shared_ptr<ScrollController> controller = {}, ScrollAxis axis = ScrollAxis::VERTICAL,
                     WidthPolicy width = {}, ScrollBarOptions options = {}, ControlStyle style = {},
                     SizePolicy policy = {});
  /** @brief Release owning values without user callbacks or terminal operations. */
  ~ScrollBar() override;
  /** @brief Release this node's lease and pointer token. */
  void unmount() override;
  /** @param available Offered extent; the intrinsic bar occupies one cell across its axis.
   * @return One-cell intrinsic size or invalid settings/controller error.
   */
  std::expected<CellSize, TreeError> measure(CellSize available) override;
  /** @param local Parent-local allocation; caches zero-origin track geometry and owning values.
   * @return Success or checked settings/controller/geometry error.
   */
  std::expected<void, TreeError> arrange(CellRect local) override;
  /** @param context Synchronous sink borrowing widget-owned glyphs through the frame.
   * @return Success, WOULD_BLOCK above budget before submission, or sink error.
   */
  std::expected<void, TreeError> paint(PaintContext &context) override;
  /** @return Owning defaults beneath theme declarations. */
  ComputedStyle styleDefaults() const override;

private:
  friend struct detail::ScrollWidgetAccess;
  std::unique_ptr<detail::ScrollWidgetState> state_;
};

/** @param tree Borrowed UI owner, between frames.
 * @param node Live area; repeated initialization preserves its existing lease and position.
 * @return Success, existing tree/controller error, INVALID_ARGUMENT for a second metrics writer,
 * or NO_MEMORY without partial initialization. No input binding or content node is created.
 */
std::expected<void, TreeError> initializeScrollArea(WidgetTree &tree, Node<ScrollArea> node);
/** @param tree Borrowed UI owner, between frames.
 * @param node Live standalone projection.
 * @param geometryWriter True for sole standalone metrics ownership; false projects an area.
 * @return Success or existing tree/controller error; repeating a different writer role is INVALID_ARGUMENT.
 */
std::expected<void, TreeError> initializeScrollBar(WidgetTree &tree, Node<ScrollBar> node, bool geometryWriter = true);
/** @param tree Borrowed UI owner; capture after successful complete layout, before invalidation.
 * @param node Live initialized area.
 * @return Owning observation, including effective hidden state; or checked tree/geometry error.
 * Does not publish controller values or invalidate. This is the explicit low-level capture path.
 */
std::expected<ScrollAreaLayoutSnapshot, TreeError> scrollAreaLayout(WidgetTree &tree, Node<ScrollArea> node);
/** @param tree Borrowed UI owner, between frames.
 * @param node Initialized area that originally produced layout.
 * @param layout Owning observation from one completed pass; capture all sibling observations first.
 * @return Success or existing checked error. Hidden areas preserve values; visible zero clamps to zero.
 * Adopts metrics atomically, cancels drag on track/metrics changes and invalidates LAYOUT if needed.
 * This explicit path is replaced by automatic managed preparation in a later integration slice.
 */
std::expected<void, TreeError> prepareScrollArea(WidgetTree &tree, Node<ScrollArea> node,
                                                 const ScrollAreaLayoutSnapshot &layout);
/** @param tree Borrowed UI owner, between frames.
 * @param node Initialized area or bar of this tree.
 * @param axis Exact selected axis; invalid values are INVALID_ARGUMENT.
 * @param offset Exact integral/fractional intended position, clamped to the current range.
 * @return Success or checked error. Read-only user policy does not restrict this command.
 * Fraction-only changes do not request a frame; raster changes invalidate tree-wide LAYOUT.
 */
std::expected<void, TreeError> setScrollOffset(WidgetTree &tree, Node<Widget> node, ScrollAxis axis,
                                               ScrollOffset offset);
/** @param tree Borrowed UI owner, between frames.
 * @param node Initialized bar holding the sole standalone geometry writer lease.
 * @param metrics Owning exact extent/viewport for this bar's axis; the other axis is preserved.
 * @return Success or checked error; a non-writer projection returns INVALID_ARGUMENT.
 */
std::expected<void, TreeError> setScrollMetrics(WidgetTree &tree, Node<ScrollBar> node, ScrollAxisMetrics metrics);
/** @param tree Borrowed UI owner, between frames.
 * @param node Live area or bar.
 * @param focused Current checked keyboard focus, or empty; copied for paint only.
 * @return Success or tree error; equal presentation flags do not invalidate.
 */
std::expected<void, TreeError> syncScrollControl(WidgetTree &tree, Node<Widget> node, Node<Widget> focused = {});
/** @param router Existing router that must outlive the binding and its admitted calls.
 * @param tree Same borrowed tree as router; UI-only, outside dispatch/frame.
 * @param node Live area; initializes its lease if needed.
 * @return Scoped binding or typed initialization/router error. A failed bind releases only the
 * lease acquired by this call. No native capability or mouse mode is enabled.
 */
std::expected<ActionBinding, TreeError> bindScrollArea(EventRouter &router, WidgetTree &tree, Node<ScrollArea> node);
/** @param router Existing router, outliving the binding/admitted calls.
 * @param tree Same UI tree as router.
 * @param node Live standalone bar.
 * @param geometryWriter Initialization role; false shares an area's controller metrics.
 * @return Scoped binding or typed error, with the same rollback and capability rules as bindScrollArea.
 */
std::expected<ActionBinding, TreeError> bindScrollBar(EventRouter &router, WidgetTree &tree, Node<ScrollBar> node,
                                                      bool geometryWriter = true);
} // namespace portals
