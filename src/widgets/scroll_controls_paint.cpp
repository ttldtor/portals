// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "glyph_profiles.hpp"
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

std::expected<void, TreeError> paintScroll(ScrollWidgetState &state, ComputedStyleView resolved,
                                           PaintContext &context) {
  if (!state.lease || state.retired || !state.arranged) {
    return {};
  }
  const auto *frame = state.isArea ? scrollFrameOptions(state) : nullptr;
  const auto stateIndex = !state.enabled ? 2 : state.focused ? 1 : 0;
  const auto *profile = state.glyphProfiles ? state.glyphProfiles->states[stateIndex].get() : nullptr;
  const auto *profileGlyphs = profile ? profile->scrollbar.get() : nullptr;
  const auto *frameText = profile && profile->frame ? profile->frame.get()
                          : state.advancedFrame     ? &state.advancedFrame->text
                          : state.legacyFrameText   ? &*state.legacyFrameText
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

  auto horizontal = scrollThumb(state.presentation.horizontal.metrics, state.presentation.horizontal.offset.raster,
                                scrollThumbTrack(state, state.geometry.horizontal, ScrollAxis::HORIZONTAL).length,
                                state.isArea ? state.area.minimumThumb : state.bar.minimumThumb);
  auto vertical = scrollThumb(state.presentation.vertical.metrics, state.presentation.vertical.offset.raster,
                              scrollThumbTrack(state, state.geometry.vertical, ScrollAxis::VERTICAL).length,
                              state.isArea ? state.area.minimumThumb : state.bar.minimumThumb);
  auto bounds = CellRect::from({}, state.allocation);
  if (!horizontal || !vertical || !bounds) {
    return std::unexpected(!horizontal ? horizontal.error()
                           : !vertical ? vertical.error()
                                       : TreeError::INVALID_GEOMETRY);
  }
  const auto style = !state.enabled ? resolved.disabled : state.focused ? resolved.focused : resolved.normal;
  const auto &parts = resolved.scrollbar[stateIndex];
  const auto *skin = state.scrollbarSkin.get();
  const auto thumbPattern = parts.thumbPattern ? parts.thumbPattern : skin ? skin->thumbPattern : std::nullopt;
  const auto trackPattern = parts.trackPattern ? parts.trackPattern : skin ? skin->trackPattern : std::nullopt;
  const auto arrowPattern = parts.arrowPattern ? parts.arrowPattern : skin ? skin->arrowPattern : std::nullopt;
  const auto *segments = resolved.scrollbarThumbSegments ? &(*resolved.scrollbarThumbSegments)[stateIndex] : nullptr;
  auto draw = [&](CellPoint point, const GraphemeText &glyph, const std::optional<CellStyle> &part) {
    const RenderCommand command = TextCommand{point, glyph, *bounds, part.value_or(style)};
    return context.submit(std::span(&command, 1));
  };
  auto drawSegment = [&](CellPoint point, const GraphemeText &glyph, const CellStyle &part) {
    const RenderCommand command = TextCommand{point, glyph, *bounds, part};
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
    if (cells.length == 0) {
      return {};
    }

    const bool arrows = scrollArrows(state, cells);
    const auto first = horizontalAxis ? 0 : 4;
    const bool customTrack = skin && skin->customGlyphs[first];
    const bool customThumb = skin && skin->customGlyphs[first + 1];
    const auto *trackGlyph = skin ? &*skin->glyphs[first] : &state.glyphs[6];
    const auto *thumbGlyph = skin ? &*skin->glyphs[first + 1] : &state.glyphs[7];

    if (profileGlyphs) {
      if (!customTrack) {
        trackGlyph = &*(*profileGlyphs)[horizontalAxis ? 0 : 1];
      }

      if (!customThumb) {
        thumbGlyph = &*(*profileGlyphs)[horizontalAxis ? 5 : 9];
      }
    }

    if (!customThumb && thumbPattern) {
      thumbGlyph = *thumbPattern == ScrollbarThumbPattern::ASCII ? &(*state.presetGlyphs)[2] : &state.glyphs[7];
    }

    if (!customTrack && trackPattern) {
      switch (*trackPattern) {
      case ScrollbarPattern::LEGACY:
        trackGlyph = &state.glyphs[6];

        break;
      case ScrollbarPattern::FRAME:
        trackGlyph =
          frameText ? &frameText->glyphs[4 + (horizontalAxis ? 0 : 1)] : &state.glyphs[horizontalAxis ? 4 : 5];

        break;
      case ScrollbarPattern::MINIMAL:
        trackGlyph = &(*state.presetGlyphs)[0];

        break;
      case ScrollbarPattern::ASCII:
        trackGlyph = &(*state.presetGlyphs)[1];

        break;
      }
    }

    for (std::int32_t i = 0; i < cells.length; ++i) {
      auto point = cells.origin;
      if (horizontalAxis) {
        point.x += i;
      } else {
        point.y += i;
      }
      const auto position = i - (arrows ? 1 : 0);
      const bool inThumb = position >= thumb.start && position - thumb.start < thumb.length;
      const auto *glyph = inThumb ? thumbGlyph : trackGlyph;
      const auto *part = inThumb ? &parts.thumb : &parts.track;
      const CellStyle *segmentStyle{};

      if (inThumb && (skin || segments || profileGlyphs)) {
        const auto segment = thumb.length == 1                            ? 3
                             : position == thumb.start                    ? 0
                             : position - thumb.start == thumb.length - 1 ? 2
                                                                          : 1;
        if (profileGlyphs && !customThumb && !thumbPattern) {
          glyph = &*(*profileGlyphs)[(horizontalAxis ? PROFILE_HORIZONTAL_THUMB : PROFILE_VERTICAL_THUMB) + segment];
        }

        if (skin) {
          const auto &prepared = skin->thumbSegments[first + segment];

          if (prepared) {
            glyph = &*prepared;
          }
        }

        if (segments) {
          const auto styles = std::array{&segments->start, &segments->body, &segments->end, &segments->single};

          segmentStyle = styles[segment];
        }
      }

      if (skin && skin->ends && !inThumb && !arrows) {
        const auto end = i == 0 ? first + 2 : i == cells.length - 1 ? first + 3 : -1;

        if (end >= 0 && skin->glyphs[end]) {
          glyph = &*skin->glyphs[end];
          part = &parts.ends;
        }
      }

      if (arrows && (i == 0 || i == cells.length - 1)) {
        const auto arrow = (horizontalAxis ? 0 : 2) + (i == 0 ? 0 : 1);
        glyph = skin && skin->arrows[arrow] ? &*skin->arrows[arrow] : &(*state.arrowGlyphs)[arrow];
        part = &parts.arrows;
        segmentStyle = nullptr;

        if (profileGlyphs && !(skin && skin->customArrows[arrow])) {
          glyph = &*(*profileGlyphs)[PROFILE_ARROWS + arrow];
        }

        if (arrowPattern && !(skin && skin->customArrows[arrow])) {
          glyph = *arrowPattern == ScrollbarArrowPattern::ASCII ? &(*state.presetGlyphs)[3 + arrow]
                                                                : &(*state.arrowGlyphs)[arrow];
        }
      }

      auto result = segmentStyle ? drawSegment(point, *glyph, *segmentStyle) : draw(point, *glyph, *part);
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
  if (state.scrollbarSkin && state.scrollbarSkin->glyphs[8]) {
    return draw(state.geometry.corner, *state.scrollbarSkin->glyphs[8], parts.corner);
  }

  const RenderCommand command = FillCommand{*corner, *bounds, parts.corner.value_or(style)};
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
  const auto &cached = computedStyle();

  if (cached) {
    // The tree's paint borrow prevents style publication while the sink runs.
    return detail::paintScroll(*state_, *cached, context);
  }

  const auto defaults = styleDefaults();

  return detail::paintScroll(*state_, defaults, context);
}

std::expected<void, TreeError> ScrollBar::paint(PaintContext &context) {
  const auto &cached = computedStyle();

  if (cached) {
    // The tree's paint borrow prevents style publication while the sink runs.
    return detail::paintScroll(*state_, *cached, context);
  }

  const auto defaults = styleDefaults();

  return detail::paintScroll(*state_, defaults, context);
}
} // namespace portals
