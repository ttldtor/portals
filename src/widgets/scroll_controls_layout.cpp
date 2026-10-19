// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "scroll_controls_state.hpp"

#include <limits>

namespace portals {
namespace {
std::int32_t intrinsicAxis(std::int32_t measured, AxisSize policy) {
  return std::clamp(policy.preferred == INTRINSIC_EXTENT ? measured : policy.preferred, policy.minimum, policy.maximum);
}

ScrollOffset effectiveOffset(ScrollOffset offset, ScrollAxisMetrics metrics) {
  const auto maximum = scrollRange(metrics).maximum;
  offset.raster = std::min(offset.raster, maximum);
  if (offset.raster == maximum) {
    offset.fraction = 0;
  }
  return offset;
}
} // namespace

namespace detail {
std::expected<ScrollAreaGeometry, TreeError> areaPlacement(const ScrollWidgetState &state, CellSize available) {
  return solveScrollLayout(
    available, state.extent,
    {state.area.horizontal, state.area.vertical, state.area.placement, state.area.frame.has_value()});
}

std::expected<void, TreeError> arrangeScroll(ScrollWidgetState &state, CellSize allocation) {
  auto valid = validateScrollWidget(state);
  if (!valid) {
    return valid;
  }
  auto observed = state.controller->snapshot();
  if (!observed) {
    return std::unexpected(observed.error());
  }
  if (state.layoutRevision == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
  }
  ScrollAreaGeometry geometry;
  if (state.isArea) {
    auto placed = areaPlacement(state, allocation);
    if (!placed) {
      return std::unexpected(placed.error());
    }
    geometry = *placed;
    observed->horizontal.metrics = {state.extent.width, static_cast<std::uint64_t>(geometry.viewport.width)};
    observed->vertical.metrics = {state.extent.height, static_cast<std::uint64_t>(geometry.viewport.height)};
    observed->horizontal.offset = effectiveOffset(observed->horizontal.offset, observed->horizontal.metrics);
    observed->vertical.offset = effectiveOffset(observed->vertical.offset, observed->vertical.metrics);
  } else {
    geometry.viewport = allocation;
    geometry.childClipSize = allocation;
    if (state.axis == ScrollAxis::HORIZONTAL) {
      geometry.horizontalBar = true;
      geometry.horizontal.length = allocation.height > 0 ? allocation.width : 0;
    } else {
      geometry.verticalBar = true;
      geometry.vertical.length = allocation.width > 0 ? allocation.height : 0;
    }
  }
  // These are derived layout values only. Canonical controller publication belongs to preparation.
  state.allocation = allocation;
  state.geometry = geometry;
  state.presentation = *observed;
  state.arranged = true;
  ++state.layoutRevision;
  return {};
}
} // namespace detail

std::expected<CellSize, TreeError> ScrollArea::measure(CellSize) {
  auto valid = detail::validateScrollWidget(*state_);
  if (!valid) {
    return std::unexpected(valid.error());
  }
  return state_->area.frame ? CellSize{2, 2} : CellSize{};
}

std::expected<CellSize, TreeError> ScrollArea::childAvailable(CellSize available) const {
  auto valid = detail::validateScrollWidget(*state_);
  if (!valid) {
    return std::unexpected(valid.error());
  }
  available = available.normalized();
  const auto inset = state_->area.frame ? 2 : 0;
  return CellSize{std::max(available.width - inset, 0), std::max(available.height - inset, 0)};
}

std::expected<CellSize, TreeError> ScrollArea::measureChildren(std::span<const MeasuredChild> children) const {
  auto valid = detail::validateScrollWidget(*state_);
  if (!valid) {
    return std::unexpected(valid.error());
  }
  if (children.size() > 1) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  CellSize size;
  if (!children.empty()) {
    size = {intrinsicAxis(children[0].desired.width, children[0].policy.width),
            intrinsicAxis(children[0].desired.height, children[0].policy.height)};
  }
  const auto inset = state_->area.frame ? 2 : 0;
  if (size.width > std::numeric_limits<std::int32_t>::max() - inset ||
      size.height > std::numeric_limits<std::int32_t>::max() - inset) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }
  state_->extent = {static_cast<std::uint64_t>(size.width), static_cast<std::uint64_t>(size.height)};
  return CellSize{std::max(size.width + inset, computedStyle() ? computedStyle()->minWidth : 0), size.height + inset};
}

std::expected<void, TreeError> ScrollArea::arrange(CellRect local) {
  return detail::arrangeScroll(*state_, local.size());
}

std::expected<CellRect, TreeError> ScrollArea::childClip(CellSize available) const {
  auto placement = detail::areaPlacement(*state_, available);
  if (!placement) {
    return std::unexpected(placement.error());
  }
  auto clip = CellRect::from(placement->viewportOrigin, placement->childClipSize);
  return clip ? std::expected<CellRect, TreeError>(*clip) : std::unexpected(TreeError::INVALID_GEOMETRY);
}

std::expected<std::vector<CellRect>, TreeError> ScrollArea::arrangeChildren(std::span<const MeasuredChild> children,
                                                                            CellSize available) const {
  if (children.size() > 1) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  if (children.empty()) {
    return std::vector<CellRect>{};
  }
  auto placement = detail::areaPlacement(*state_, available);
  auto observed = state_->controller->snapshot();
  if (!placement || !observed) {
    return std::unexpected(placement ? observed.error() : placement.error());
  }
  const auto horizontal = effectiveOffset(
    observed->horizontal.offset, {state_->extent.width, static_cast<std::uint64_t>(placement->viewport.width)});
  const auto vertical = effectiveOffset(
    observed->vertical.offset, {state_->extent.height, static_cast<std::uint64_t>(placement->viewport.height)});
  const auto x = static_cast<std::int64_t>(placement->viewportOrigin.x) - static_cast<std::int64_t>(horizontal.raster);
  const auto y = static_cast<std::int64_t>(placement->viewportOrigin.y) - static_cast<std::int64_t>(vertical.raster);
  if (x < std::numeric_limits<std::int32_t>::min() || y < std::numeric_limits<std::int32_t>::min() ||
      state_->extent.width > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) ||
      state_->extent.height > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }
  auto rectangle =
    CellRect::from({static_cast<std::int32_t>(x), static_cast<std::int32_t>(y)},
                   {static_cast<std::int32_t>(state_->extent.width), static_cast<std::int32_t>(state_->extent.height)});
  if (!rectangle) {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }
  try {
    return std::vector<CellRect>{*rectangle};
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<CellSize, TreeError> ScrollBar::measure(CellSize) {
  auto valid = detail::validateScrollWidget(*state_);
  return valid ? std::expected<CellSize, TreeError>(
                   CellSize{std::max(1, computedStyle() ? computedStyle()->minWidth : 0), 1})
               : std::unexpected(valid.error());
}

std::expected<void, TreeError> ScrollBar::arrange(CellRect local) {
  return detail::arrangeScroll(*state_, local.size());
}
} // namespace portals
