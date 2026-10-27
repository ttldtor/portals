// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "action_state.hpp"
#include "log_view_state.hpp"
#include "scroll_controls_state.hpp"
#include <limits>

namespace portals {
namespace {
using detail::ScrollControllerAccess;
using detail::ScrollRepeatState;
using detail::ScrollWidgetState;

void repeatScroll(const std::shared_ptr<ScrollRepeatState> &repeat);

bool repeatConnected(const ScrollRepeatState &repeat) noexcept {
  const auto binding = repeat.binding.lock();
  return binding && binding->connected.load();
}

std::expected<void, TreeError> armScrollRepeat(const std::shared_ptr<ScrollRepeatState> &repeat,
                                               std::uint64_t interval) {
  if (!repeatConnected(*repeat)) {
    repeat->cancel();
    return {};
  }
  const auto generation = repeat->generation;
  SchedulerTime now{};
  try {
    now = repeat->services.now();
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(TreeError::CALLBACK_FAILED);
  }
  if (!repeat->held || repeat->generation != generation) {
    return {};
  }
  if (interval > std::numeric_limits<SchedulerTime>::max() - now) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  auto eligible = repeat->tree.inputEligible(repeat->node);
  if (!eligible || !*eligible) {
    repeat->cancel();
    return {};
  }
  auto timer = repeat->tree.at(repeat->node, now + interval, [weak = std::weak_ptr(repeat), generation](Widget &) {
    if (auto current = weak.lock(); current && current->held && current->generation == generation) {
      repeatScroll(current);
    }
  });
  if (!timer) {
    return std::unexpected(timer.error());
  }
  if (repeat->held && repeat->generation == generation) {
    repeat->timer = std::move(*timer);
  }
  return {};
}

bool scrollAtEnd(const detail::ScrollControllerPlan &plan, ScrollAxis axis, bool towardEnd) {
  const auto &position = axis == ScrollAxis::HORIZONTAL ? plan.horizontal : plan.vertical;
  const auto metrics = axis == ScrollAxis::HORIZONTAL ? plan.horizontalMetrics : plan.verticalMetrics;
  return towardEnd ? position.rasterOffset() == scrollRange(metrics).maximum
                   : position.rasterOffset() == 0 && position.fraction() == 0;
}

void repeatScroll(const std::shared_ptr<ScrollRepeatState> &repeat) {
  repeat->timer = {};
  std::expected<void, TreeError> result;
  auto edited = repeat->tree.edit(repeat->node, [&](Widget &widget) {
    auto *state = detail::ScrollWidgetAccess::state(widget);
    auto eligible = repeat->tree.inputEligible(repeat->node);
    if (!repeatConnected(*repeat) || !state || state->repeat != repeat || state->retired || !state->lease ||
        !eligible || !*eligible || !state->capture.connected() ||
        (state->isArea ? state->area.readOnly : state->bar.readOnly) || state->lease->session->writer.expired()) {
      repeat->cancel();
      return;
    }
    auto plan = ScrollControllerAccess::stage(*state->lease);
    if (!plan) {
      result = std::unexpected(plan.error());
      return;
    }
    const auto metrics = repeat->axis == ScrollAxis::HORIZONTAL ? plan->horizontalMetrics : plan->verticalMetrics;
    auto live = repeat->tree.inspect(repeat->node);
    const auto channel = repeat->axis == ScrollAxis::HORIZONTAL ? state->geometry.horizontal : state->geometry.vertical;
    auto button = channel.origin;
    if (repeat->towardEnd && repeat->axis == ScrollAxis::HORIZONTAL) {
      button.x += channel.length - 1;
    } else if (repeat->towardEnd) {
      button.y += channel.length - 1;
    }
    if (live) {
      button.x += live->geometry.surface.origin().x;
      button.y += live->geometry.surface.origin().y;
    }
    if (!live || !detail::scrollArrows(*state, channel) || button != repeat->surface ||
        metrics.extent != repeat->metrics.extent || metrics.viewport != repeat->metrics.viewport) {
      repeat->cancel();
      state->capture.disconnect();
      return;
    }
    if (scrollAtEnd(*plan, repeat->axis, repeat->towardEnd)) {
      repeat->cancel();
      state->capture.disconnect();
      return;
    }
    auto stepped = ScrollControllerAccess::step(
      *plan, repeat->axis, state->isArea ? state->area.keyStep : state->bar.keyStep, repeat->towardEnd);
    if (!stepped) {
      result = std::unexpected(stepped.error());
      return;
    }
    const bool end = scrollAtEnd(*plan, repeat->axis, repeat->towardEnd);
    auto changed = detail::commitScroll(repeat->tree, repeat->node, *state, std::move(*plan));
    if (!changed) {
      result = std::unexpected(changed.error());
      return;
    }
    if (end) {
      repeat->cancel();
      state->capture.disconnect();
    } else if (repeat->held && !state->retired && state->capture.connected()) {
      result = armScrollRepeat(repeat, state->isArea ? state->area.arrowRepeat : state->bar.arrowRepeat);
    }
  });
  if (!edited) {
    result = edited;
  }
  if (!result && repeat->held) {
    repeat->cancel();
    repeat->services.onError(result.error());
  }
}

std::expected<EventReply, TreeError> arrowScroll(EventRouter &router, WidgetTree &tree, Node<Widget> node,
                                                 ScrollWidgetState &state, detail::ScrollControllerPlan plan,
                                                 ScrollAxis axis, bool towardEnd, CellPoint surface) {
  if (scrollAtEnd(plan, axis, towardEnd)) {
    return EventReply::HANDLED;
  }
  auto stepped =
    ScrollControllerAccess::step(plan, axis, state.isArea ? state.area.keyStep : state.bar.keyStep, towardEnd);
  if (!stepped) {
    return std::unexpected(stepped.error());
  }
  auto repeat = state.repeat;
  if (repeat && !scrollAtEnd(plan, axis, towardEnd)) {
    if (repeat->generation == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
    }
    auto capture = router.capturePointer(node);
    if (!capture && capture.error() != TreeError::INVALID_ARGUMENT) {
      return std::unexpected(capture.error());
    }
    if (capture) {
      state.capture = std::move(*capture);
      repeat->axis = axis;
      repeat->towardEnd = towardEnd;
      repeat->surface = surface;
      repeat->metrics = axis == ScrollAxis::HORIZONTAL ? plan.horizontalMetrics : plan.verticalMetrics;
      ++repeat->generation;
      repeat->held = true;
      auto armed = armScrollRepeat(repeat, state.isArea ? state.area.arrowDelay : state.bar.arrowDelay);
      if (!armed) {
        repeat->cancel();
        state.capture.disconnect();
        return std::unexpected(armed.error());
      }
      if (!repeat->held || state.retired || !state.lease) {
        return EventReply::HANDLED;
      }
    }
  }
  auto eligible = tree.inputEligible(node);
  if (!eligible || !*eligible || state.retired || !state.lease) {
    if (repeat) {
      repeat->cancel();
    }
    state.capture.disconnect();
    return EventReply::HANDLED;
  }
  auto changed = detail::commitScroll(tree, node, state, std::move(plan));
  if (!changed && repeat) {
    repeat->cancel();
    state.capture.disconnect();
  }
  return changed ? std::expected<EventReply, TreeError>(EventReply::HANDLED) : std::unexpected(changed.error());
}

std::uint64_t pageStep(ScrollAxisMetrics metrics, std::uint64_t overlap) {
  return metrics.viewport == 0 ? 0 : metrics.viewport > overlap ? metrics.viewport - overlap : 1;
}

std::expected<EventReply, TreeError> publish(WidgetTree &tree, Node<Widget> node, ScrollWidgetState &state,
                                             detail::ScrollControllerPlan plan) {
  auto changed = detail::commitScroll(tree, node, state, std::move(plan));
  return changed
           ? std::expected<EventReply, TreeError>(changed->logicalChanged ? EventReply::HANDLED : EventReply::IGNORED)
           : std::unexpected(changed.error());
}

std::expected<EventReply, TreeError> keyScroll(WidgetTree &tree, Node<Widget> node, ScrollWidgetState &state,
                                               const RoutedEvent &event) {
  ScrollAxis axis = state.isArea ? ScrollAxis::VERTICAL : state.axis;
  bool towardEnd{};
  bool page{};
  bool endpoint{};
  InputModifiers modifiers;
  if (const auto *action = std::get_if<ActionEvent>(&event)) {
    modifiers = action->modifiers;
    switch (action->action) {
    case Action::MOVE_LEFT:
      axis = ScrollAxis::HORIZONTAL;
      break;
    case Action::MOVE_RIGHT:
      axis = ScrollAxis::HORIZONTAL;
      towardEnd = true;
      break;
    case Action::MOVE_UP:
      axis = ScrollAxis::VERTICAL;
      break;
    case Action::MOVE_DOWN:
      axis = ScrollAxis::VERTICAL;
      towardEnd = true;
      break;
    case Action::MOVE_HOME:
      endpoint = true;
      break;
    case Action::MOVE_END:
      endpoint = true;
      towardEnd = true;
      break;
    default:
      return EventReply::IGNORED;
    }
  } else if (const auto *key = std::get_if<KeyEvent>(&event)) {
    if (key->action == KeyAction::RELEASE || (key->key != InputKey::PAGE_UP && key->key != InputKey::PAGE_DOWN)) {
      return EventReply::IGNORED;
    }
    modifiers = key->modifiers;
    page = true;
    towardEnd = key->key == InputKey::PAGE_DOWN;
  } else {
    return EventReply::IGNORED;
  }
  if (modifiers.alt || modifiers.super || (!state.isArea && axis != state.axis) || (modifiers.control && !endpoint) ||
      (modifiers.shift && !page)) {
    return EventReply::IGNORED;
  }
  if (page && modifiers.shift && state.isArea) {
    axis = ScrollAxis::HORIZONTAL;
  }
  auto plan = ScrollControllerAccess::stage(*state.lease);
  if (!plan) {
    return std::unexpected(plan.error());
  }
  const auto metrics = axis == ScrollAxis::HORIZONTAL ? plan->horizontalMetrics : plan->verticalMetrics;
  const auto overlap = state.isArea ? state.area.pageOverlap : state.bar.pageOverlap;
  std::expected<void, TreeError> moved;
  if (endpoint) {
    if (modifiers.control && state.isArea) {
      moved = ScrollControllerAccess::offset(*plan, ScrollAxis::HORIZONTAL,
                                             {towardEnd ? scrollRange(plan->horizontalMetrics).maximum : 0, 0});
      if (moved) {
        moved = ScrollControllerAccess::offset(*plan, ScrollAxis::VERTICAL,
                                               {towardEnd ? scrollRange(plan->verticalMetrics).maximum : 0, 0});
      }
    } else {
      moved = ScrollControllerAccess::offset(*plan, axis, {towardEnd ? scrollRange(metrics).maximum : 0, 0});
    }
  } else {
    moved = ScrollControllerAccess::step(*plan, axis,
                                         page           ? pageStep(metrics, overlap)
                                         : state.isArea ? state.area.keyStep
                                                        : state.bar.keyStep,
                                         towardEnd);
  }
  return moved ? publish(tree, node, state, std::move(*plan)) : std::unexpected(moved.error());
}

std::expected<EventReply, TreeError> wheelScroll(WidgetTree &tree, Node<Widget> node, ScrollWidgetState &state,
                                                 const MouseInputEvent &mouse) {
  if (mouse.modifiers.alt || mouse.modifiers.control || mouse.modifiers.super) {
    return EventReply::IGNORED;
  }
  const bool horizontal =
    mouse.button == MouseButton::WHEEL_LEFT || mouse.button == MouseButton::WHEEL_RIGHT || mouse.modifiers.shift;
  const bool towardEnd = mouse.button == MouseButton::WHEEL_DOWN || mouse.button == MouseButton::WHEEL_RIGHT;
  if (mouse.button != MouseButton::WHEEL_UP && mouse.button != MouseButton::WHEEL_DOWN &&
      mouse.button != MouseButton::WHEEL_LEFT && mouse.button != MouseButton::WHEEL_RIGHT) {
    return EventReply::IGNORED;
  }
  const auto axis = horizontal ? ScrollAxis::HORIZONTAL : ScrollAxis::VERTICAL;
  if (!state.isArea && axis != state.axis) {
    return EventReply::IGNORED;
  }
  auto plan = ScrollControllerAccess::stage(*state.lease);
  if (!plan) {
    return std::unexpected(plan.error());
  }
  const auto step = state.isArea ? state.area.wheelStep : state.bar.wheelStep;
  auto moved = ScrollControllerAccess::move(*plan, axis, towardEnd ? step : -step);
  return moved ? publish(tree, node, state, std::move(*plan)) : std::unexpected(moved.error());
}

std::expected<EventReply, TreeError> pointerScroll(EventRouter &router, WidgetTree &tree, Node<Widget> node,
                                                   ScrollWidgetState &state, const MouseInputEvent &mouse) {
  if (mouse.action == MouseAction::RELEASE) {
    detail::cancelScrollRepeat(state);
    state.capture.disconnect();
    state.drag = {};
    return EventReply::IGNORED;
  }
  if (mouse.action == MouseAction::MOVE) {
    if (state.repeat && state.repeat->held) {
      if (!state.capture.connected() || state.repeat->surface != CellPoint{mouse.column, mouse.row}) {
        state.repeat->cancel();
        state.capture.disconnect();
      }
      return EventReply::HANDLED;
    }
    if (!state.capture.connected()) {
      return EventReply::IGNORED;
    }
    if (state.lease->session->writer.expired()) {
      state.capture.disconnect();
      return EventReply::IGNORED;
    }
    auto current = state.controller->snapshot();
    if (!current) {
      return std::unexpected(current.error());
    }
    const auto currentMetrics =
      state.drag.axis == ScrollAxis::HORIZONTAL ? current->horizontal.metrics : current->vertical.metrics;
    // Another projection may publish metrics while LAYOUT is still invalid. Offset/revision changes
    // alone are compatible with this anchor; extent or viewport changes are not.
    if (currentMetrics.extent != state.drag.metrics.extent || currentMetrics.viewport != state.drag.metrics.viewport) {
      state.capture.disconnect();
      return EventReply::IGNORED;
    }
    auto geometry = tree.inspect(node);
    if (!geometry) {
      return std::unexpected(geometry.error());
    }
    if (!state.lease || state.retired) {
      return std::unexpected(TreeError::STALE_NODE);
    }
    if (geometry->geometry.valid) {
      const auto track = detail::scrollThumbTrack(
        state, state.drag.axis == ScrollAxis::HORIZONTAL ? state.geometry.horizontal : state.geometry.vertical,
        state.drag.axis);
      const auto start = state.drag.axis == ScrollAxis::HORIZONTAL
                           ? static_cast<std::int64_t>(geometry->geometry.surface.origin().x) + track.origin.x
                           : static_cast<std::int64_t>(geometry->geometry.surface.origin().y) + track.origin.y;
      const auto metrics = state.drag.axis == ScrollAxis::HORIZONTAL ? state.presentation.horizontal.metrics
                                                                     : state.presentation.vertical.metrics;
      auto thumb =
        scrollThumb(metrics, 0, track.length, state.isArea ? state.area.minimumThumb : state.bar.minimumThumb);
      if (!thumb) {
        return std::unexpected(thumb.error());
      }
      if (start != state.drag.surfaceStart || thumb->travel != state.drag.travel ||
          scrollRange(metrics).maximum != state.drag.maximum) {
        state.capture.disconnect();
        return EventReply::IGNORED;
      }
    }
    const auto pointer = state.drag.axis == ScrollAxis::HORIZONTAL ? mouse.column : mouse.row;
    auto offset = scrollOffsetForThumb(static_cast<std::int64_t>(pointer) - state.drag.surfaceStart - state.drag.grab,
                                       state.drag.travel, state.drag.maximum);
    auto plan = ScrollControllerAccess::stage(*state.lease);
    if (!offset || !plan) {
      return std::unexpected(offset ? plan.error() : offset.error());
    }
    auto moved = ScrollControllerAccess::offset(*plan, state.drag.axis, {*offset, 0});
    return moved ? publish(tree, node, state, std::move(*plan)) : std::unexpected(moved.error());
  }
  if (mouse.action != MouseAction::PRESS || mouse.button != MouseButton::LEFT || mouse.modifiers != InputModifiers{}) {
    return EventReply::IGNORED;
  }
  auto live = tree.inspect(node);
  if (!live) {
    return std::unexpected(live.error());
  }
  if (!state.lease || state.retired) {
    return std::unexpected(TreeError::STALE_NODE);
  }
  if (!live->geometry.valid || !state.arranged || (state.isArea && !state.adoptedGeometry)) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }
  const auto localX = static_cast<std::int64_t>(mouse.column) - live->geometry.surface.origin().x;
  const auto localY = static_cast<std::int64_t>(mouse.row) - live->geometry.surface.origin().y;
  auto axis = ScrollAxis::VERTICAL;
  auto track = state.geometry.vertical;
  auto inTrack = [&](ScrollTrack candidate, bool horizontal) {
    return horizontal ? localY == candidate.origin.y && localX >= candidate.origin.x &&
                          localX - candidate.origin.x < candidate.length
                      : localX == candidate.origin.x && localY >= candidate.origin.y &&
                          localY - candidate.origin.y < candidate.length;
  };
  if (!inTrack(track, false)) {
    track = state.geometry.horizontal;
    axis = ScrollAxis::HORIZONTAL;
    if (!inTrack(track, true)) {
      return EventReply::IGNORED;
    }
  }
  detail::cancelScrollRepeat(state);
  state.capture.disconnect();
  auto plan = ScrollControllerAccess::stage(*state.lease);
  if (!plan) {
    return std::unexpected(plan.error());
  }
  const auto metrics = axis == ScrollAxis::HORIZONTAL ? plan->horizontalMetrics : plan->verticalMetrics;
  const auto position =
    axis == ScrollAxis::HORIZONTAL ? plan->horizontal.rasterOffset() : plan->vertical.rasterOffset();
  const auto channelPoint = axis == ScrollAxis::HORIZONTAL ? localX - track.origin.x : localY - track.origin.y;
  if (detail::scrollArrows(state, track) && (channelPoint == 0 || channelPoint == track.length - 1)) {
    return arrowScroll(router, tree, node, state, std::move(*plan), axis, channelPoint != 0, {mouse.column, mouse.row});
  }
  track = detail::scrollThumbTrack(state, track, axis);
  auto thumb =
    scrollThumb(metrics, position, track.length, state.isArea ? state.area.minimumThumb : state.bar.minimumThumb);
  if (!thumb) {
    return std::unexpected(thumb.error());
  }
  const auto point = axis == ScrollAxis::HORIZONTAL ? localX - track.origin.x : localY - track.origin.y;
  if (point < thumb->start || point - thumb->start >= thumb->length) {
    auto moved = ScrollControllerAccess::step(
      *plan, axis, pageStep(metrics, state.isArea ? state.area.pageOverlap : state.bar.pageOverlap),
      point >= thumb->start);
    return moved ? publish(tree, node, state, std::move(*plan)) : std::unexpected(moved.error());
  }
  if (thumb->travel == 0) {
    return EventReply::HANDLED;
  }
  auto capture = router.capturePointer(node);
  if (!capture) {
    // Missing host motion/release capability is an ordinary click-only mode. Other failures stay typed.
    if (capture.error() != TreeError::INVALID_ARGUMENT) {
      return std::unexpected(capture.error());
    }
    auto eligible = tree.inputEligible(node);
    if (!eligible) {
      return std::unexpected(eligible.error());
    }
    return *eligible && state.lease && !state.retired ? std::expected<EventReply, TreeError>(EventReply::HANDLED)
                                                      : std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  const auto start = axis == ScrollAxis::HORIZONTAL
                       ? static_cast<std::int64_t>(live->geometry.surface.origin().x) + track.origin.x
                       : static_cast<std::int64_t>(live->geometry.surface.origin().y) + track.origin.y;
  state.drag = {axis,          static_cast<std::int32_t>(start), static_cast<std::int32_t>(point - thumb->start),
                thumb->travel, scrollRange(metrics).maximum,     metrics};
  state.capture = std::move(*capture);
  return EventReply::HANDLED;
}

std::expected<std::shared_ptr<ScrollRepeatState>, TreeError> prepareScrollRepeat(WidgetTree &tree, Node<Widget> node,
                                                                                 ScrollRepeatServices services) {
  if (!services.now || !services.onError) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  bool enabled{};
  auto read = tree.read(node, [&](const Widget &widget) {
    if (const auto *state = detail::ScrollWidgetAccess::state(widget)) {
      enabled = state->isArea ? state->area.arrows : state->bar.arrows;
    }
  });
  if (!read) {
    return std::unexpected(read.error());
  }
  if (!enabled) {
    return std::shared_ptr<ScrollRepeatState>{};
  }
  try {
    return std::make_shared<ScrollRepeatState>(tree, node, std::move(services));
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

template <class T>
std::expected<ActionBinding, TreeError> bindScroll(EventRouter &router, WidgetTree &tree, Node<T> node, bool writer,
                                                   std::shared_ptr<ScrollRepeatState> repeat = {}) {
  bool focusable{};
  bool recognized{};
  auto read = tree.read(node, [&](const T &widget) {
    const auto *state = detail::ScrollWidgetAccess::state(static_cast<const Widget &>(widget));
    if (state) {
      recognized = true;
      focusable = state->isArea ? state->area.focusable : state->bar.focusable;
    }
  });
  if (!read) {
    return std::unexpected(read.error());
  }
  if (!recognized) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  // Prepare repeat services before registration. Existing bindings are exclusive per node;
  // publish their replacement only after registration and same-role lease admission succeed.
  auto binding = detail::EventRouterAccess::bindChecked(
    router, node,
    [&router, &tree, node](T &widget, const RoutedEvent &event) {
      if (auto *log = dynamic_cast<LogView *>(&widget)) {
        const auto reply = detail::logViewInput(tree, node, *log, event);
        if (!reply || *reply != EventReply::IGNORED) {
          return reply;
        }
      }
      auto *state = detail::ScrollWidgetAccess::state(static_cast<Widget &>(widget));
      return state ? detail::scrollInput(router, tree, node, *state, event)
                   : std::unexpected(TreeError::INVALID_ARGUMENT);
    },
    focusable);
  if (!binding) {
    return std::unexpected(binding.error());
  }
  auto initialized = detail::initializeScrollWidget(tree, node, writer);
  if (!initialized) {
    return std::unexpected(initialized.error());
  }
  const auto cancellation = detail::EventRouterAccess::bindingCancellation(*binding);
  std::expected<void, TreeError> status;
  auto edited = tree.edit(node, [&](T &widget) {
    auto *state = detail::ScrollWidgetAccess::state(static_cast<Widget &>(widget));
    if (!state || state->retired || !state->lease) {
      status = std::unexpected(TreeError::STALE_NODE);
      return;
    }
    auto previous = std::move(state->repeat);
    state->repeat = std::move(repeat);
    if (state->repeat) {
      state->repeat->binding = cancellation;
    }
    state->capture.disconnect();
    if (previous) {
      previous->cancel();
    }
  });
  if (!edited || !status) {
    if (*initialized) {
      (void)detail::releaseScrollInitialization(tree, node);
    }
    return std::unexpected(edited ? status.error() : edited.error());
  }
  return binding;
}

template <class T>
std::expected<ActionBinding, TreeError> bindScrollRepeat(EventRouter &router, WidgetTree &tree, Node<T> node,
                                                         ScrollRepeatServices services, bool writer) {
  auto repeat = prepareScrollRepeat(tree, node, std::move(services));
  if (!repeat) {
    return std::unexpected(repeat.error());
  }
  return bindScroll(router, tree, node, writer, std::move(*repeat));
}
} // namespace

namespace detail {
std::expected<void, TreeError> configureScrollRepeat(WidgetTree &tree, Node<Widget> node, ScrollRepeatServices services,
                                                     const ActionBinding &binding) {
  auto repeat = prepareScrollRepeat(tree, node, std::move(services));
  if (!repeat) {
    return std::unexpected(repeat.error());
  }
  const auto cancellation = EventRouterAccess::bindingCancellation(binding);
  std::expected<void, TreeError> status;
  auto edited = tree.edit(node, [&](Widget &widget) {
    auto *state = ScrollWidgetAccess::state(widget);
    const auto owner = cancellation.lock();
    if (!state || state->retired || !state->lease || !owner || !owner->connected.load()) {
      status = std::unexpected(TreeError::INVALID_ARGUMENT);
      return;
    }
    auto previous = std::move(state->repeat);
    state->repeat = std::move(*repeat);
    if (state->repeat) {
      state->repeat->binding = cancellation;
    }
    state->capture.disconnect();
    if (previous) {
      previous->cancel();
    }
  });
  return edited ? status : edited;
}

std::expected<ActionBinding, TreeError> bindScrollOwner(EventRouter &router, WidgetTree &tree, Node<Widget> node) {
  return bindScroll(router, tree, node, true);
}

std::expected<EventReply, TreeError> scrollInput(EventRouter &router, WidgetTree &tree, Node<Widget> node,
                                                 ScrollWidgetState &state, const RoutedEvent &event) {
  if (!state.lease || state.retired) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  if (state.isArea ? state.area.readOnly : state.bar.readOnly) {
    cancelScrollRepeat(state);
    state.capture.disconnect();
    return EventReply::IGNORED;
  }
  if (const auto *mouse = std::get_if<MouseInputEvent>(&event)) {
    return mouse->action == MouseAction::SCROLL ? wheelScroll(tree, node, state, *mouse)
                                                : pointerScroll(router, tree, node, state, *mouse);
  }
  return keyScroll(tree, node, state, event);
}
} // namespace detail

std::expected<ActionBinding, TreeError> bindScrollArea(EventRouter &router, WidgetTree &tree, Node<ScrollArea> node) {
  return bindScroll(router, tree, node, true);
}

std::expected<ActionBinding, TreeError> bindScrollArea(EventRouter &router, WidgetTree &tree, Node<ScrollArea> node,
                                                       ScrollRepeatServices services) {
  return bindScrollRepeat(router, tree, node, std::move(services), true);
}

std::expected<ActionBinding, TreeError> bindScrollBar(EventRouter &router, WidgetTree &tree, Node<ScrollBar> node,
                                                      bool geometryWriter) {
  return bindScroll(router, tree, node, geometryWriter);
}

std::expected<ActionBinding, TreeError> bindScrollBar(EventRouter &router, WidgetTree &tree, Node<ScrollBar> node,
                                                      ScrollRepeatServices services, bool geometryWriter) {
  return bindScrollRepeat(router, tree, node, std::move(services), geometryWriter);
}
} // namespace portals
