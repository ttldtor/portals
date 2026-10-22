// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <optional>
#include <portals/dialog.hpp>

namespace portals::detail {
inline constexpr std::string_view DIALOG_CLOSE_TEXT = "[x]";
inline constexpr CellSize DIALOG_CLOSE_SIZE{static_cast<std::int32_t>(DIALOG_CLOSE_TEXT.size()), 1};
inline constexpr std::int32_t DIALOG_CLOSE_CORNER_INSET = 1;
inline constexpr std::int32_t DIALOG_CLOSE_MIN_PANEL_WIDTH = DIALOG_CLOSE_SIZE.width + DIALOG_CLOSE_CORNER_INSET * 2;

// The managed owner validates affinity, exact ancestry and liveness before this allocation-free assignment.
struct DialogAccess {
  static bool anchor(Dialog &dialog, std::optional<CellRect> value, DialogPlacement placement) noexcept {
    if (dialog.anchor_ == value && dialog.options_.placement == placement) {
      return false;
    }
    dialog.anchor_ = value;
    dialog.options_.placement = placement;
    return true;
  }

  static void bindClose(Dialog &dialog, NodeId close) noexcept {
    dialog.close_ = close;
  }

  static void bindBody(OverlayHost &host, NodeId body) noexcept {
    host.body_ = body;
  }
};

// Owning host-local geometry only. Anchor ancestry/admission is the managed owner's responsibility.
std::expected<CellRect, TreeError> placeDialogPanel(DialogOptions options, CellSize available,
                                                    const MeasuredChild &panel, std::optional<CellRect> anchor = {});

// Complete type shared by retained managed composition and private layout fixtures.
class DialogPanel : public Border {
public:
  explicit DialogPanel(WidthPolicy width = {}, BorderOptions options = {}, ControlStyle style = {});
  std::expected<void, TreeError> arrange(CellRect local) override;
  std::expected<void, TreeError> paint(PaintContext &context) override;

private:
  BorderOptions options_;
  CellRect bounds_;
};
} // namespace portals::detail
