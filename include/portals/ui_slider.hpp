// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_controls.hpp>

#include <algorithm>
#include <array>

namespace portals::ui {
namespace detail {
inline ManagedError numericError(Node<Widget> node, NumericError cause) {
  auto result = error(node, TreeError::INVALID_ARGUMENT);

  result.numeric = cause;

  return result;
}

inline ManagedError numericSettingsError(Node<Widget> node, NumericError cause, const ControlSettings &settings) {
  std::array<std::string_view, 3> candidates{"step", "minimum", "maximum"};

  if (cause == NumericError::INVALID_RANGE) {
    candidates = {"maximum", "minimum", {}};
  } else if (cause == NumericError::OUT_OF_RANGE) {
    const bool below = settings.value && *settings.value < settings.minimum.value_or(NumericRange{}.minimum);

    candidates = {"value", below ? "minimum" : "maximum", {}};
  }

  if (cause == NumericError::INITIAL_CONFLICT) {
    const auto supplied = std::ranges::find_if(settings.origins, [](const ControlPropertyOrigin &origin) {
      return origin.source &&
             (origin.path == "value" || origin.path == "minimum" || origin.path == "maximum" || origin.path == "step");
    });

    if (supplied != settings.origins.end()) {
      candidates = {supplied->path, {}, {}};
    }
  }

  std::string_view property;
  std::shared_ptr<const ControlSourceOrigin> source;

  for (const auto candidate : candidates) {
    const auto supplied = std::ranges::find(settings.origins, candidate, &ControlPropertyOrigin::path);

    if (supplied != settings.origins.end() && supplied->source) {
      property = candidate;
      source = supplied->source;

      break;
    }
  }

  if (!source) {
    return numericError(node, cause);
  }

  auto diagnostic = std::make_shared<ControlContextDiagnostic>();

  diagnostic->path = "Slider." + std::string(property);
  diagnostic->origin = std::move(source);
  auto result = controlContextError({ControlContextErrorCode::INVALID_VALUE, std::move(diagnostic)}, node);

  result.numeric = cause;

  return result;
}

inline std::expected<NumericValue, NumericError> sliderInitial(const ControlSettings &settings) {
  NumericRange range;

  range.minimum = settings.minimum.value_or(range.minimum);
  range.maximum = settings.maximum.value_or(range.maximum);
  range.step = settings.step.value_or(range.step);

  // Only the omitted constructor value is a fallback. Every present value stays strict.
  const auto initial =
    settings.value.value_or(range.minimum <= range.maximum ? std::clamp(0.0, range.minimum, range.maximum) : 0.0);

  return NumericValue::create(initial, range);
}

inline SliderOptions sliderOptions(const ControlSettings &settings) {
  SliderOptions options;

  options.preferredWidth = settings.preferredWidth.value_or(options.preferredWidth);
  options.maxPaintCells = settings.maxPaintCells.value_or(options.maxPaintCells);
  options.pageSteps = settings.pageSteps.value_or(options.pageSteps);
  options.readOnly = settings.readOnly.value_or(options.readOnly);

  return options;
}

inline bool numericOverride(const ControlSettings &settings) noexcept {
  return settings.value || settings.minimum || settings.maximum || settings.step;
}

inline Description<Slider> sliderRecipe(ControlSettings arguments, std::optional<NumericState> model = {}) {
  auto description = Access::cold<Slider>([model](ManagedScene &scene, Node<Widget> parent,
                                                  MountContext &context) -> std::expected<Node<Slider>, ManagedError> {
    const auto &settings = *context.controlSettings;

    if (model) {
      if (numericOverride(settings)) {
        return std::unexpected(numericSettingsError(parent, NumericError::INITIAL_CONFLICT, settings));
      }

      auto current = model->snapshot();

      if (!current) {
        return std::unexpected(current.error());
      }
    } else {
      auto initial = sliderInitial(settings);

      if (!initial) {
        return std::unexpected(numericSettingsError(parent, initial.error(), settings));
      }
    }

    return scene.emplace<Slider>(parent, scene.widthPolicy(), sliderOptions(settings));
  });

  return Access::controlSetup(
    Access::controlArguments(Access::configuredControl(std::move(description), "Slider"), std::move(arguments)),
    [model](ManagedScene &scene, Node<Slider> node,
            const ControlSettings &settings) -> std::expected<void, ManagedError> {
      if (model) {
        if (numericOverride(settings)) {
          return std::unexpected(numericSettingsError(node, NumericError::INITIAL_CONFLICT, settings));
        }

        return scene.bind(node, *model);
      }

      auto initial = sliderInitial(settings);

      if (!initial) {
        return std::unexpected(numericSettingsError(node, initial.error(), settings));
      }

      auto created = scene.numericState(initial->value(), initial->range());

      return created ? scene.bind(node, *created) : std::unexpected(created.error());
    });
}
} // namespace detail

/** @return Unbound default 0..100 slider; only an omitted initial value can fall back into a changed final range. */
inline Description<Slider> slider() {
  return detail::sliderRecipe({});
}

/** @param initial Explicit finite initial value; checked against the final configured bounds without clamping.
 * @return Single-use numeric recipe with scene-owned validated state.
 */
inline Description<Slider> slider(double initial) {
  return detail::sliderRecipe({.value = initial});
}

/** @param initial Explicit finite initial value in the inclusive supplied bounds.
 * @param range Complete numeric source configuration; later fluent leaves have ordinary final precedence.
 * @return Unbound recipe staging its complete validated numeric source.
 */
inline Description<Slider> slider(double initial, NumericRange range) {
  return detail::sliderRecipe(
    {.value = initial, .minimum = range.minimum, .maximum = range.maximum, .step = range.step});
}

/** @tparam Model Exactly NumericState, deduced so braces continue to select an explicit numeric initial value.
 * @param model Weak state from the receiving scene; explicit numeric overrides are competing-source errors.
 * @return Recipe sharing the existing validated numeric source without copying or overwriting it.
 */
template <class Model>
  requires std::same_as<std::remove_cvref_t<Model>, NumericState>
Description<Slider> slider(Model &&model) {
  return detail::sliderRecipe({}, std::forward<Model>(model));
}

/** @tparam Model Exactly the checked NumericState creation result.
 * @param model Checked model whose complete original failure is preserved before any node work.
 * @return Numeric source recipe, or a cold failed recipe with the original managed cause.
 */
template <class Model>
  requires std::same_as<std::remove_cvref_t<Model>, std::expected<NumericState, ManagedError>>
Description<Slider> slider(Model &&model) {
  return model ? slider(*model) : detail::Access::failed<Slider>(model.error());
}

/** @tparam Reference Exactly a Slider Ref; deduced to preserve value-first brace resolution.
 * @tparam Args Ordinary slider factory arguments.
 * @param reference Weak slot published only after the complete mount succeeds.
 * @param args Omitted/explicit initial value, numeric range or checked external state.
 * @return The ordinary single-use Slider recipe with its transactional observing slot.
 */
template <class Reference, class... Args>
  requires(std::same_as<std::remove_cvref_t<Reference>, Ref<Slider>> &&
           requires(Args &&...args) { slider(std::forward<Args>(args)...); })
Description<Slider> slider(Reference &&reference, Args &&...args) {
  return slider(std::forward<Args>(args)...).reference(std::forward<Reference>(reference));
}

/** @tparam Reference Exactly a Slider Ref, deduced before interpreting a braced initial value.
 * @param reference Transactional weak slot. @param initial Explicit finite initial value.
 * @return The ordinary strict initial-value recipe, including for an empty numeric brace initializer.
 */
template <class Reference>
  requires std::same_as<std::remove_cvref_t<Reference>, Ref<Slider>>
Description<Slider> slider(Reference &&reference, double initial) {
  return slider(initial).reference(std::forward<Reference>(reference));
}

/** @tparam Reference Exactly a Slider Ref.
 * @param reference Transactional weak slot. @param initial Explicit initial value. @param range Complete source bounds.
 * @return Strict initial-value recipe with an explicit numeric configuration.
 */
template <class Reference>
  requires std::same_as<std::remove_cvref_t<Reference>, Ref<Slider>>
Description<Slider> slider(Reference &&reference, double initial, NumericRange range) {
  return slider(initial, range).reference(std::forward<Reference>(reference));
}
} // namespace portals::ui
