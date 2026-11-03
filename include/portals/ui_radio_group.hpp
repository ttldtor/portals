// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_construction.hpp>

namespace portals::ui {
namespace detail {
/// @cond INTERNAL
/** @brief Internal cold recipe; captions resolve once using the receiving scene's policy. */
inline Description<RadioGroup> radioRecipe(std::vector<RadioOption> captions,
                                           std::optional<ManagedState<RadioGroupValue>> model,
                                           std::optional<ManagedError> failure, KeyTextResolver resolver,
                                           ControlStyle style, SizePolicy policy, CollectionLimits limits) {
  auto recipe = Access::cold<RadioGroup>(
    [captions = std::move(captions), model, failure = std::move(failure), resolver = std::move(resolver), style, policy,
     limits](ManagedScene &scene, Node<Widget> parent,
             MountContext &context) mutable -> std::expected<Node<RadioGroup>, ManagedError> {
      if (failure) {
        return std::unexpected(*failure);
      }

      const auto &initial = context.controlSettings->radioInitial;
      if ((model && (initial.enabled || initial.index)) || (initial.enabled == true && !initial.index) ||
          (initial.enabled == false && initial.index)) {
        return std::unexpected(error(parent, TreeError::INVALID_ARGUMENT));
      }

      auto state = model ? std::expected<ManagedState<RadioGroupValue>, ManagedError>(*model)
                         : scene.radioState(captions, resolver, limits);
      if (!state) {
        return std::unexpected(state.error());
      }

      if (!model && (initial.enabled || initial.index)) {
        auto snapshot = state->snapshot();
        if (!snapshot) {
          return std::unexpected(error(parent, snapshot.error()));
        }
        const auto &value = (*snapshot)->value;
        if (initial.index && *initial.index >= value.rows().size()) {
          return std::unexpected(error(parent, TreeError::INVALID_ARGUMENT));
        }
        const auto id =
          initial.index ? std::optional(value.rows()[static_cast<std::size_t>(*initial.index)].id) : std::nullopt;
        auto candidate = value.withSelection(id);
        auto updated = state->set(std::move(*candidate));
        if (!updated) {
          return std::unexpected(error(parent, updated.error()));
        }
      }

      auto node = scene.emplace<RadioGroup>(parent, style, policy, context.controlSettings->readOnly.value_or(false),
                                            scene.widthPolicy());
      if (!node) {
        return node;
      }

      auto bound = scene.bind(*node, *state);

      return bound ? node : std::unexpected(bound.error());
    });

  return Access::configuredControl(std::move(recipe), "RadioGroup");
}

/// @endcond
} // namespace detail

/** @param captions Ordinary UTF-8 labels copied now and validated at mount; automatic stable IDs.
 * @param style Manual appearances. @param policy Cell sizing. @return First-eligible selected scene-owned group.
 * Empty input has no selection. No binding token or terminal cleanup escapes the scene.
 */
inline Description<RadioGroup> radioGroup(std::initializer_list<std::string_view> captions, ControlStyle style = {},
                                          SizePolicy policy = DEFAULT_CONTENT_POLICY) {
  std::vector<RadioOption> values(std::initializer_list<RadioOption>{});

  values.reserve(captions.size());
  for (auto caption : captions) {
    values.emplace_back(caption);
  }

  return detail::radioRecipe(std::move(values), {}, {}, {}, style, policy, {});
}

/** @param reference Unified weak observation, published after mount. @param captions Owning label initializer.
 * @param style Manual appearances. @param policy Ordinary sizing. @return Single-use radio recipe.
 */
inline Description<RadioGroup> radioGroup(Ref<RadioGroup> reference, std::initializer_list<std::string_view> captions,
                                          ControlStyle style = {}, SizePolicy policy = DEFAULT_CONTENT_POLICY) {
  return radioGroup(captions, style, policy).reference(std::move(reference));
}

/** @param captions Owning literal/i18n expressions. @param resolver Explicit synchronous localization adapter.
 * @param style Manual appearances. @param policy Ordinary sizing. @param limits Resident collection cap.
 * @return Cold recipe; no resolver, text validation or model operation runs in paint/layout.
 */
inline Description<RadioGroup> radioGroup(std::vector<TextValue> captions, KeyTextResolver resolver,
                                          ControlStyle style = {}, SizePolicy policy = DEFAULT_CONTENT_POLICY,
                                          CollectionLimits limits = {}) {
  std::vector<RadioOption> options(std::initializer_list<RadioOption>{});
  options.reserve(captions.size());
  for (const auto &caption : captions) {
    options.emplace_back(caption);
  }
  return detail::radioRecipe(std::move(options), {}, {}, std::move(resolver), style, policy, limits);
}

/** @param options Owning captions and per-option eligibility. @param resolver Optional localization adapter.
 * @param style Manual appearances. @param policy Cell sizing. @param limits Existing resident collection cap.
 * @return Cold scene-owned radio model with no mandatory IDs or client binding token.
 */
inline Description<RadioGroup> radioGroup(std::vector<RadioOption> options, KeyTextResolver resolver = {},
                                          ControlStyle style = {}, SizePolicy policy = DEFAULT_CONTENT_POLICY,
                                          CollectionLimits limits = {}) {
  return detail::radioRecipe(std::move(options), {}, {}, std::move(resolver), style, policy, limits);
}

/** @param model Authoritative state from the receiving scene; mount never writes a default.
 * @param style Manual appearances. @param policy Ordinary sizing. @return Model-backed single-use recipe.
 */
inline Description<RadioGroup> radioGroup(ManagedState<RadioGroupValue> model, ControlStyle style = {},
                                          SizePolicy policy = DEFAULT_CONTENT_POLICY) {
  return detail::radioRecipe(std::vector<RadioOption>(std::initializer_list<RadioOption>{}), model, {}, {}, style,
                             policy, {});
}

/** @param reference Unified weak observation. @param model Authoritative state.
 * @param style Manual appearances. @param policy Sizing. @return Bound recipe preserving the model's exact value.
 */
inline Description<RadioGroup> radioGroup(Ref<RadioGroup> reference, ManagedState<RadioGroupValue> model,
                                          ControlStyle style = {}, SizePolicy policy = DEFAULT_CONTENT_POLICY) {
  return radioGroup(model, style, policy).reference(std::move(reference));
}

/** @param reference Weak observation. @param model Checked scene.radioState result, retaining its original error.
 * @return Compact model-backed recipe without client-side expected adapters.
 */
inline Description<RadioGroup> radioGroup(Ref<RadioGroup> reference,
                                          std::expected<ManagedState<RadioGroupValue>, ManagedError> model) {
  return detail::radioRecipe(std::vector<RadioOption>(std::initializer_list<RadioOption>{}),
                             model ? std::optional(*model) : std::nullopt,
                             model ? std::nullopt : std::optional(model.error()), {}, {}, DEFAULT_CONTENT_POLICY, {})
    .reference(std::move(reference));
}
} // namespace portals::ui
