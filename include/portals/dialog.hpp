// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <optional>
#include <portals/grid.hpp>
#include <portals/primitives.hpp>

namespace portals {
namespace detail {
struct DialogAccess;
}

/** @brief Input policy applied by the managed dialog owner. */
enum class DialogMode {
  MODAL,    ///< Confine input to the active modal layer.
  NON_MODAL ///< Let input outside the panel reach the scene.
};

/** @brief Preferred panel position within the owning host. */
enum class DialogPlacement {
  CENTER,      ///< Center in the usable viewport, rounding down.
  BELOW_START, ///< Prefer below the body anchor, aligned to its leading edge.
  ABOVE_START  ///< Prefer above the body anchor, aligned to its leading edge.
};

/** @brief Generic dismissal cause, independent of application results. */
enum class DialogDismissReason {
  PROGRAMMATIC, ///< Explicit dismissal by application code.
  CANCEL        ///< Cancellation through the managed input policy.
};

/** @brief Which top-frame segment owns the close affordance. */
enum class DialogClosePlacement {
  START, ///< Immediately after the leading corner.
  END    ///< Immediately before the trailing corner.
};

/** @brief Optional finite close chrome settings; the glyph occupies three terminal cells. */
struct DialogCloseOptions {
  bool enabled{true};                                        ///< Enable this explicitly requested close affordance.
  DialogClosePlacement placement{DialogClosePlacement::END}; ///< Top-edge placement.
  bool focusable{}; ///< Include in focus traversal; initial action focus is unchanged.
};

/** @brief Owning registration budget for a single scene overlay host. */
struct OverlayHostOptions {
  std::size_t maxLayers{32}; ///< Positive cap on registered layers, including hidden layers.
};

/** @brief Owning panel geometry and managed input settings, in terminal cells. */
struct DialogOptions {
  DialogMode mode{DialogMode::MODAL};                 ///< Modal input confinement by default.
  DialogPlacement placement{DialogPlacement::CENTER}; ///< Preferred placement; anchors belong to the host body.
  CellSize maximumPanelSize{60, 20};                  ///< Strictly positive independent width and height caps.
  std::int32_t viewportMargin{1};                     ///< Nonnegative inset, reduced on tiny viewports.
  std::int32_t anchorGap{1};                          ///< Nonnegative distance from the anchor.
  bool closeOnEscape{true};                           ///< Allow managed cancellation with Escape.
  BorderOptions frame{};                              ///< Frame glyph policy and allocated opaque-area paint cap.
  DialogCloseOptions closeButton{.enabled = false};   ///< Opt-in close chrome on the top frame edge.
};

/** @brief Transparent overlay with exact body identity and body-only intrinsic measurement.
 * The managed owner binds the body before layout. Unbound hosts return INVALID_ARGUMENT.
 * Hidden body contributes no intrinsic extent. Existing Overlay placement and sibling order apply.
 * Pure layout methods borrow their spans only for the call and never access the tree.
 */
class OverlayHost : public Overlay {
public:
  /** @param options Owning positive layer-registration budget, validated during layout.
   * @param policy Owning host size policy within its parent.
   */
  explicit OverlayHost(OverlayHostOptions options = {}, SizePolicy policy = {});
  /** @return Owning copy of the host registration budget. */
  OverlayHostOptions hostOptions() const;
  /** @param available Offered host extent, normalized before use.
   * @return Existing overlay child bounds, or INVALID_ARGUMENT for unbound/invalid host settings.
   */
  std::expected<CellSize, TreeError> childAvailable(CellSize available) const override;
  /** @param children Visible measurements with exact owning node identities.
   * @return Only the bound body's intrinsic extent, or validation/geometry errors.
   */
  std::expected<CellSize, TreeError> measureChildren(std::span<const MeasuredChild> children) const override;
  /** @param children Visible body and dialog roots in sibling paint order.
   * @param available Full host allocation in cells.
   * @return Existing overlay placements, or unchanged validation/geometry/allocation errors.
   */
  std::expected<std::vector<CellRect>, TreeError> arrangeChildren(std::span<const MeasuredChild> children,
                                                                  CellSize available) const override;

private:
  friend struct detail::DialogAccess;
  OverlayHostOptions options_;
  NodeId body_{};
};

/** @brief Full-host structural root containing at most one visible opaque panel.
 * This is deliberately not a SizedWidget: panel limits never narrow the structural root.
 * Layout is pure; input confinement, registration, anchors and title/body composition belong
 * to the managed owner. No observer, callback, model or tree reference is retained here.
 */
class Dialog : public Widget, public Layout {
public:
  /** @param options Owning panel and input settings; pure layout validates them before use. */
  explicit Dialog(DialogOptions options = {});
  /** @return Owning copy of the complete dialog settings. */
  DialogOptions options() const;
  /** @param options Owning settings, checked without mutation or allocation.
   * @return Success, or INVALID_ARGUMENT for invalid enums, extents, margins, gap or frame settings.
   */
  static std::expected<void, TreeError> validate(DialogOptions options);
  /** @param available Full offered host extent in cells.
   * @return Usable panel measurement bounds, or INVALID_ARGUMENT for invalid settings.
   */
  std::expected<CellSize, TreeError> childAvailable(CellSize available) const override;
  /** @param children Zero or one visible panel measurement, borrowed only for this call.
   * @return Panel intrinsic extent capped by options, or INVALID_ARGUMENT for invalid structure/settings.
   */
  std::expected<CellSize, TreeError> measureChildren(std::span<const MeasuredChild> children) const override;
  /** @param children Zero or one visible panel with its intrinsic measurement and policies.
   * @param available Full host allocation; only the panel is capped and centered.
   * @return Owning panel rectangle, or validation, geometry or allocation error.
   */
  std::expected<std::vector<CellRect>, TreeError> arrangeChildren(std::span<const MeasuredChild> children,
                                                                  CellSize available) const override;

private:
  friend struct detail::DialogAccess;
  DialogOptions options_;
  std::optional<CellRect> anchor_;
  NodeId close_{};
};
} // namespace portals
