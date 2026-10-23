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
  const auto *frame = state.isArea ? scrollFrameOptions(state) : nullptr;
  const auto *frameText = state.advancedFrame     ? &state.advancedFrame->text
                          : state.legacyFrameText ? &*state.legacyFrameText
                                                  : nullptr;
  const bool onFrame = frame && state.area.placement == ScrollPlacement::ON_FRAME;
  std::array<CellRect, 2> exclusions;
  std::size_t exclusionCount{};

  if (onFrame) {
    if (state.geometry.horizontal.length != 0) {
      exclusions[exclusionCount++] =
        *CellRect::from(state.geometry.horizontal.origin, {state.geometry.horizontal.length, 1});
    }

    if (state.geometry.vertical.length != 0) {
      exclusions[exclusionCount++] =
        *CellRect::from(state.geometry.vertical.origin, {1, state.geometry.vertical.length});
    }
  }

  const auto excluded = std::span(exclusions.data(), exclusionCount);
  const auto frameWork = frame && frameText ? framePaintWork(*frameText, *frame, state.allocation, excluded) : 0;
  const auto fullFrameWork = frame && frameText ? framePaintWork(*frameText, *frame, state.allocation) : 0;
  const auto trackCells = static_cast<std::uint64_t>(state.geometry.horizontal.length) + state.geometry.vertical.length;
  const auto work = frameWork + trackCells + (state.geometry.hasCorner ? 1 : 0);
  const auto budget = state.isArea ? state.area.maxPaintCells : state.bar.maxPaintCells;

  if (work > budget || (frame && fullFrameWork > frame->border.maxPaintCells)) {
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
  if (frame && frameText) {
    auto painted = paintFrame(context, *frameText, *frame, *bounds, style, excluded,
                              resolved.frame[!state.enabled  ? 2
                                             : state.focused ? 1
                                                             : 0]);

    if (!painted) {
      return painted;
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
