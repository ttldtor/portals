// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "scroll_controls_state.hpp"
#include <portals/grid.hpp>

namespace portals {
namespace {
GraphemeText scrollGlyph(std::string_view unicode, std::string_view ascii, WidthPolicy width, PrimitiveGlyphs mode) {
  auto text = GraphemeText::from(mode == PrimitiveGlyphs::UNICODE ? unicode : ascii, width).value();
  return text.measure() == 1 ? text : GraphemeText::from(ascii, width).value();
}

template <class Function>
std::expected<void, TreeError> editScroll(WidgetTree &tree, Node<Widget> node, Function function) {
  std::expected<void, TreeError> result;
  auto edited = tree.edit(node, [&](Widget &widget) {
    auto *state = detail::ScrollWidgetAccess::state(widget);
    if (!state || !state->lease || state->retired) {
      result = std::unexpected(TreeError::INVALID_ARGUMENT);
      return;
    }
    result = function(*state);
  });
  return edited ? result : edited;
}
} // namespace

namespace detail {
ScrollWidgetState::ScrollWidgetState(std::shared_ptr<ScrollController> values, WidthPolicy width, PrimitiveGlyphs mode)
    : controller(std::move(values)),
      glyphs{scrollGlyph("\xe2\x94\x8c", "+", width, mode), scrollGlyph("\xe2\x94\x90", "+", width, mode),
             scrollGlyph("\xe2\x94\x94", "+", width, mode), scrollGlyph("\xe2\x94\x98", "+", width, mode),
             scrollGlyph("\xe2\x94\x80", "-", width, mode), scrollGlyph("\xe2\x94\x82", "|", width, mode),
             scrollGlyph("\xe2\x96\x91", ".", width, mode), scrollGlyph("\xe2\x96\x88", "#", width, mode)} {
  if (!controller) {
    auto created = ScrollController::create();
    if (!created) {
      throw std::bad_alloc();
    }
    controller = std::move(*created);
  }
}

ScrollWidgetState *ScrollWidgetAccess::state(Widget &widget) {
  if (auto *area = dynamic_cast<ScrollArea *>(&widget)) {
    return &state(*area);
  }
  if (auto *bar = dynamic_cast<ScrollBar *>(&widget)) {
    return &state(*bar);
  }
  if (auto *grid = dynamic_cast<Grid *>(&widget)) {
    return grid->scroll_.get();
  }
  return nullptr;
}

const ScrollWidgetState *ScrollWidgetAccess::state(const Widget &widget) {
  if (const auto *area = dynamic_cast<const ScrollArea *>(&widget)) {
    return &state(*area);
  }
  if (const auto *bar = dynamic_cast<const ScrollBar *>(&widget)) {
    return &state(*bar);
  }
  if (const auto *grid = dynamic_cast<const Grid *>(&widget)) {
    return grid->scroll_.get();
  }
  return nullptr;
}

std::expected<void, TreeError> validateScrollWidget(const ScrollWidgetState &state) {
  auto valid = state.isArea ? validateScrollOptions(state.area) : validateScrollOptions(state.bar);
  if (!valid) {
    return valid;
  }
  if (state.retired || (!state.isArea && state.axis != ScrollAxis::HORIZONTAL && state.axis != ScrollAxis::VERTICAL)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  auto observed = state.controller->snapshot();
  return observed ? std::expected<void, TreeError>{} : std::unexpected(observed.error());
}

std::expected<bool, TreeError> initializeScrollWidget(WidgetTree &tree, Node<Widget> node, bool writer) {
  std::expected<bool, TreeError> result{false};
  auto edited = tree.edit(node, [&](Widget &widget) {
    auto *state = ScrollWidgetAccess::state(widget);
    if (!state) {
      result = std::unexpected(TreeError::INVALID_ARGUMENT);
      return;
    }
    auto valid = validateScrollWidget(*state);
    if (!valid) {
      result = std::unexpected(valid.error());
      return;
    }
    if (state->lease) {
      if (state->node != node.id() || static_cast<bool>(state->lease->writer) != writer) {
        result = std::unexpected(TreeError::INVALID_ARGUMENT);
      }
      return;
    }
    auto lease = ScrollControllerAccess::acquire(state->controller, node.id().session, writer);
    if (!lease) {
      result = std::unexpected(lease.error());
      return;
    }
    // Reentrant allocation can retire this widget. Never publish its newly acquired lease afterward.
    auto live = tree.inspect(node);
    if (!live || state->retired) {
      result = std::unexpected(live ? TreeError::STALE_NODE : live.error());
      return;
    }
    if (state->lease) {
      result = std::unexpected(TreeError::WOULD_BLOCK);
      return;
    }
    state->node = node.id();
    state->lease.emplace(std::move(*lease));
    result = true;
  });
  return edited ? result : std::unexpected(edited.error());
}

std::expected<void, TreeError> releaseScrollInitialization(WidgetTree &tree, Node<Widget> node) {
  auto edited = tree.edit(node, [&](Widget &widget) {
    if (auto *state = ScrollWidgetAccess::state(widget)) {
      state->capture.disconnect();
      state->lease.reset();
      state->node = {};
    }
  });
  // Removed controls already released their own lease in unmount.
  return !edited && (edited.error() == TreeError::STALE_NODE || edited.error() == TreeError::CLOSED)
           ? std::expected<void, TreeError>{}
           : edited;
}

std::expected<ScrollControllerChange, TreeError> commitScroll(WidgetTree &tree, Node<Widget> node,
                                                              ScrollWidgetState &state, ScrollControllerPlan plan,
                                                              ScrollPublication publication) {
  if (!state.lease || state.retired || state.node != node.id()) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  if (publication == ScrollPublication::POSITION) {
    auto intent = checkScrollIntent(state, plan);
    if (!intent) {
      return std::unexpected(intent.error());
    }
  }
  auto change = ScrollControllerAccess::commit(*state.lease, std::move(plan));
  if (!change) {
    return std::unexpected(change.error());
  }
  if (publication == ScrollPublication::POSITION && change->logicalChanged) {
    recordScrollIntent(state);
  }
  if (change->metricsChanged) {
    state.capture.disconnect();
  }
  if (change->metricsChanged || change->rasterChanged) {
    auto invalidated = tree.invalidate(node, Invalidation::LAYOUT);
    if (!invalidated) {
      return std::unexpected(invalidated.error());
    }
  }
  return change;
}
} // namespace detail

ScrollArea::ScrollArea(std::shared_ptr<ScrollController> controller, WidthPolicy width, ScrollAreaOptions options,
                       ControlStyle style, SizePolicy policy)
    : SizedWidget(policy),
      state_(std::make_unique<detail::ScrollWidgetState>(std::move(controller), width, options.glyphs)) {
  state_->isArea = true;
  state_->area = std::move(options);
  state_->style = style;
  if (state_->area.frame) {
    constexpr std::array<std::string_view, 6> UNICODE{"\xe2\x94\x8c", "\xe2\x94\x90", "\xe2\x94\x94",
                                                      "\xe2\x94\x98", "\xe2\x94\x80", "\xe2\x94\x82"};
    constexpr std::array<std::string_view, 6> ASCII{"+", "+", "+", "+", "-", "|"};
    for (std::size_t i = 0; i < UNICODE.size(); ++i) {
      state_->glyphs[i] = scrollGlyph(UNICODE[i], ASCII[i], width, state_->area.frame->glyphs);
    }
  }
}

ScrollArea::~ScrollArea() = default;

void ScrollArea::unmount() {
  state_->retired = true;
  state_->intent.reset();
  state_->capture.disconnect();
  state_->lease.reset();
}

ScrollBar::ScrollBar(std::shared_ptr<ScrollController> controller, ScrollAxis axis, WidthPolicy width,
                     ScrollBarOptions options, ControlStyle style, SizePolicy policy)
    : SizedWidget(policy),
      state_(std::make_unique<detail::ScrollWidgetState>(std::move(controller), width, options.glyphs)) {
  state_->axis = axis;
  state_->bar = options;
  state_->style = style;
}

ScrollBar::~ScrollBar() = default;

void ScrollBar::unmount() {
  state_->retired = true;
  state_->intent.reset();
  state_->capture.disconnect();
  state_->lease.reset();
}

std::expected<void, TreeError> initializeScrollArea(WidgetTree &tree, Node<ScrollArea> node) {
  auto result = detail::initializeScrollWidget(tree, node, true);
  return result ? std::expected<void, TreeError>{} : std::unexpected(result.error());
}

std::expected<void, TreeError> initializeScrollBar(WidgetTree &tree, Node<ScrollBar> node, bool geometryWriter) {
  auto result = detail::initializeScrollWidget(tree, node, geometryWriter);
  return result ? std::expected<void, TreeError>{} : std::unexpected(result.error());
}

std::expected<void, TreeError> setScrollOffset(WidgetTree &tree, Node<Widget> node, ScrollAxis axis,
                                               ScrollOffset offset) {
  return editScroll(tree, node, [&](detail::ScrollWidgetState &state) -> std::expected<void, TreeError> {
    auto plan = detail::ScrollControllerAccess::stage(*state.lease);
    if (!plan) {
      return std::unexpected(plan.error());
    }
    auto changed = detail::ScrollControllerAccess::offset(*plan, axis, offset);
    if (!changed) {
      return changed;
    }
    auto result = detail::commitScroll(tree, node, state, std::move(*plan));
    return result ? std::expected<void, TreeError>{} : std::unexpected(result.error());
  });
}

std::expected<void, TreeError> setScrollMetrics(WidgetTree &tree, Node<ScrollBar> node, ScrollAxisMetrics metrics) {
  return editScroll(tree, node, [&](detail::ScrollWidgetState &state) -> std::expected<void, TreeError> {
    auto plan = detail::ScrollControllerAccess::stage(*state.lease);
    if (!plan) {
      return std::unexpected(plan.error());
    }
    auto staged = detail::ScrollControllerAccess::metrics(
      *state.lease, *plan, state.axis == ScrollAxis::HORIZONTAL ? metrics : plan->horizontalMetrics,
      state.axis == ScrollAxis::VERTICAL ? metrics : plan->verticalMetrics);
    if (!staged) {
      return staged;
    }
    auto result = detail::commitScroll(tree, node, state, std::move(*plan), detail::ScrollPublication::PREPARATION);
    if (result && result->metricsChanged) {
      state.capture.disconnect();
    }
    return result ? std::expected<void, TreeError>{} : std::unexpected(result.error());
  });
}

std::expected<ScrollAreaLayoutSnapshot, TreeError> scrollAreaLayout(WidgetTree &tree, Node<ScrollArea> node) {
  auto inspected = tree.inspect(node);
  if (!inspected) {
    return std::unexpected(inspected.error());
  }
  bool visible = true;
  for (Node<Widget> current = node; current.id().session != 0;) {
    auto ancestor = tree.inspect(current);
    if (!ancestor) {
      return std::unexpected(ancestor.error());
    }
    visible &= ancestor->visible;
    current = ancestor->parent;
  }
  std::expected<ScrollAreaLayoutSnapshot, TreeError> result{std::unexpected(TreeError::INVALID_ARGUMENT)};
  auto read = tree.read(node, [&](const ScrollArea &area) {
    result = detail::ScrollWidgetAccess::snapshot(area, node.id(), inspected->geometry, visible);
  });
  return read ? result : std::unexpected(read.error());
}

std::expected<void, TreeError> prepareScrollArea(WidgetTree &tree, Node<ScrollArea> node,
                                                 const ScrollAreaLayoutSnapshot &layout) {
  return editScroll(tree, node, [&](detail::ScrollWidgetState &state) -> std::expected<void, TreeError> {
    if (layout.node != node.id()) {
      return std::unexpected(layout.node.session != node.id().session ? TreeError::WRONG_SESSION
                                                                      : TreeError::STALE_NODE);
    }
    if (!layout.visible) {
      return {};
    }
    if (!layout.geometry.valid) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }
    if (layout.layoutRevision != state.layoutRevision) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    if (layout.extent.width != state.extent.width || layout.extent.height != state.extent.height ||
        layout.geometry.local.size() != state.allocation) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }
    auto placement = solveScrollLayout(
      layout.geometry.local.size(), layout.extent,
      {state.area.horizontal, state.area.vertical, state.area.placement, state.area.frame.has_value()});
    if (!placement || placement->viewport != layout.placement.viewport ||
        placement->viewportOrigin != layout.placement.viewportOrigin) {
      return std::unexpected(placement ? TreeError::INVALID_GEOMETRY : placement.error());
    }
    auto plan = detail::ScrollControllerAccess::stage(*state.lease);
    if (!plan) {
      return std::unexpected(plan.error());
    }
    auto staged = detail::ScrollControllerAccess::metrics(
      *state.lease, *plan, {layout.extent.width, static_cast<std::uint64_t>(placement->viewport.width)},
      {layout.extent.height, static_cast<std::uint64_t>(placement->viewport.height)});
    if (!staged) {
      return staged;
    }
    const bool trackChanged = !state.adoptedGeometry || !state.adoptedSurface ||
                              *state.adoptedSurface != layout.geometry.surface.origin() ||
                              state.adoptedGeometry->horizontal.origin != placement->horizontal.origin ||
                              state.adoptedGeometry->horizontal.length != placement->horizontal.length ||
                              state.adoptedGeometry->vertical.origin != placement->vertical.origin ||
                              state.adoptedGeometry->vertical.length != placement->vertical.length;
    if (trackChanged) {
      state.capture.disconnect();
    }
    auto changed = detail::commitScroll(tree, node, state, std::move(*plan), detail::ScrollPublication::PREPARATION);
    if (!changed) {
      return std::unexpected(changed.error());
    }
    if (trackChanged || changed->metricsChanged) {
      state.capture.disconnect();
    }
    state.adoptedGeometry = *placement;
    state.adoptedSurface = layout.geometry.surface.origin();
    return {};
  });
}

std::expected<void, TreeError> syncScrollControl(WidgetTree &tree, Node<Widget> node, Node<Widget> focused) {
  auto eligible = tree.inputEligible(node);
  if (!eligible) {
    return std::unexpected(eligible.error());
  }
  std::expected<void, TreeError> result;
  auto edited = tree.edit(node, [&](Widget &widget) {
    auto *state = detail::ScrollWidgetAccess::state(widget);
    if (!state) {
      result = std::unexpected(TreeError::INVALID_ARGUMENT);
      return;
    }
    const bool hasFocus = focused == node;
    if (state->enabled != *eligible || state->focused != hasFocus) {
      state->enabled = *eligible;
      state->focused = hasFocus;
      result = tree.invalidate(node, Invalidation::PAINT);
    }
  });
  return edited ? result : edited;
}
} // namespace portals
