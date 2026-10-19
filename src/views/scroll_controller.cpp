// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "scroll_controller_state.hpp"

#include <limits>
#include <portals/scroll_options.hpp>

namespace portals {
namespace {
template <class Options> bool validInputs(const Options &options) noexcept {
  return options.wheelStep > 0 && options.wheelStep <= std::numeric_limits<double>::max() && options.keyStep != 0 &&
         options.minimumThumb > 0 && options.maxPaintCells != 0 &&
         (options.glyphs == PrimitiveGlyphs::UNICODE || options.glyphs == PrimitiveGlyphs::ASCII);
}

bool sameMetrics(ScrollAxisMetrics left, ScrollAxisMetrics right) noexcept {
  return left.extent == right.extent && left.viewport == right.viewport;
}

bool samePosition(const ScrollState &left, const ScrollState &right) noexcept {
  return left.rasterOffset() == right.rasterOffset() && left.fraction() == right.fraction();
}

ScrollAxisSnapshot axisSnapshot(const ScrollState &state, ScrollAxisMetrics metrics) noexcept {
  return {metrics, {state.rasterOffset(), state.fraction()}};
}
} // namespace

std::expected<void, TreeError> validateScrollOptions(const ScrollBarOptions &options) noexcept {
  if (!validInputs(options)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  return {};
}

std::expected<void, TreeError> validateScrollOptions(const ScrollAreaOptions &options) noexcept {
  if (!validInputs(options) ||
      !solveScrollLayout({}, {},
                         {options.horizontal, options.vertical, options.placement, options.frame.has_value()})) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  return options.frame ? Border::validate(*options.frame) : std::expected<void, TreeError>{};
}

ScrollController::ScrollController(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {
}

ScrollController::~ScrollController() = default;

std::expected<std::shared_ptr<ScrollController>, TreeError> ScrollController::create(ScrollControllerOptions options) {
  if (!validScrollOffset(options.horizontal.offset) || !validScrollOffset(options.vertical.offset)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  try {
    auto state = std::make_unique<State>();
    auto horizontal = state->horizontal.setOffset(options.horizontal.offset.raster, options.horizontal.offset.fraction);
    auto vertical = state->vertical.setOffset(options.vertical.offset.raster, options.vertical.offset.fraction);

    if (!horizontal || !vertical) {
      return std::unexpected(!horizontal ? horizontal.error() : vertical.error());
    }

    state->horizontalMetrics = options.horizontal.metrics;
    state->verticalMetrics = options.vertical.metrics;
    state->horizontal.clamp(scrollRange(options.horizontal.metrics).count);
    state->vertical.clamp(scrollRange(options.vertical.metrics).count);

    return std::shared_ptr<ScrollController>(new ScrollController(std::move(state)));
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<ScrollControllerSnapshot, TreeError> ScrollController::snapshot() const noexcept {
  if (state_->owner != std::this_thread::get_id()) {
    return std::unexpected(TreeError::WRONG_THREAD);
  }

  return ScrollControllerSnapshot{axisSnapshot(state_->horizontal, state_->horizontalMetrics),
                                  axisSnapshot(state_->vertical, state_->verticalMetrics), state_->revision};
}
} // namespace portals

namespace portals::detail {
namespace {
std::expected<void, TreeError> checkLease(const ScrollControllerLease &lease) {
  if (!lease.owner || !lease.session) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  auto snapshot = lease.owner->snapshot();

  return snapshot ? std::expected<void, TreeError>{} : std::unexpected(snapshot.error());
}

std::expected<std::pair<ScrollState *, ScrollAxisMetrics>, TreeError> axis(ScrollControllerPlan &plan,
                                                                           ScrollAxis selection) {
  if (!plan.owner) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  auto snapshot = plan.owner->snapshot();

  if (!snapshot) {
    return std::unexpected(snapshot.error());
  }

  if (selection == ScrollAxis::HORIZONTAL) {
    return std::pair{&plan.horizontal, plan.horizontalMetrics};
  }

  if (selection == ScrollAxis::VERTICAL) {
    return std::pair{&plan.vertical, plan.verticalMetrics};
  }

  return std::unexpected(TreeError::INVALID_ARGUMENT);
}
} // namespace

std::expected<ScrollControllerLease, TreeError>
ScrollControllerAccess::acquire(std::shared_ptr<ScrollController> owner, std::uint64_t session, bool geometryWriter) {
  if (!owner || session == 0) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  auto observed = owner->snapshot();

  if (!observed) {
    return std::unexpected(observed.error());
  }

  auto association = owner->state_->association.lock();

  if (association && association->session != session) {
    return std::unexpected(TreeError::WRONG_SESSION);
  }

  if (geometryWriter && association && !association->writer.expired()) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  try {
    if (!association) {
      association = std::make_shared<ScrollMountSession>(session);
    }

    auto writer = geometryWriter ? std::make_shared<ScrollGeometryWriter>() : nullptr;

    // Client allocation can reenter acquisition and publish the winning association or writer.
    // Recheck after every allocation is complete; unpublished local values grant no reservation.
    if (auto current = owner->state_->association.lock()) {
      if (current->session != session) {
        return std::unexpected(TreeError::WRONG_SESSION);
      }

      if (geometryWriter && !current->writer.expired()) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }

      association = std::move(current);
    }

    if (writer) {
      association->writer = writer;
    }

    owner->state_->association = association;

    return ScrollControllerLease(std::move(owner), std::move(association), std::move(writer));
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<ScrollControllerPlan, TreeError> ScrollControllerAccess::stage(const ScrollControllerLease &lease) {
  auto allowed = checkLease(lease);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  const auto &state = *lease.owner->state_;

  if (state.association.lock() != lease.session) {
    return std::unexpected(TreeError::WRONG_SESSION);
  }

  return ScrollControllerPlan{
    lease.owner,    lease.session,           lease.writer,         state.revision, state.horizontal,
    state.vertical, state.horizontalMetrics, state.verticalMetrics};
}

std::expected<void, TreeError> ScrollControllerAccess::metrics(const ScrollControllerLease &lease,
                                                               ScrollControllerPlan &plan, ScrollAxisMetrics horizontal,
                                                               ScrollAxisMetrics vertical) {
  auto allowed = checkLease(lease);

  if (!allowed) {
    return allowed;
  }

  if (lease.owner->state_->association.lock() != lease.session) {
    return std::unexpected(TreeError::WRONG_SESSION);
  }

  if (plan.owner != lease.owner || plan.association.lock() != lease.session || !lease.writer ||
      plan.writer.lock() != lease.writer || lease.session->writer.lock() != lease.writer) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  plan.horizontalMetrics = horizontal;
  plan.verticalMetrics = vertical;
  plan.horizontal.clamp(scrollRange(horizontal).count);
  plan.vertical.clamp(scrollRange(vertical).count);

  return {};
}

std::expected<void, TreeError> ScrollControllerAccess::offset(ScrollControllerPlan &plan, ScrollAxis selection,
                                                              ScrollOffset offset) {
  auto selected = axis(plan, selection);

  if (!selected) {
    return std::unexpected(selected.error());
  }

  auto changed = selected->first->setOffset(offset.raster, offset.fraction);

  if (!changed) {
    return changed;
  }

  selected->first->clamp(scrollRange(selected->second).count);

  return {};
}

std::expected<void, TreeError> ScrollControllerAccess::move(ScrollControllerPlan &plan, ScrollAxis selection,
                                                            double cells) {
  auto selected = axis(plan, selection);

  if (!selected) {
    return std::unexpected(selected.error());
  }

  auto moved = selected->first->moveBy(cells, scrollRange(selected->second).count);

  return moved ? std::expected<void, TreeError>{} : std::unexpected(moved.error());
}

std::expected<void, TreeError> ScrollControllerAccess::step(ScrollControllerPlan &plan, ScrollAxis selection,
                                                            std::uint64_t cells, bool towardEnd) {
  auto selected = axis(plan, selection);

  if (!selected) {
    return std::unexpected(selected.error());
  }

  auto &position = *selected->first;
  const auto maximum = scrollRange(selected->second).maximum;
  const auto current = position.rasterOffset();
  auto next = current;
  auto fraction = position.fraction();

  if (towardEnd) {
    next = cells > maximum - current ? maximum : current + cells;

    if (next == maximum) {
      fraction = 0;
    }
  } else if (cells > current) {
    next = 0;
    fraction = 0;
  } else {
    next = current - cells;
  }

  return position.setOffset(next, fraction);
}

std::expected<ScrollControllerChange, TreeError> ScrollControllerAccess::commit(const ScrollControllerLease &lease,
                                                                                ScrollControllerPlan plan) {
  auto allowed = checkLease(lease);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  if (plan.owner != lease.owner) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  if (plan.association.lock() != lease.session || lease.owner->state_->association.lock() != lease.session) {
    return std::unexpected(TreeError::WRONG_SESSION);
  }

  auto &state = *lease.owner->state_;

  if (plan.revision != state.revision) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  const ScrollControllerChange change{!samePosition(state.horizontal, plan.horizontal) ||
                                        !samePosition(state.vertical, plan.vertical),
                                      state.horizontal.rasterOffset() != plan.horizontal.rasterOffset() ||
                                        state.vertical.rasterOffset() != plan.vertical.rasterOffset(),
                                      !sameMetrics(state.horizontalMetrics, plan.horizontalMetrics) ||
                                        !sameMetrics(state.verticalMetrics, plan.verticalMetrics)};

  if (!change.logicalChanged && !change.metricsChanged) {
    return change;
  }

  if (change.metricsChanged &&
      (!lease.writer || plan.writer.lock() != lease.writer || lease.session->writer.lock() != lease.writer)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  if (state.revision == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
  }

  state.horizontal = plan.horizontal;
  state.vertical = plan.vertical;
  state.horizontalMetrics = plan.horizontalMetrics;
  state.verticalMetrics = plan.verticalMetrics;
  ++state.revision;

  return change;
}
} // namespace portals::detail
