// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../layout/grid_detail.hpp"
#include "dialog_detail.hpp"

#include <algorithm>
#include <new>

namespace portals {
namespace {
using grid_detail::Wide;

std::int32_t usableMargin(std::int32_t extent, std::int32_t requested) {
  return std::min(requested, std::max(0, extent - 1) / 2);
}

std::expected<CellRect, TreeError> usableViewport(DialogOptions options, CellSize available) {
  const auto valid = Dialog::validate(options);

  if (!valid) {
    return std::unexpected(valid.error());
  }

  const auto size = available.normalized();
  const auto horizontal = usableMargin(size.width, options.viewportMargin);
  const auto vertical = usableMargin(size.height, options.viewportMargin);
  const auto result = CellRect::from({horizontal, vertical}, {size.width - horizontal * 2, size.height - vertical * 2});

  return result ? std::expected<CellRect, TreeError>(*result) : std::unexpected(TreeError::INVALID_GEOMETRY);
}

// Reuse Border's resolved enabled/disabled appearance rather than keeping a second style state.
class OpaqueBorderSink : public PaintContext {
public:
  OpaqueBorderSink(PaintContext &sink, CellRect bounds) : sink_(sink), bounds_(bounds) {
  }

  std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
    if (!filled_ && !commands.empty()) {
      const auto style = std::visit(
        [](const auto &command) {
          return command.style;
        },
        commands.front());
      const RenderCommand fill = FillCommand{bounds_, bounds_, style};
      const auto result = sink_.submit(std::span(&fill, 1));

      if (!result) {
        return result;
      }

      filled_ = true;
    }

    return sink_.submit(commands);
  }

private:
  PaintContext &sink_;
  CellRect bounds_;
  bool filled_{};
};
} // namespace

OverlayHost::OverlayHost(OverlayHostOptions options, SizePolicy policy) : Overlay({}, policy), options_(options) {
}

OverlayHostOptions OverlayHost::hostOptions() const {
  return options_;
}

std::expected<CellSize, TreeError> OverlayHost::childAvailable(CellSize available) const {
  if (options_.maxLayers == 0 || body_.session == 0 || body_.generation == 0) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  return Overlay::childAvailable(available);
}

std::expected<CellSize, TreeError> OverlayHost::measureChildren(std::span<const MeasuredChild> children) const {
  const auto valid = childAvailable({});

  if (!valid) {
    return std::unexpected(valid.error());
  }

  const MeasuredChild *body = nullptr;

  for (const auto &child : children) {
    if (!grid_detail::valid(child)) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    if (child.node == body_) {
      body = &child;
    }
  }

  return body ? Overlay::measureChildren(std::span(body, 1)) : std::expected<CellSize, TreeError>(CellSize{});
}

std::expected<std::vector<CellRect>, TreeError> OverlayHost::arrangeChildren(std::span<const MeasuredChild> children,
                                                                             CellSize available) const {
  const auto valid = childAvailable(available);

  if (!valid) {
    return std::unexpected(valid.error());
  }

  return Overlay::arrangeChildren(children, available);
}

Dialog::Dialog(DialogOptions options) : options_(options) {
}

DialogOptions Dialog::options() const {
  return options_;
}

std::expected<void, TreeError> Dialog::validate(DialogOptions options) {
  if ((options.mode != DialogMode::MODAL && options.mode != DialogMode::NON_MODAL) ||
      (options.placement != DialogPlacement::CENTER && options.placement != DialogPlacement::BELOW_START &&
       options.placement != DialogPlacement::ABOVE_START) ||
      options.maximumPanelSize.width <= 0 || options.maximumPanelSize.height <= 0 || options.viewportMargin < 0 ||
      options.anchorGap < 0 ||
      (options.closeButton.placement != DialogClosePlacement::START &&
       options.closeButton.placement != DialogClosePlacement::END)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  return Border::validate(options.frame);
}

std::expected<CellSize, TreeError> Dialog::childAvailable(CellSize available) const {
  const auto viewport = usableViewport(options_, available);

  if (!viewport) {
    return std::unexpected(viewport.error());
  }

  return CellSize{std::min(viewport->size().width, options_.maximumPanelSize.width),
                  std::min(viewport->size().height, options_.maximumPanelSize.height)};
}

std::expected<CellSize, TreeError> Dialog::measureChildren(std::span<const MeasuredChild> children) const {
  const auto valid = validate(options_);

  if (!valid) {
    return std::unexpected(valid.error());
  }

  if (children.size() > (close_.session ? 2u : 1u) ||
      (!children.empty() &&
       (!grid_detail::valid(children.front()) || (close_.session && children.front().node == close_))) ||
      (children.size() == 2 && (children.back().node != close_ || !grid_detail::valid(children.back())))) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  if (children.empty()) {
    return CellSize{};
  }

  const auto &panel = children.front();

  if (panel.policy.width.minimum > options_.maximumPanelSize.width ||
      panel.policy.height.minimum > options_.maximumPanelSize.height) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  if (children.size() == 2 && options_.closeButton.enabled) {
    const auto &close = children.back().policy;
    const auto size = detail::DIALOG_CLOSE_SIZE;

    // Bounds describe the visible chrome. Tiny panels still deliberately hide it with an empty rectangle.
    if (close.width.minimum > size.width || close.width.maximum < size.width || close.height.minimum > size.height ||
        close.height.maximum < size.height) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }
  }

  return grid_detail::size(
    std::min<Wide>(grid_detail::requested(panel.policy.width, panel.desired.width), options_.maximumPanelSize.width),
    std::min<Wide>(grid_detail::requested(panel.policy.height, panel.desired.height),
                   options_.maximumPanelSize.height));
}

std::expected<std::vector<CellRect>, TreeError> Dialog::arrangeChildren(std::span<const MeasuredChild> children,
                                                                        CellSize available) const {
  const auto measured = measureChildren(children);

  if (!measured) {
    return std::unexpected(measured.error());
  }

  try {
    // As in Overlay, avoid noexcept vector default/move constructors: MSVC Debug allocates iterator proxies.
    std::vector<CellRect> result(0);

    if (!children.empty()) {
      const auto panel = detail::placeDialogPanel(options_, available, children.front(), anchor_);

      if (!panel) {
        return std::unexpected(panel.error());
      }

      result.push_back(*panel);

      if (children.size() == 2) {
        // The chrome is a sibling of the bordered content: it occupies no body column,
        // and Border retains its inner child clip even for oversized custom content.
        const auto size = panel->size();
        const auto origin = panel->origin();
        const auto visible =
          options_.closeButton.enabled && size.width >= detail::DIALOG_CLOSE_MIN_PANEL_WIDTH && size.height > 0;
        const auto x = options_.closeButton.placement == DialogClosePlacement::START
                         ? origin.x + detail::DIALOG_CLOSE_CORNER_INSET
                         : origin.x + size.width - detail::DIALOG_CLOSE_SIZE.width - detail::DIALOG_CLOSE_CORNER_INSET;
        const auto close =
          CellRect::from(visible ? CellPoint{x, origin.y} : origin, visible ? detail::DIALOG_CLOSE_SIZE : CellSize{});

        if (!close) {
          return std::unexpected(TreeError::INVALID_GEOMETRY);
        }

        result.push_back(*close);
      }
    }

    return std::expected<std::vector<CellRect>, TreeError>(std::in_place, result);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

namespace detail {
std::expected<CellRect, TreeError> placeDialogPanel(DialogOptions options, CellSize available,
                                                    const MeasuredChild &panel, std::optional<CellRect> anchor) {
  const auto viewport = usableViewport(options, available);

  if (!viewport) {
    return std::unexpected(viewport.error());
  }

  if (!grid_detail::valid(panel)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  if (panel.policy.width.minimum > std::min(options.maximumPanelSize.width, viewport->size().width) ||
      panel.policy.height.minimum > std::min(options.maximumPanelSize.height, viewport->size().height)) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }

  const auto width = std::min({grid_detail::requested(panel.policy.width, panel.desired.width),
                               Wide{options.maximumPanelSize.width}, Wide{viewport->size().width}});
  const auto height = std::min({grid_detail::requested(panel.policy.height, panel.desired.height),
                                Wide{options.maximumPanelSize.height}, Wide{viewport->size().height}});
  Wide x = viewport->origin().x + (viewport->size().width - width) / 2;
  Wide y = viewport->origin().y + (viewport->size().height - height) / 2;

  if (anchor && options.placement != DialogPlacement::CENTER) {
    const auto above = Wide{anchor->origin().y} - options.anchorGap;
    const auto below = Wide{anchor->bottom()} + options.anchorGap;
    const auto aboveSpace = std::max<Wide>(0, above - viewport->origin().y);
    const auto belowSpace = std::max<Wide>(0, Wide{viewport->bottom()} - below);
    bool useBelow = options.placement == DialogPlacement::BELOW_START;

    if ((useBelow && height > belowSpace && aboveSpace > belowSpace) ||
        (!useBelow && height > aboveSpace && belowSpace > aboveSpace)) {
      useBelow = !useBelow;
    }

    x = std::clamp<Wide>(anchor->origin().x, viewport->origin().x, Wide{viewport->right()} - width);
    y = std::clamp<Wide>(useBelow ? below : above - height, viewport->origin().y, Wide{viewport->bottom()} - height);
  }

  const auto result = CellRect::from({static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)},
                                     {static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)});

  return result ? std::expected<CellRect, TreeError>(*result) : std::unexpected(TreeError::INVALID_GEOMETRY);
}

DialogPanel::DialogPanel(WidthPolicy width, BorderOptions options, ControlStyle style)
    : Border(width, options, style), options_(options) {
}

std::expected<void, TreeError> DialogPanel::arrange(CellRect local) {
  const auto result = Border::arrange(local);

  if (result) {
    bounds_ = *CellRect::from({}, local.size());
  }

  return result;
}

std::expected<void, TreeError> DialogPanel::paint(PaintContext &context) {
  const auto valid = Border::validate(options_);

  if (!valid) {
    return valid;
  }

  const auto size = bounds_.size();
  const auto area = static_cast<std::uint64_t>(size.width) * static_cast<std::uint64_t>(size.height);

  if (area > options_.maxPaintCells) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  OpaqueBorderSink sink(context, bounds_);

  return Border::paint(sink);
}
} // namespace detail
} // namespace portals
