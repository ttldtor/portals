// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <optional>
#include <portals/dialog.hpp>

namespace portals::detail {
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
