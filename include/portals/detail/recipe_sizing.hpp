// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <optional>
#include <portals/layout.hpp>
#include <portals/ui_control_context.hpp>

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
  FIT_HEIGHT,
  AUTO_WIDTH,
  AUTO_HEIGHT
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
    case SizeDimension::AUTO_WIDTH:
    case SizeDimension::AUTO_HEIGHT:
      preferred = INTRINSIC_EXTENT;
      mode = SizeMode::AUTOMATIC;
      weight = 1;
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

  /** @param source Higher-priority owning leaves; absent leaves preserve this overlay. */
  constexpr void overlay(const RecipeSizing &source) noexcept {
    const auto mergeAxis = [](auto &target, const auto &patch) {
      if (!patch) {
        return;
      }

      if (!target) {
        target.emplace();
      }

      const auto assign = [](auto &destination, const auto &value) {
        if (value) {
          destination = value;
        }
      };

      assign(target->minimum, patch->minimum);
      assign(target->preferred, patch->preferred);
      assign(target->maximum, patch->maximum);
      assign(target->mode, patch->mode);
      assign(target->weight, patch->weight);
    };

    mergeAxis(width, source.width);
    mergeAxis(height, source.height);
  }

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
    case SizeDimension::AUTO_WIDTH:
      stageAxis(width);
      break;
    case SizeDimension::HEIGHT:
    case SizeDimension::FLEX_Y:
    case SizeDimension::MIN_HEIGHT:
    case SizeDimension::MAX_HEIGHT:
    case SizeDimension::FIT_HEIGHT:
    case SizeDimension::AUTO_HEIGHT:
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

/** @param settings Borrowed owning literals; no callback or view is retained.
 * @param excluded Source leaves replayed later; null projects every present leaf.
 * @return Sparse atomic policy projection; independent bounds follow the selected mode.
 */
inline RecipeSizing controlSizing(const ControlSettings &settings, const ControlSettings *excluded = nullptr) noexcept {
  RecipeSizing result;
  const auto stage = [&](SizeDimension dimension, auto value) {
    if (value) {
      result.stage({dimension, static_cast<std::int32_t>(*value)}, true);
    }
  };
  const auto flex = [&](SizeDimension dimension, std::optional<std::uint32_t> value) {
    if (value) {
      result.stage({dimension, 0, *value}, true);
    }
  };

  const bool excludeWidth = excluded && (excluded->width || excluded->fitWidth || excluded->flexX || excluded->flex);
  const bool excludeHeight = excluded && (excluded->height || excluded->fitHeight || excluded->flexY || excluded->flex);

  if (!excludeWidth) {
    stage(SizeDimension::WIDTH, settings.width);
    flex(SizeDimension::FLEX_X, settings.flexX ? settings.flexX : settings.flex);

    if (settings.fitWidth) {
      result.stage({*settings.fitWidth ? SizeDimension::FIT_WIDTH : SizeDimension::AUTO_WIDTH}, true);
    }
  }

  if (!excludeHeight) {
    stage(SizeDimension::HEIGHT, settings.height);
    flex(SizeDimension::FLEX_Y, settings.flexY ? settings.flexY : settings.flex);

    if (settings.fitHeight) {
      result.stage({*settings.fitHeight ? SizeDimension::FIT_HEIGHT : SizeDimension::AUTO_HEIGHT}, true);
    }
  }

  const auto bound = [](auto &axis, auto value, bool minimum, bool exclude) {
    if (!value || exclude) {
      return;
    }

    if (!axis) {
      axis.emplace();
    }

    auto &destination = minimum ? axis->minimum : axis->maximum;

    destination = !destination ? *value : minimum ? std::max(*destination, *value) : std::min(*destination, *value);
  };

  bound(result.width, settings.minWidth, true, excluded && excluded->minWidth.has_value());
  bound(result.width, settings.maxWidth, false, excluded && excluded->maxWidth.has_value());
  bound(result.height, settings.minHeight, true, excluded && excluded->minHeight.has_value());
  bound(result.height, settings.maxHeight, false, excluded && excluded->maxHeight.has_value());

  return result;
}

/// @endcond
} // namespace portals::ui::detail
