// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <optional>
#include <portals/layout.hpp>

namespace portals::ui::detail {
/// @cond INTERNAL
/** @brief Owning cold geometry operation; no widget or callback reference. */
enum class SizeDimension {
  WIDTH,
  HEIGHT,
  FLEX_X,
  FLEX_Y,
  FLEX,
  MIN_WIDTH,
  MAX_WIDTH,
  MIN_HEIGHT,
  MAX_HEIGHT,
  FIT_WIDTH,
  FIT_HEIGHT
};

struct SizeChange {
  SizeDimension dimension; ///< Selected axis and operation.
  std::int32_t extent{};   ///< Fixed extent or individual bound, in cells.
  std::uint32_t weight{1}; ///< Positive weight for flex operations.
};

/** @brief Sparse explicit axis leaves, validated together after ordinary setup. */
struct RecipeAxisSize {
  std::optional<std::int32_t> minimum;
  std::optional<std::int32_t> preferred;
  std::optional<std::int32_t> maximum;
  std::optional<SizeMode> mode;
  std::optional<std::uint32_t> weight;

  constexpr void stage(SizeChange change) noexcept {
    switch (change.dimension) {
    case SizeDimension::WIDTH:
    case SizeDimension::HEIGHT:
      minimum = preferred = maximum = change.extent;
      mode = SizeMode::FIXED;
      weight = 1;
      break;
    case SizeDimension::FLEX_X:
    case SizeDimension::FLEX_Y:
    case SizeDimension::FLEX:
      preferred = 0;
      mode = SizeMode::FLEX;
      weight = change.weight;
      break;
    case SizeDimension::MIN_WIDTH:
    case SizeDimension::MIN_HEIGHT:
      minimum = change.extent;
      break;
    case SizeDimension::MAX_WIDTH:
    case SizeDimension::MAX_HEIGHT:
      maximum = change.extent;
      break;
    case SizeDimension::FIT_WIDTH:
    case SizeDimension::FIT_HEIGHT:
      preferred = INTRINSIC_EXTENT;
      mode = SizeMode::CONTENT;
      break;
    }
  }

  constexpr AxisSize apply(AxisSize value) const noexcept {
    if (minimum) {
      value.minimum = *minimum;
    }

    if (preferred) {
      value.preferred = *preferred;
    }

    if (maximum) {
      value.maximum = *maximum;
    }

    if (mode) {
      value.mode = *mode;
    }

    if (weight) {
      value.weight = *weight;
    }

    return value;
  }
};

/** @brief Optional recipe-only overlay; old recipes do not allocate it. */
struct RecipeSizing {
  std::optional<RecipeAxisSize> width;
  std::optional<RecipeAxisSize> height;

  constexpr void stage(SizeChange change, bool activate) noexcept {
    const auto stageAxis = [&](std::optional<RecipeAxisSize> &axis) {
      if (activate && !axis) {
        axis.emplace();
      }

      if (axis) {
        axis->stage(change);
      }
    };

    switch (change.dimension) {
    case SizeDimension::WIDTH:
    case SizeDimension::FLEX_X:
    case SizeDimension::MIN_WIDTH:
    case SizeDimension::MAX_WIDTH:
    case SizeDimension::FIT_WIDTH:
      stageAxis(width);
      break;
    case SizeDimension::HEIGHT:
    case SizeDimension::FLEX_Y:
    case SizeDimension::MIN_HEIGHT:
    case SizeDimension::MAX_HEIGHT:
    case SizeDimension::FIT_HEIGHT:
      stageAxis(height);
      break;
    case SizeDimension::FLEX:
      stageAxis(width);
      stageAxis(height);
      break;
    }
  }

  constexpr SizePolicy apply(SizePolicy value) const noexcept {
    if (width) {
      value.width = width->apply(value.width);
    }

    if (height) {
      value.height = height->apply(value.height);
    }

    return value;
  }
};

/// @endcond
} // namespace portals::ui::detail
