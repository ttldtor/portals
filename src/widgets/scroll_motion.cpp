// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "scroll_motion.hpp"
#include "action_state.hpp"

#include <algorithm>
#include <limits>
#include <new>

namespace portals::detail {
namespace {
constexpr SchedulerTime MOTION_CADENCE_MS = 16;

bool before(ScrollOffset left, ScrollOffset right) noexcept {
  return left.raster < right.raster || (left.raster == right.raster && left.fraction < right.fraction);
}

bool same(ScrollOffset left, ScrollOffset right) noexcept {
  return left.raster == right.raster && left.fraction == right.fraction;
}

std::array<ScrollOffset, 2> offsets(const ScrollControllerPlan &plan) noexcept {
  return {{{plan.horizontal.rasterOffset(), plan.horizontal.fraction()},
           {plan.vertical.rasterOffset(), plan.vertical.fraction()}}};
}

// Scale only the unsigned distance. Absolute logical positions never pass through double.
ScrollOffset interpolate(ScrollOffset from, ScrollOffset target, SchedulerTime elapsed,
                         SchedulerTime duration) noexcept {
  if (!elapsed) {
    return from;
  }
  if (elapsed >= duration) {
    return target;
  }

  const bool forward = before(from, target);
  const auto low = forward ? from : target;
  const auto high = forward ? target : from;
  const auto progress = forward ? elapsed : duration - elapsed;
  auto distance = high.raster - low.raster;
  auto fraction = high.fraction - low.fraction;

  if (fraction < 0) {
    --distance;
    fraction += 1;
  }

  const auto scaled = scrollMultiplyDivide(progress, distance, duration);
  ScrollOffset result{low.raster + scaled.quotient,
                      low.fraction + static_cast<double>(scaled.remainder) / static_cast<double>(duration) +
                        fraction * static_cast<double>(progress) / static_cast<double>(duration)};

  const auto carry = static_cast<std::uint64_t>(result.fraction);

  if (carry > high.raster - result.raster) {
    return high;
  }

  result.raster += carry;
  result.fraction -= static_cast<double>(carry);
  if (before(high, result)) {
    return high;
  }

  return result;
}

bool connected(const ScrollRepeatState &input) noexcept {
  const auto binding = input.binding.lock();

  return binding && binding->connected.load();
}

std::expected<SchedulerTime, TreeError> currentTime(const ScrollRepeatState &input) {
  try {
    return input.services.now();
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(TreeError::CALLBACK_FAILED);
  }
}

bool eligible(const std::shared_ptr<ScrollMotion> &motion, const std::shared_ptr<ScrollRepeatState> &input,
              const ScrollWidgetState &state) {
  const auto session = motion->session.lock();

  if (!motion->active || !connected(*input) || input->motion != motion || !session ||
      session->motion.lock() != motion || state.repeat != input || !state.lease || state.retired ||
      state.node != input->node.id() || state.lease->session != session || session->writer.expired() ||
      !smoothScrollEnabled(state) || (state.isArea ? state.area.readOnly : state.bar.readOnly)) {
    return false;
  }

  const auto live = input->tree.inputEligible(input->node);

  return live && *live;
}

void advance(const std::shared_ptr<ScrollMotion> &motion, Widget &widget);

std::expected<Subscription, TreeError> arm(const std::shared_ptr<ScrollMotion> &motion,
                                           const std::shared_ptr<ScrollRepeatState> &input, SchedulerTime now,
                                           SchedulerTime remaining) {
  const auto interval = std::min(MOTION_CADENCE_MS, remaining);

  if (interval > std::numeric_limits<SchedulerTime>::max() - now) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  return input->tree.at(input->node, now + interval, [weak = std::weak_ptr(motion)](Widget &widget) {
    if (const auto current = weak.lock(); current && current->active) {
      advance(current, widget);
    }
  });
}

void fail(const std::shared_ptr<ScrollMotion> &motion, const std::shared_ptr<ScrollRepeatState> &input,
          TreeError error) {
  if (!motion->active) {
    return;
  }

  motion->cancel();
  // The pinned services owner survives a callback that removes the widget or closes its scene.
  input->services.onError(error);
}

void advance(const std::shared_ptr<ScrollMotion> &motion, Widget &widget) {
  motion->timer = {};

  const auto input = motion->input.lock();
  auto *state = ScrollWidgetAccess::state(widget);

  if (!input || !state || !eligible(motion, input, *state)) {
    motion->cancel();

    return;
  }

  auto plan = ScrollControllerAccess::stage(*state->lease);

  if (!plan || plan->revision != motion->revision) {
    motion->cancel();

    return;
  }

  const auto now = currentTime(*input);

  if (!eligible(motion, input, *state)) {
    motion->cancel();

    return;
  }
  if (!now || *now < motion->started) {
    fail(motion, input, now ? TreeError::INVALID_ARGUMENT : now.error());

    return;
  }

  auto current = state->controller->snapshot();

  if (!current || current->revision != motion->revision) {
    motion->cancel();

    return;
  }

  const auto elapsed = std::min<SchedulerTime>(*now - motion->started, motion->duration);

  for (const auto axis : {ScrollAxis::HORIZONTAL, ScrollAxis::VERTICAL}) {
    const auto index = axis == ScrollAxis::HORIZONTAL ? 0u : 1u;
    const auto value = interpolate(motion->from[index], motion->target[index], elapsed, motion->duration);
    const auto moved = ScrollControllerAccess::offset(*plan, axis, value);

    if (!moved) {
      fail(motion, input, moved.error());

      return;
    }
  }

  const auto changed = commitScroll(input->tree, input->node, *state, std::move(*plan), ScrollPublication::MOTION);

  if (!changed) {
    fail(motion, input, changed.error());

    return;
  }

  current = state->controller->snapshot();

  if (!current || !eligible(motion, input, *state) || elapsed == motion->duration) {
    motion->cancel();

    return;
  }

  motion->revision = current->revision;

  auto timer = arm(motion, input, *now, motion->duration - elapsed);

  if (!timer) {
    fail(motion, input, timer.error());
  } else if (eligible(motion, input, *state)) {
    motion->timer = std::move(*timer);
  }
}
} // namespace

void ScrollMotion::cancel() noexcept {
  active = false;
  timer = {};
}

bool smoothScrollEnabled(const ScrollWidgetState &state) noexcept {
  return (state.isArea ? state.area.smoothDurationMs : state.bar.smoothDurationMs) != 0 && fractionalScrollbar(state);
}

void cancelScrollMotion(ScrollWidgetState &state) noexcept {
  if (state.lease) {
    if (const auto motion = state.lease->session->motion.lock()) {
      motion->cancel();
    }
    state.lease->session->motion.reset();
  }
  cancelOwnedScrollMotion(state);
}

void cancelOwnedScrollMotion(ScrollWidgetState &state) noexcept {
  if (!state.repeat || !state.repeat->motion) {
    return;
  }

  const auto &motion = state.repeat->motion;

  motion->cancel();

  if (const auto session = motion->session.lock(); session && session->motion.lock() == motion) {
    session->motion.reset();
  }
}

std::expected<ScrollControllerPlan, TreeError> stageScrollInput(ScrollWidgetState &state, bool accumulate) {
  auto plan = ScrollControllerAccess::stage(*state.lease);

  if (!plan || !accumulate || !state.repeat || !smoothScrollEnabled(state)) {
    return plan;
  }

  const auto motion = state.lease->session->motion.lock();

  if (!motion || !motion->active) {
    return plan;
  }
  if (motion->revision != plan->revision) {
    motion->cancel();

    return plan;
  }

  const auto input = motion->input.lock();
  bool admitted{};

  const auto read = input ? input->tree.read(input->node,
                                             [&](const Widget &widget) {
                                               const auto *owner = ScrollWidgetAccess::state(widget);

                                               admitted = owner && eligible(motion, input, *owner);
                                             })
                          : std::expected<void, TreeError>{};

  if (!read || !admitted) {
    motion->cancel();

    return plan;
  }

  for (const auto axis : {ScrollAxis::HORIZONTAL, ScrollAxis::VERTICAL}) {
    const auto moved =
      ScrollControllerAccess::offset(*plan, axis, motion->target[axis == ScrollAxis::HORIZONTAL ? 0u : 1u]);

    if (!moved) {
      return std::unexpected(moved.error());
    }
  }

  return plan;
}

std::expected<EventReply, TreeError> publishScrollInput(WidgetTree &tree, Node<Widget> node, ScrollWidgetState &state,
                                                        ScrollControllerPlan plan, bool smooth) {
  if (!smooth || !smoothScrollEnabled(state) || !state.repeat || !connected(*state.repeat)) {
    const auto changed = commitScroll(tree, node, state, std::move(plan));

    return changed
             ? std::expected<EventReply, TreeError>(changed->logicalChanged ? EventReply::HANDLED : EventReply::IGNORED)
             : std::unexpected(changed.error());
  }

  auto current = ScrollControllerAccess::stage(*state.lease);

  if (!current || current->revision != plan.revision) {
    return std::unexpected(current ? TreeError::WOULD_BLOCK : current.error());
  }

  const auto from = offsets(*current);
  const auto target = offsets(plan);
  const auto pending = state.lease->session->motion.lock();

  if (pending && pending->active && pending->revision == plan.revision && same(pending->target[0], target[0]) &&
      same(pending->target[1], target[1])) {
    return EventReply::HANDLED;
  }

  if (same(from[0], target[0]) && same(from[1], target[1])) {
    const auto active = state.lease->session->motion.lock();
    const bool moving = active && active->active;

    cancelScrollMotion(state);

    return moving ? EventReply::HANDLED : EventReply::IGNORED;
  }

  const auto intent = checkScrollIntent(state, plan);

  if (!intent) {
    return std::unexpected(intent.error());
  }

  const auto input = state.repeat;
  const auto session = state.lease->session;
  const auto previous = session->motion.lock();
  const auto command = session->motionCommand;
  const auto duration = state.isArea ? state.area.smoothDurationMs : state.bar.smoothDurationMs;
  const auto stillCurrent = [&] {
    const auto live = tree.inputEligible(node);
    const auto observed = state.controller->snapshot();

    return live && *live && !state.retired && state.lease && state.lease->session == session && state.repeat == input &&
           connected(*input) && smoothScrollEnabled(state) &&
           !(state.isArea ? state.area.readOnly : state.bar.readOnly) && session->motion.lock() == previous &&
           session->motionCommand == command && observed && observed->revision == plan.revision;
  };
  std::shared_ptr<ScrollMotion> motion;

  try {
    motion = std::make_shared<ScrollMotion>();
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
  if (!stillCurrent()) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  const auto now = currentTime(*input);

  if (!now) {
    return std::unexpected(now.error());
  }
  if (!stillCurrent()) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  motion->input = input;
  motion->session = session;
  motion->from = from;
  motion->target = target;
  motion->started = *now;
  motion->revision = plan.revision;
  motion->duration = duration;

  if (motion->duration > std::numeric_limits<SchedulerTime>::max() - *now) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  auto timer = arm(motion, input, *now, motion->duration);

  if (!timer) {
    return std::unexpected(timer.error());
  }

  // Allocation and the client clock may reenter another input, replace a binding, or remove a node.
  if (!stillCurrent()) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  if (previous) {
    previous->cancel();
  }

  motion->timer = std::move(*timer);
  motion->active = true;
  input->motion = motion;
  session->motion = motion;
  recordScrollIntent(state);

  return EventReply::HANDLED;
}
} // namespace portals::detail
