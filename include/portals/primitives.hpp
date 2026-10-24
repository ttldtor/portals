// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/controls.hpp>
#include <portals/frame.hpp>

#include <array>
#include <optional>
#include <string>
#include <utility>

// Spell the existing decoration enum without changing the caller's native Unicode-selection macro.
#pragma push_macro("UNICODE")
#undef UNICODE

namespace portals {
/** @brief One-cell decoration selection under the complete receiving WidthPolicy. */
enum class PrimitiveGlyphs {
  UNICODE,                  ///< Unicode lines/blocks, falling back to ASCII when a glyph is not one cell.
  UNICODE_GLYPHS = UNICODE, ///< Macro-safe alias for Unicode decoration selection.
  ASCII                     ///< Portable one-cell ASCII decorations.
};

/** @brief Owning frame decoration and finite paint-work policy. */
struct BorderOptions {
  PrimitiveGlyphs glyphs{PrimitiveGlyphs::UNICODE}; ///< Owning glyph selection; no font probing.
  std::size_t maxPaintCells{16384};                 ///< Positive cap on allocated perimeter cells, even when clipped.
};

/** @brief Owning custom source; each glyph must be one printable one-cell grapheme, at most 64 bytes. */
struct FrameGlyphs {
  // Count constructors and copy construction let MSVC Debug proxy allocation failures propagate.
  std::string topLeft = std::string(0, '\0');     ///< Corner joining top and left sides.
  std::string topRight = std::string(0, '\0');    ///< Corner joining top and right sides.
  std::string bottomLeft = std::string(0, '\0');  ///< Corner joining bottom and left sides.
  std::string bottomRight = std::string(0, '\0'); ///< Corner joining bottom and right sides.
  std::string horizontal = std::string(0, '\0');  ///< Ordinary top/bottom edge.
  std::string vertical = std::string(0, '\0');    ///< Ordinary left/right edge.

  /** @brief Destroy owned glyph sources; keep construction fallible rather than generating a noexcept string move. */
  ~FrameGlyphs() = default;
};

/** @brief Maximum complete UTF-8 source bytes in each title end. */
inline constexpr std::size_t MAX_FRAME_TITLE_END_BYTES = 64;
/** @brief Maximum printable one-cell graphemes in each title end. */
inline constexpr std::int32_t MAX_FRAME_TITLE_END_CELLS = 16;

/** @brief Owning title ends; each holds at most 64 UTF-8 bytes and 16 printable one-cell graphemes. */
struct CustomTitleDecoration {
  std::string prefix = std::string(0, '\0'); ///< Complete left end, including desired spacing.
  std::string suffix = std::string(0, '\0'); ///< Complete right end, including desired spacing.

  /** @brief Preserve fallible checked-library transfers. */
  ~CustomTitleDecoration() = default;
};

/** @brief Owning advanced frame settings; legacy BorderOptions remains a literal value type. */
struct FrameOptions {
  BorderOptions border{};                     ///< Existing glyph selection and paint-work cap.
  FramePattern pattern{FramePattern::SINGLE}; ///< Built-in line pattern, overridden by customGlyphs.
  FrameSides sides{};                         ///< Independently present one-cell edges and their content insets.
  std::optional<FrameGlyphs> customGlyphs{};  ///< Optional owning custom glyph source, compiled at construction.
  std::string title = std::string(0, '\0');   ///< Owning UTF-8 top-edge title; does not increase intrinsic width.
  FrameTitleAlignment titleAlignment{FrameTitleAlignment::START}; ///< Alignment of the fitting title prefix.
  std::size_t maxTitleBytes{4096}; ///< Positive maximum complete title source bytes, including clipped text.
  TitleDecoration titleDecoration{TitleDecoration::NONE};       ///< Opt-in title ends; NONE keeps the literal title.
  std::optional<CustomTitleDecoration> customTitleDecoration{}; ///< Owning ends overriding the preset.

  /** @brief Destroy owned sources; copy construction preserves checked allocation failures in MSVC Debug. */
  ~FrameOptions() = default;
};

namespace detail {
/** @brief Construction-time owning frame text retained through each synchronous paint. */
struct PreparedFrameText {
  std::array<GraphemeText, 6> glyphs; ///< Validated corners, horizontal and vertical one-cell glyphs.
  GraphemeText title;                 ///< Complete validated title; clipping never borrows a temporary prefix.
  GraphemeText prefix;                ///< Complete validated left title end.
  GraphemeText suffix;                ///< Complete validated right title end.
};
} // namespace detail

/** @brief Owning strictly determinate gauge settings. */
struct GaugeOptions {
  double minimum{};                                 ///< Inclusive finite lower bound.
  double maximum{1};                                ///< Finite upper bound, strictly greater than minimum.
  std::int32_t preferredWidth{10};                  ///< Nonnegative intrinsic width in cells; height is one.
  PrimitiveGlyphs glyphs{PrimitiveGlyphs::UNICODE}; ///< Block or ASCII fill, with one-cell fallback.
  std::size_t maxPaintCells{16384};                 ///< Positive cap on allocated area cells, even when clipped.
};

/** @brief Frame using the existing vertical LinearLayout with one-cell insets for each present side.
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
   * Construction may allocate; tree emplacement translates allocation failures. preparation() reports invalid text
   * policy.
   */
  explicit Border(WidthPolicy width = {}, BorderOptions options = {}, ControlStyle style = {}, SizePolicy policy = {});
  /** @param width Complete receiving text-width policy.
   * @param options Owning advanced frame appearance and finite construction/paint limits.
   * @param style Construction-only manual style defaults.
   * @param policy Owning size constraints checked by the existing solver.
   * @param advanced Explicit tag preserving unambiguous legacy brace-initialized calls.
   * Text compilation precedes painting; preparation() reports semantic errors without throwing.
   */
  Border(WidthPolicy width, FrameOptions options, ControlStyle style, SizePolicy policy, std::in_place_t advanced);
  /** @param available Offered extent; titles do not enlarge intrinsic frame width.
   * @return Present-side decoration size (two by two by default), or INVALID_ARGUMENT for invalid options/text.
   */
  std::expected<CellSize, TreeError> measure(CellSize available) override;
  /** @param available Allocated outer extent offered by the parent, in cells.
   * @return Existing Column calculation with one-cell padding per present side, or unchanged layout error.
   */
  std::expected<CellSize, TreeError> childAvailable(CellSize available) const override;
  /** @param children Visible child measurements and policies, borrowed only during placement.
   * @param available Allocated outer extent in cells.
   * @return Existing Column placements with present-side insets, or unchanged layout error.
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
   * @return Success, INVALID_ARGUMENT, WOULD_BLOCK above the paint cap before submission, or unchanged sink error.
   * The cap counts perimeter glyph cells and the complete submitted title bytes plus graphemes, including clipped
   * tails. Sink exceptions propagate to the existing WidgetTree::paint boundary; discard a failed frame.
   */
  std::expected<void, TreeError> paint(PaintContext &context) override;
  /** @return Owning manual appearances beneath the existing style resolver. */
  ComputedStyle styleDefaults() const override;
  /** @param options Owning settings to validate without allocation or mutation.
   * @return Success for a recognized glyph mode and positive cap, otherwise INVALID_ARGUMENT.
   */
  static std::expected<void, TreeError> validate(BorderOptions options);
  /** @param options Borrowed owning settings; no text segmentation or allocation occurs.
   * @return Success for valid scalar/byte bounds, otherwise INVALID_ARGUMENT; call preparation() for text errors.
   */
  static std::expected<void, TreeError> validateFrame(const FrameOptions &options) noexcept;
  /** @return Construction-time text validation result, without allocation or callbacks.
   * Checked factories call this before constructing framed descendants.
   */
  std::expected<void, TreeError> preparation() const noexcept;

private:
  friend std::expected<void, TreeError> syncBorder(WidgetTree &, Node<Border>);
  friend std::expected<void, TreeError> syncBorder(WidgetTree &, Node<Border>, Node<Widget>);
  Column layout_;
  FrameOptions options_;
  ControlStyle style_;
  std::optional<detail::PreparedFrameText> text_;
  std::optional<TreeError> preparationError_;
  CellRect bounds_;
  bool enabled_{true};
  bool focused_{};
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
  friend std::expected<void, TreeError> adjustGaugeValue(WidgetTree &, Node<Gauge>, double);
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
/** @param tree Borrowed UI owner; mutation is allowed only between frames.
 * @param node Live gauge belonging to tree.
 * @param delta Finite amount added to the current value without clamping.
 * @return Checked boundary/value error, or success. Nonfinite/out-of-range results preserve state;
 * unchanged results request no redraw, other changes invalidate PAINT before publication.
 */
std::expected<void, TreeError> adjustGaugeValue(WidgetTree &tree, Node<Gauge> node, double delta);
/** @param tree Borrowed UI owner, between frames.
 * @param node Live frame whose input eligibility is copied for disabled presentation; focus projection is cleared.
 * @return Tree boundary error or success; unchanged presentation requests no redraw.
 */
std::expected<void, TreeError> syncBorder(WidgetTree &tree, Node<Border> node);
/** @param tree Borrowed UI owner; synchronization is allowed only between frames.
 * @param node Live frame receiving disabled/focused/normal presentation without becoming a focus target.
 * @param focused Empty observer, this frame, or another live node in tree; descendants project focused appearance.
 * @return Checked boundary/identity error without mutation, or success; only changed appearance invalidates PAINT.
 * Disabled presentation takes precedence over focus. The focus observer is borrowed only during this call.
 */
std::expected<void, TreeError> syncBorder(WidgetTree &tree, Node<Border> node, Node<Widget> focused);
/** @param tree Borrowed UI owner, between frames.
 * @param node Live gauge whose own and ancestor input eligibility is copied for disabled presentation.
 * @return Tree boundary error or success; unchanged eligibility requests no redraw.
 */
std::expected<void, TreeError> syncGauge(WidgetTree &tree, Node<Gauge> node);
} // namespace portals

#pragma pop_macro("UNICODE")
