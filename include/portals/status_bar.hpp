// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/controls.hpp>

namespace portals {

/** @brief Fill offered width and request one fixed row; explicit sizing remains authoritative. */
inline constexpr SizePolicy DEFAULT_STATUS_BAR_POLICY{.width = {.preferred = 0, .mode = SizeMode::FLEX},
                                                      .height = {.preferred = 1, .mode = SizeMode::FIXED}};

/** @brief Ordinary horizontal status sections with a full-allocation background.
 * Children retain their existing layout, text and input contracts. The bar is not a
 * focus stop and has no message timer. Managed scenes synchronize eligibility;
 * low-level callers call syncStatusBar before painting after eligibility changes.
 */
class StatusBar : public Row {
public:
  /** @param options Owning ordinary Row gap, padding, alignment and overflow settings.
   * @param style Construction-only presentation defaults beneath the theme.
   * @param policy Owning cell sizing; default fills width and requests one row.
   */
  explicit StatusBar(LinearOptions options = {}, ControlStyle style = {},
                     SizePolicy policy = DEFAULT_STATUS_BAR_POLICY);

  /** @param local Parent-relative allocation; only its checked extent is retained.
   * @return Success or INVALID_GEOMETRY for an unrepresentable local extent.
   */
  std::expected<void, TreeError> arrange(CellRect local) override;
  /** @param context Synchronous borrowed sink; no context or command escapes this call.
   * @return The sink's unchanged result after one local fill command.
   */
  std::expected<void, TreeError> paint(PaintContext &context) override;
  /** @return Owning manual normal/focused/disabled defaults beneath stylesheet declarations. */
  ComputedStyle styleDefaults() const override;

private:
  friend std::expected<void, TreeError> syncStatusBar(WidgetTree &, Node<StatusBar>);
  ControlStyle style_;
  CellRect bounds_;
  bool enabled_{true};
};

/** @brief Synchronize the bar's own disabled appearance between frames on UI.
 * @param tree Owning live tree, borrowed synchronously.
 * @param node Live observing bar identity in that tree.
 * @return Existing checked tree errors; changed eligibility invalidates paint before publication.
 */
std::expected<void, TreeError> syncStatusBar(WidgetTree &tree, Node<StatusBar> node);

} // namespace portals
