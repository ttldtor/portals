// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <algorithm>
#include <portals/grid.hpp>

namespace portals::grid_detail {
using Wide = std::int64_t;
inline constexpr Wide cell_limit = std::numeric_limits<std::int32_t>::max();

inline bool valid(LayoutAlign align) {
  return align == LayoutAlign::start || align == LayoutAlign::center || align == LayoutAlign::end ||
         align == LayoutAlign::stretch;
}

inline bool valid(LayoutPadding padding) {
  return padding.left >= 0 && padding.top >= 0 && padding.right >= 0 && padding.bottom >= 0;
}

inline bool valid(AxisSize policy) {
  return policy.minimum >= 0 && policy.maximum >= policy.minimum && policy.preferred >= intrinsic_extent &&
         policy.weight > 0 &&
         (policy.mode == SizeMode::automatic || policy.mode == SizeMode::fixed || policy.mode == SizeMode::flex);
}

inline bool valid(const MeasuredChild &child) {
  return valid(child.policy.width) && valid(child.policy.height) && child.desired.width >= 0 &&
         child.desired.height >= 0;
}

inline Wide requested(AxisSize policy, std::int32_t intrinsic) {
  return std::clamp<Wide>(policy.preferred == intrinsic_extent ? intrinsic : policy.preferred, policy.minimum,
                          policy.maximum);
}

inline std::expected<CellSize, TreeError> size(Wide width, Wide height) {
  if (width < 0 || height < 0 || width > cell_limit || height > cell_limit) {
    return std::unexpected(TreeError::invalid_geometry);
  }

  return CellSize{static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
}

inline std::expected<CellSize, TreeError> inset(CellSize available, LayoutPadding padding) {
  if (!valid(padding)) {
    return std::unexpected(TreeError::invalid_argument);
  }

  available = available.normalized();

  return size(std::max<Wide>(0, Wide{available.width} - padding.left - padding.right),
              std::max<Wide>(0, Wide{available.height} - padding.top - padding.bottom));
}

inline std::expected<CellSize, TreeError> padded(CellSize content, LayoutPadding padding) {
  return size(Wide{content.width} + padding.left + padding.right, Wide{content.height} + padding.top + padding.bottom);
}

inline Wide extent(AxisSize policy, std::int32_t intrinsic, std::int32_t available, LayoutAlign align) {
  const auto wanted = requested(policy, intrinsic);

  if (policy.mode == SizeMode::fixed) {
    return wanted;
  }

  return std::clamp<Wide>(
    (align == LayoutAlign::stretch || policy.mode == SizeMode::flex) ? available : std::min<Wide>(wanted, available),
    policy.minimum, policy.maximum);
}

inline Wide offset(Wide space, Wide child, LayoutAlign align) {
  const auto spare = std::max<Wide>(0, space - child);

  if (align == LayoutAlign::center) {
    return spare / 2;
  }

  return align == LayoutAlign::end ? spare : 0;
}

inline std::expected<CellRect, TreeError> place(const MeasuredChild &child, CellRect slot, LayoutAlign horizontal,
                                                LayoutAlign vertical) {
  if (!valid(child) || !valid(horizontal) || !valid(vertical)) {
    return std::unexpected(TreeError::invalid_argument);
  }

  const auto width = extent(child.policy.width, child.desired.width, slot.size().width, horizontal);
  const auto height = extent(child.policy.height, child.desired.height, slot.size().height, vertical);
  const auto origin = size(Wide{slot.origin().x} + offset(slot.size().width, width, horizontal),
                           Wide{slot.origin().y} + offset(slot.size().height, height, vertical));
  const auto dimensions = size(width, height);

  if (!origin || !dimensions) {
    return std::unexpected(TreeError::invalid_geometry);
  }

  auto result = CellRect::from({origin->width, origin->height}, *dimensions);

  if (!result) {
    return std::unexpected(TreeError::invalid_geometry);
  }

  return *result;
}
} // namespace portals::grid_detail
