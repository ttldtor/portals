// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "../views/scroll_controller_state.hpp"
#include "frame_text.hpp"
#include "scroll_intent.hpp"
#include <array>
#include <portals/scroll_controls.hpp>

namespace portals::detail {
struct PreparedGlyphProfiles;
bool fractionalScrollbar(const struct ScrollWidgetState &state) noexcept;

struct ScrollDrag {
  ScrollAxis axis{ScrollAxis::VERTICAL};
  std::int32_t surfaceStart{};
  std::int32_t grab{};
  std::int32_t travel{};
  std::uint64_t maximum{};
  ScrollAxisMetrics metrics;
  std::optional<InputPointerPrecision> precision{};
  std::uint64_t grabUnits{};
  std::uint64_t anchorUnits{};
  ScrollOffset anchorOffset{};
  bool anchored{};
};

struct PreparedScrollFrame {
  FrameOptions options;
  PreparedFrameText text;
};

struct PreparedScrollbarSkin {
  // Horizontal track/thumb/start/end, vertical track/thumb/start/end, optional corner.
  std::array<std::optional<GraphemeText>, 9> glyphs{};
  std::array<std::optional<GraphemeText>, 4> arrows{};
  std::array<bool, 9> customGlyphs{};
  std::array<bool, 4> customArrows{};
  std::array<std::optional<GraphemeText>, 8> thumbSegments{}; // Start/body/end/single for each axis.
  bool ends{};
  std::optional<ScrollbarPattern> trackPattern;
  std::optional<ScrollbarThumbPattern> thumbPattern;
  std::optional<ScrollbarArrowPattern> arrowPattern;
};

struct ScrollRepeatState {
  ScrollRepeatState(WidgetTree &owner, Node<Widget> target, ScrollRepeatServices input)
      : tree(owner), node(target), services(std::move(input)) {
  }

  void cancel() noexcept {
    held = false;
    timer = {};
  }

  WidgetTree &tree;
  Node<Widget> node;
  ScrollRepeatServices services;
  Subscription timer;
  std::uint64_t generation{};
  ScrollAxis axis{ScrollAxis::VERTICAL};
  CellPoint surface;
  ScrollAxisMetrics metrics;
  bool towardEnd{};
  bool held{};
  std::weak_ptr<ActionCancellation> binding;
};

using ScrollbarPresetGlyphs = std::array<GraphemeText, 7>;

struct ScrollWidgetState {
  ScrollWidgetState(std::shared_ptr<ScrollController> controller, WidthPolicy width, PrimitiveGlyphs mode,
                    bool arrows = false);
  std::shared_ptr<ScrollController> controller;
  std::optional<ScrollControllerLease> lease;
  NodeId node;
  ScrollAreaOptions area;
  FrameOptions legacyFrameOptions;
  std::optional<PreparedFrameText> legacyFrameText;
  std::unique_ptr<PreparedScrollFrame> advancedFrame;
  std::optional<TreeError> frameError;
  ScrollBarOptions bar;
  ControlStyle style;
  ScrollAxis axis{ScrollAxis::VERTICAL};
  bool isArea{};
  bool retired{};
  bool focused{};
  bool enabled{true};
  mutable ScrollExtent extent;
  CellSize allocation;
  ScrollAreaGeometry geometry;
  ScrollControllerSnapshot presentation;
  bool arranged{};
  std::uint64_t layoutRevision{};
  std::optional<ScrollAreaGeometry> adoptedGeometry;
  std::optional<CellPoint> adoptedSurface;
  PointerCapture capture;
  std::unique_ptr<ScrollDrag> drag;
  std::shared_ptr<ScrollRepeatState> repeat;
  std::weak_ptr<ScrollIntentRecord> intent;
  std::array<GraphemeText, 8> glyphs;
  std::shared_ptr<const ScrollbarPresetGlyphs>
    presetGlyphs; // Constructor-prepared palette; paint only dereferences it.
  std::unique_ptr<std::array<GraphemeText, 4>> arrowGlyphs;
  std::unique_ptr<PreparedScrollbarSkin> scrollbarSkin;
  std::uint64_t skinRevision{};
  GlyphSelection glyphSelection;
  std::shared_ptr<const PreparedGlyphProfiles> glyphProfiles;
};

inline bool scrollArrows(const ScrollWidgetState &state, ScrollTrack channel) noexcept {
  return (state.isArea ? state.area.arrows : state.bar.arrows) && channel.length >= 3;
}

inline ScrollTrack scrollThumbTrack(const ScrollWidgetState &state, ScrollTrack channel, ScrollAxis axis) noexcept {
  if (scrollArrows(state, channel)) {
    channel.length -= 2;
    if (axis == ScrollAxis::HORIZONTAL) {
      ++channel.origin.x;
    } else {
      ++channel.origin.y;
    }
  }
  return channel;
}

inline void cancelScrollDrag(ScrollWidgetState &state) noexcept {
  state.capture.disconnect();
  state.drag.reset();
}

inline void cancelScrollRepeat(ScrollWidgetState &state) noexcept {
  if (state.repeat) {
    state.repeat->cancel();
  }
}

inline const FrameOptions *scrollFrameOptions(const ScrollWidgetState &state) noexcept {
  return state.advancedFrame ? &state.advancedFrame->options : state.area.frame ? &state.legacyFrameOptions : nullptr;
}

inline LayoutPadding scrollFrameInsets(const ScrollWidgetState &state) noexcept {
  const auto *frame = scrollFrameOptions(state);

  return frame ? frame->sides.insets() : LayoutPadding{};
}

inline ScrollLayoutOptions scrollLayoutOptions(const ScrollWidgetState &state) noexcept {
  const auto *frame = scrollFrameOptions(state);

  return {state.area.horizontal, state.area.vertical, state.area.placement, frame != nullptr,
          frame ? frame->sides : FrameSides{}};
}

struct ScrollWidgetAccess {
  static ScrollWidgetState &state(ScrollArea &widget) {
    return *widget.state_;
  }

  static const ScrollWidgetState &state(const ScrollArea &widget) {
    return *widget.state_;
  }

  static ScrollWidgetState &state(ScrollBar &widget) {
    return *widget.state_;
  }

  static const ScrollWidgetState &state(const ScrollBar &widget) {
    return *widget.state_;
  }

  static ScrollWidgetState *state(Widget &widget);
  static const ScrollWidgetState *state(const Widget &widget);

  static std::expected<ScrollAreaLayoutSnapshot, TreeError> snapshot(const Widget &widget, NodeId node,
                                                                     NodeGeometry geometry, bool visible) {
    const auto *source = state(widget);
    if (!source || !source->isArea) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    const auto &value = *source;
    if (!value.lease || value.retired || value.node != node) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    if (visible && (!geometry.valid || !value.arranged)) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }
    return ScrollAreaLayoutSnapshot{node,
                                    geometry,
                                    value.extent,
                                    value.geometry,
                                    value.layoutRevision,
                                    visible,
                                    value.presentation.horizontal.offset,
                                    value.presentation.vertical.offset};
  }
};

std::expected<void, TreeError> validateScrollWidget(const ScrollWidgetState &state);
std::expected<bool, TreeError> initializeScrollWidget(WidgetTree &tree, Node<Widget> node, bool writer);
std::expected<void, TreeError> releaseScrollInitialization(WidgetTree &tree, Node<Widget> node);
std::expected<ActionBinding, TreeError> bindScrollOwner(EventRouter &router, WidgetTree &tree, Node<Widget> node);
std::expected<void, TreeError> configureScrollRepeat(WidgetTree &tree, Node<Widget> node, ScrollRepeatServices services,
                                                     const ActionBinding &binding);
enum class ScrollPublication {
  POSITION,
  PREPARATION
};
std::expected<ScrollControllerChange, TreeError>
commitScroll(WidgetTree &tree, Node<Widget> node, ScrollWidgetState &state, ScrollControllerPlan plan,
             ScrollPublication publication = ScrollPublication::POSITION);
std::expected<ScrollAreaGeometry, TreeError> areaPlacement(const ScrollWidgetState &state, CellSize available);
std::expected<void, TreeError> arrangeScroll(ScrollWidgetState &state, CellSize allocation);
ComputedStyle scrollDefaults(const ScrollWidgetState &state);
std::expected<void, TreeError> paintScroll(ScrollWidgetState &state, ComputedStyleView style, PaintContext &context);
std::expected<EventReply, TreeError> scrollInput(EventRouter &router, WidgetTree &tree, Node<Widget> node,
                                                 ScrollWidgetState &state, const RoutedEvent &event);
} // namespace portals::detail
