// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "../views/scroll_controller_state.hpp"
#include "frame_text.hpp"
#include "scroll_intent.hpp"
#include <array>
#include <portals/scroll_controls.hpp>

namespace portals::detail {
struct ScrollDrag {
  ScrollAxis axis{ScrollAxis::VERTICAL};
  std::int32_t surfaceStart{};
  std::int32_t grab{};
  std::int32_t travel{};
  std::uint64_t maximum{};
  ScrollAxisMetrics metrics;
};

struct PreparedScrollFrame {
  FrameOptions options;
  PreparedFrameText text;
};

struct PreparedScrollbarSkin {
  // Horizontal track/thumb/start/end, vertical track/thumb/start/end, optional corner.
  std::array<std::optional<GraphemeText>, 9> glyphs{};
  bool ends{};
};

struct ScrollWidgetState {
  ScrollWidgetState(std::shared_ptr<ScrollController> controller, WidthPolicy width, PrimitiveGlyphs mode);
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
  ScrollDrag drag;
  std::weak_ptr<ScrollIntentRecord> intent;
  std::array<GraphemeText, 8> glyphs;
  std::unique_ptr<PreparedScrollbarSkin> scrollbarSkin;
  std::uint64_t skinRevision{};
};

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
std::expected<void, TreeError> paintScroll(ScrollWidgetState &state, const ComputedStyle &style, PaintContext &context);
std::expected<EventReply, TreeError> scrollInput(EventRouter &router, WidgetTree &tree, Node<Widget> node,
                                                 ScrollWidgetState &state, const RoutedEvent &event);
} // namespace portals::detail
