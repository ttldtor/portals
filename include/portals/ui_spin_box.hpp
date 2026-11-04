// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <portals/ui_slider.hpp>

namespace portals::ui {
namespace detail {
inline Description<SpinBox> spinBoxRecipe(ControlSettings arguments, std::optional<NumericState> model = {}) {
  auto description =
    Access::cold<SpinBox>([model](ManagedScene &scene, Node<Widget> parent,
                                  MountContext &context) -> std::expected<Node<SpinBox>, ManagedError> {
      const auto &settings = *context.controlSettings;
      if (model && numericOverride(settings)) {
        return std::unexpected(numericSettingsError(parent, NumericError::INITIAL_CONFLICT, settings, "SpinBox"));
      }
      if (model) {
        auto current = model->snapshot();
        if (!current) {
          return std::unexpected(current.error());
        }
      } else {
        auto initial = sliderInitial(settings);
        if (!initial) {
          return std::unexpected(numericSettingsError(parent, initial.error(), settings, "SpinBox"));
        }
      }
      return scene.emplace<SpinBox>(parent);
    });
  return Access::controlSetup(
    Access::controlArguments(Access::configuredControl(std::move(description), "SpinBox"), std::move(arguments)),
    [model](ManagedScene &scene, Node<SpinBox> node,
            const ControlSettings &settings) -> std::expected<void, ManagedError> {
      auto options = controlLineEditOptions(settings, {}, scene.widthPolicy(), {}, node);
      if (!options) {
        return std::unexpected(options.error());
      }
      std::expected<void, ManagedError> bound;
      if (model) {
        bound = scene.bind(node, *model, *options, settings.pageSteps.value_or(10));
      } else {
        auto initial = sliderInitial(settings);
        if (!initial) {
          return std::unexpected(numericSettingsError(node, initial.error(), settings, "SpinBox"));
        }
        auto created = scene.numericState(initial->value(), initial->range());
        if (!created) {
          return std::unexpected(created.error());
        }
        bound = scene.bind(node, *created, *options, settings.pageSteps.value_or(10));
      }
      if (!bound || !settings.text) {
        return bound;
      }
      Ref<SpinBox> reference;
      auto published = NamedAccess::publish(reference, scene, node);
      if (!published) {
        return published;
      }
      return reference.text().set(*settings.text).result();
    });
}
} // namespace detail

/** @return Unbound default 0..100 spinBox; only an omitted initial value can fall back into a changed final range. */
inline Description<SpinBox> spinBox() {
  return detail::spinBoxRecipe({});
}

/** @param initial Explicit finite initial value; checked against the final configured bounds without clamping.
 * @return Single-use numeric recipe with scene-owned validated state.
 */
inline Description<SpinBox> spinBox(double initial) {
  return detail::spinBoxRecipe({.value = initial});
}

/** @param initial Explicit finite initial value in the inclusive supplied bounds.
 * @param range Complete numeric source configuration; later fluent leaves have ordinary final precedence.
 * @return Unbound recipe staging its complete validated numeric source.
 */
inline Description<SpinBox> spinBox(double initial, NumericRange range) {
  return detail::spinBoxRecipe(
    {.value = initial, .minimum = range.minimum, .maximum = range.maximum, .step = range.step});
}

/** @tparam Model Exactly NumericState, deduced so braces continue to select an explicit numeric initial value.
 * @param model Weak state from the receiving scene; explicit numeric overrides are competing-source errors.
 * @return Recipe sharing the existing validated numeric source without copying or overwriting it.
 */
template <class Model>
  requires std::same_as<std::remove_cvref_t<Model>, NumericState>
Description<SpinBox> spinBox(Model &&model) {
  return detail::spinBoxRecipe({}, std::forward<Model>(model));
}

/** @tparam Model Exactly the checked NumericState creation result.
 * @param model Checked model whose complete original failure is preserved before any node work.
 * @return Numeric source recipe, or a cold failed recipe with the original managed cause.
 */
template <class Model>
  requires std::same_as<std::remove_cvref_t<Model>, std::expected<NumericState, ManagedError>>
Description<SpinBox> spinBox(Model &&model) {
  return model ? spinBox(*model) : detail::Access::failed<SpinBox>(model.error());
}

/** @tparam Reference Exactly a SpinBox Ref; deduced to preserve value-first brace resolution.
 * @tparam Args Ordinary spinBox factory arguments.
 * @param reference Weak slot published only after the complete mount succeeds.
 * @param args Omitted/explicit initial value, numeric range or checked external state.
 * @return The ordinary single-use SpinBox recipe with its transactional observing slot.
 */
template <class Reference, class... Args>
  requires(std::same_as<std::remove_cvref_t<Reference>, Ref<SpinBox>> &&
           requires(Args &&...args) { spinBox(std::forward<Args>(args)...); })
Description<SpinBox> spinBox(Reference &&reference, Args &&...args) {
  return spinBox(std::forward<Args>(args)...).reference(std::forward<Reference>(reference));
}

/** @tparam Reference Exactly a SpinBox Ref, deduced before interpreting a braced initial value.
 * @param reference Transactional weak slot. @param initial Explicit finite initial value.
 * @return The ordinary strict initial-value recipe, including for an empty numeric brace initializer.
 */
template <class Reference>
  requires std::same_as<std::remove_cvref_t<Reference>, Ref<SpinBox>>
Description<SpinBox> spinBox(Reference &&reference, double initial) {
  return spinBox(initial).reference(std::forward<Reference>(reference));
}

/** @tparam Reference Exactly a SpinBox Ref.
 * @param reference Transactional weak slot. @param initial Explicit initial value. @param range Complete source bounds.
 * @return Strict initial-value recipe with an explicit numeric configuration.
 */
template <class Reference>
  requires std::same_as<std::remove_cvref_t<Reference>, Ref<SpinBox>>
Description<SpinBox> spinBox(Reference &&reference, double initial, NumericRange range) {
  return spinBox(initial, range).reference(std::forward<Reference>(reference));
}
} // namespace portals::ui
