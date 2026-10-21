// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/controls.hpp>

#include <array>

// Spell the existing decoration enum without changing the caller's native Unicode-selection macro.
#pragma push_macro("UNICODE")
#undef UNICODE

namespace portals {
/** @brief One-cell decoration selection under the complete receiving WidthPolicy. */
enum class PrimitiveGlyphs {
  UNICODE, ///< Unicode lines/blocks, falling back to ASCII when a glyph is not one cell.
  ASCII    ///< Portable one-cell ASCII decorations.
};

/** @brief Owning frame decoration and finite paint-work policy. */
struct BorderOptions {
  PrimitiveGlyphs glyphs{PrimitiveGlyphs::UNICODE}; ///< Owning glyph selection; no font probing.
  std::size_t maxPaintCells{16384};                 ///< Positive cap on allocated perimeter cells, even when clipped.
};

/** @brief Owning strictly determinate gauge settings. */
struct GaugeOptions {
  double minimum{};                                 ///< Inclusive finite lower bound.
  double maximum{1};                                ///< Finite upper bound, strictly greater than minimum.
  std::int32_t preferredWidth{10};                  ///< Nonnegative intrinsic width in cells; height is one.
  PrimitiveGlyphs glyphs{PrimitiveGlyphs::UNICODE}; ///< Block or ASCII fill, with one-cell fallback.
  std::size_t maxPaintCells{16384};                 ///< Positive cap on allocated area cells, even when clipped.
};

/** @brief One-cell frame using the existing vertical LinearLayout with one-cell insets.
 * Paint borrows only widget-owned glyphs until the enclosing tree.frame ends. The inner
 * child clip prevents fixed/minimum-size children from painting or receiving input on edges.
 * Manual style defaults are construction-only; dynamic styling uses the existing style resolver.
 * No I/O, timer, retained command storage or allocation occurs inside paint itself.
 */
class Border : public SizedWidget, public Layout {
public:
  /** @param width Complete owning text policy, shared with the receiving frame.
   * @param options Owning glyph mode and finite paint cap; checked by measure/paint.
   * @param style Construction-only owning manual normal/focused/disabled defaults beneath theme rules.
   * @param policy Owning size constraints, checked by the existing layout solver.
   * Construction may allocate or throw for an invalid width policy; tree emplacement translates exceptions.
   */
  explicit Border(WidthPolicy width = {}, BorderOptions options = {}, ControlStyle style = {}, SizePolicy policy = {});
  /** @param available Offered extent; the intrinsic empty frame remains two by two cells.
   * @return Two-cell decoration size, or INVALID_ARGUMENT for invalid options.
   */
  std::expected<CellSize, TreeError> measure(CellSize available) override;
  /** @param available Allocated outer extent offered by the parent, in cells.
   * @return Existing Column inset calculation with fixed one-cell padding, or unchanged layout error.
   */
  std::expected<CellSize, TreeError> childAvailable(CellSize available) const override;
  /** @param children Visible child measurements and policies, borrowed only during placement.
   * @param available Allocated outer extent in cells.
   * @return Existing Column placements with fixed one-cell insets, or unchanged layout error.
   */
  std::expected<std::vector<CellRect>, TreeError> arrangeChildren(std::span<const MeasuredChild> children,
                                                                  CellSize available) const override;

  /** @param children Visible child measurements and policies, borrowed only for this calculation.
   * @return Existing linear aggregate plus the resolved theme minimum width, or unchanged layout error.
   */
  std::expected<CellSize, TreeError> measureChildren(std::span<const MeasuredChild> children) const override;

  /** @param local Parent-local allocation; only its extent is retained.
   * @return Success, or INVALID_GEOMETRY for an unrepresentable local extent.
   */
  std::expected<void, TreeError> arrange(CellRect local) override;
  /** @param available Allocated normalized outer extent in cells.
   * @return Container-local inner clip, possibly empty at zero/one/two-cell extents.
   */
  std::expected<CellRect, TreeError> childClip(CellSize available) const override;
  /** @param context Synchronous sink; borrowed glyph text remains widget-owned through this frame.
   * @return Success, INVALID_ARGUMENT, WOULD_BLOCK above maxPaintCells before submission, or unchanged sink error.
   * Sink exceptions propagate to the existing WidgetTree::paint boundary; discard a failed frame.
   */
  std::expected<void, TreeError> paint(PaintContext &context) override;
  /** @return Owning manual appearances beneath the existing style resolver. */
  ComputedStyle styleDefaults() const override;
  /** @param options Owning settings to validate without allocation or mutation.
   * @return Success for a recognized glyph mode and positive cap, otherwise INVALID_ARGUMENT.
   */
  static std::expected<void, TreeError> validate(BorderOptions options);

private:
  friend std::expected<void, TreeError> syncBorder(WidgetTree &, Node<Border>);
  Column layout_;
  BorderOptions options_;
  ControlStyle style_;
  std::array<GraphemeText, 6> glyphs_;
  CellRect bounds_;
  bool enabled_{true};
};

/** @brief Noninteractive horizontal determinate gauge with widget-owned fill text.
 * Paint has no owning allocation or polling; text remains alive until the enclosing frame ends.
 * Value mutations are UI-only and use checked tree operations or existing onPrepare callbacks.
 * Manual style defaults are construction-only; dynamic styling uses the existing style resolver.
 */
class Gauge : public SizedWidget {
public:
  /** @param value Initial finite value in the inclusive configured range; measure validates it.
   * @param width Complete owning text policy, shared with the receiving frame.
   * @param options Owning bounds, preferred width, glyph mode and finite paint cap.
   * @param style Construction-only owning manual defaults beneath theme declarations.
   * @param policy Owning layout constraints, checked by the existing solver.
   * Construction may allocate or throw for an invalid width policy; tree emplacement translates exceptions.
   */
  explicit Gauge(double value, WidthPolicy width = {}, GaugeOptions options = {}, ControlStyle style = {},
                 SizePolicy policy = {});
  /** @param available Offered extent; intrinsic width comes from GaugeOptions.
   * @return Preferred width and height one, or INVALID_ARGUMENT for invalid value/settings.
   */
  std::expected<CellSize, TreeError> measure(CellSize available) override;
  /** @param local Parent-local allocation; only its extent is retained.
   * @return Success, or INVALID_GEOMETRY for an unrepresentable local extent.
   */
  std::expected<void, TreeError> arrange(CellRect local) override;
  /** @param context Synchronous sink borrowing widget-owned glyph text through this frame.
   * @return Success, INVALID_ARGUMENT, WOULD_BLOCK above the allocated-area cap before submit, or unchanged sink error.
   * Sink exceptions propagate to the existing WidgetTree::paint boundary; discard a failed frame.
   */
  std::expected<void, TreeError> paint(PaintContext &context) override;
  /** @return Owning manual appearances beneath the existing style resolver. */
  ComputedStyle styleDefaults() const override;
  /** @return Current finite presentation value; borrow the widget only inside a checked tree callback. */
  double value() const noexcept;
  /** @param value Finite replacement in the inclusive range; equal values are a no-op.
   * @return Success, or INVALID_ARGUMENT without mutation. This snapshot helper does not invalidate;
   * use setGaugeValue between frames, or call display from an already dirty onPrepare callback.
   */
  std::expected<void, TreeError> display(double value);
  /** @param value Finite value checked against the owning settings.
   * @param options Finite bounds with a representable positive span, nonnegative width and positive paint cap.
   * @return Success, or INVALID_ARGUMENT for NaN/infinity, invalid mode, bounds, cap or out-of-range value.
   */
  static std::expected<void, TreeError> validate(double value, GaugeOptions options);

private:
  friend std::expected<void, TreeError> setGaugeValue(WidgetTree &, Node<Gauge>, double);
  friend std::expected<void, TreeError> syncGauge(WidgetTree &, Node<Gauge>);
  double value_;
  GaugeOptions options_;
  ControlStyle style_;
  GraphemeText glyph_;
  CellRect bounds_;
  bool enabled_{true};
};

/** @param tree Borrowed UI owner; mutation is allowed only between frames.
 * @param node Live gauge belonging to tree.
 * @param value Finite replacement in the inclusive configured range.
 * @return Checked boundary/value error, or success; equal values request no redraw, other changes invalidate PAINT.
 */
std::expected<void, TreeError> setGaugeValue(WidgetTree &tree, Node<Gauge> node, double value);
/** @param tree Borrowed UI owner, between frames.
 * @param node Live frame whose own and ancestor input eligibility is copied for disabled presentation.
 * @return Tree boundary error or success; unchanged eligibility requests no redraw.
 */
std::expected<void, TreeError> syncBorder(WidgetTree &tree, Node<Border> node);
/** @param tree Borrowed UI owner, between frames.
 * @param node Live gauge whose own and ancestor input eligibility is copied for disabled presentation.
 * @return Tree boundary error or success; unchanged eligibility requests no redraw.
 */
std::expected<void, TreeError> syncGauge(WidgetTree &tree, Node<Gauge> node);
} // namespace portals

#pragma pop_macro("UNICODE")
