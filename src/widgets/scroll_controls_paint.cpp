// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "scroll_controls_state.hpp"

namespace portals {
namespace detail {
ComputedStyle scrollDefaults(const ScrollWidgetState &state) {
  ComputedStyle result;
  result.normal = state.style.normal;
  result.focused = state.style.focused;
  result.disabled = state.style.disabled;
  return result;
}

std::expected<void, TreeError> paintScroll(ScrollWidgetState &state, const ComputedStyle &resolved,
                                           PaintContext &context) {
  if (!state.lease || state.retired || !state.arranged) {
    return {};
  }
  const auto [width, height] = state.allocation;
  const bool frame = state.isArea && state.area.frame.has_value();
  const auto perimeter = !frame || width == 0 || height == 0 ? 0ULL
                         : width == 1 || height == 1         ? static_cast<std::uint64_t>(width) * height
                                                             : 2ULL * width + 2ULL * height - 4;
  const auto trackCells = static_cast<std::uint64_t>(state.geometry.horizontal.length) + state.geometry.vertical.length;
  const bool onFrame = frame && state.area.placement == ScrollPlacement::ON_FRAME;
  const auto work = perimeter + (onFrame ? 0 : trackCells) + (state.geometry.hasCorner ? 1 : 0);
  const auto budget = state.isArea ? state.area.maxPaintCells : state.bar.maxPaintCells;
  if (work > budget || (frame && perimeter > state.area.frame->maxPaintCells)) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }
  auto horizontal =
    scrollThumb(state.presentation.horizontal.metrics, state.presentation.horizontal.offset.raster,
                state.geometry.horizontal.length, state.isArea ? state.area.minimumThumb : state.bar.minimumThumb);
  auto vertical =
    scrollThumb(state.presentation.vertical.metrics, state.presentation.vertical.offset.raster,
                state.geometry.vertical.length, state.isArea ? state.area.minimumThumb : state.bar.minimumThumb);
  auto bounds = CellRect::from({}, state.allocation);
  if (!horizontal || !vertical || !bounds) {
    return std::unexpected(!horizontal ? horizontal.error()
                           : !vertical ? vertical.error()
                                       : TreeError::INVALID_GEOMETRY);
  }
  const auto style = !state.enabled ? resolved.disabled : state.focused ? resolved.focused : resolved.normal;
  auto draw = [&](CellPoint point, std::size_t glyph) {
    const RenderCommand command = TextCommand{point, state.glyphs[glyph], *bounds, style};
    return context.submit(std::span(&command, 1));
  };
  auto ownedByTrack = [&](CellPoint point) {
    if (!onFrame) {
      return false;
    }
    const auto &x = state.geometry.horizontal;
    const auto &y = state.geometry.vertical;
    return (point.y == x.origin.y && point.x >= x.origin.x &&
            static_cast<std::int64_t>(point.x) < static_cast<std::int64_t>(x.origin.x) + x.length) ||
           (point.x == y.origin.x && point.y >= y.origin.y &&
            static_cast<std::int64_t>(point.y) < static_cast<std::int64_t>(y.origin.y) + y.length);
  };
  auto edge = [&](CellPoint point, std::size_t glyph) -> std::expected<void, TreeError> {
    return ownedByTrack(point) ? std::expected<void, TreeError>{} : draw(point, glyph);
  };
  if (frame && width > 0 && height > 0) {
    for (std::int32_t x = 0; x < width; ++x) {
      auto result = edge({x, 0}, x == 0 ? 0 : x == width - 1 ? 1 : 4);
      if (!result) {
        return result;
      }
      if (height > 1) {
        result = edge({x, height - 1}, x == 0 ? 2 : x == width - 1 ? 3 : 4);
        if (!result) {
          return result;
        }
      }
    }
    for (std::int32_t y = 1; y < height - 1; ++y) {
      auto result = edge({0, y}, 5);
      if (!result) {
        return result;
      }
      if (width > 1) {
        result = edge({width - 1, y}, 5);
        if (!result) {
          return result;
        }
      }
    }
  }
  auto track = [&](ScrollTrack cells, ScrollThumb thumb, bool horizontalAxis) -> std::expected<void, TreeError> {
    for (std::int32_t i = 0; i < cells.length; ++i) {
      auto point = cells.origin;
      if (horizontalAxis) {
        point.x += i;
      } else {
        point.y += i;
      }
      auto result = draw(point, i >= thumb.start && i - thumb.start < thumb.length ? 7 : 6);
      if (!result) {
        return result;
      }
    }
    return {};
  };
  auto result = track(state.geometry.horizontal, *horizontal, true);
  if (!result) {
    return result;
  }
  result = track(state.geometry.vertical, *vertical, false);
  if (!result || !state.geometry.hasCorner) {
    return result;
  }
  auto corner = CellRect::from(state.geometry.corner, {1, 1});
  if (!corner) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }
  const RenderCommand command = FillCommand{*corner, *bounds, style};
  return context.submit(std::span(&command, 1));
}
} // namespace detail

ComputedStyle ScrollArea::styleDefaults() const {
  return detail::scrollDefaults(*state_);
}

ComputedStyle ScrollBar::styleDefaults() const {
  return detail::scrollDefaults(*state_);
}

std::expected<void, TreeError> ScrollArea::paint(PaintContext &context) {
  return detail::paintScroll(*state_, computedStyle() ? *computedStyle() : styleDefaults(), context);
}

std::expected<void, TreeError> ScrollBar::paint(PaintContext &context) {
  return detail::paintScroll(*state_, computedStyle() ? *computedStyle() : styleDefaults(), context);
}
} // namespace portals
