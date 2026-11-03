// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <portals/ui_controls.hpp>

namespace portals::ui {
/** @brief Cold per-Switch configuration; never retained by unrelated controls. */
struct SwitchOptions {
  bool readOnly{}; ///< Block user writes while retaining focus and programmatic bool access.
  std::string offText{DEFAULT_SWITCH_OFF_TEXT}; ///< Owning same-width single-line decoration.
  std::string onText{DEFAULT_SWITCH_ON_TEXT};   ///< Owning same-width single-line decoration.
  KeyTextResolver resolver{};                   ///< Optional caption resource resolver, called only during mount.
};

/// @cond INTERNAL
namespace detail {
template <class T>
concept ToggleCaption = std::convertible_to<T, std::string_view> || std::same_as<std::remove_cvref_t<T>, TextValue>;

template <ToggleCaption T> TextValue toggleCaption(T &&caption) {
  if constexpr (std::same_as<std::remove_cvref_t<T>, TextValue>) {
    return std::forward<T>(caption);
  } else {
    return TextValue(std::string_view(caption));
  }
}

inline Description<Switch> toggleRecipe(TextValue caption, std::optional<ManagedState<bool>> external,
                                        std::optional<bool> initial, SwitchOptions options) {
  auto description =
    Access::cold<Switch>([options](ManagedScene &scene, Node<Widget> parent,
                                   MountContext &context) -> std::expected<Node<Switch>, ManagedError> {
      const auto lifetime = Access::lifetime(scene);
      const auto policy = scene.widthPolicy();

      // Capture policy and lifetime before an optional user resolver can reenter.
      auto text = options.resolver ? context.controlSettings->text->resolve(policy, options.resolver)
                                   : context.controlSettings->text->resolve(policy);

      if (lifetime.expired()) {
        return std::unexpected(error(parent, TreeError::CLOSED));
      }

      if (!text) {
        if (const auto *cause = std::get_if<TextError>(&text.error())) {
          return std::unexpected(textError(parent, *cause));
        }
        auto failure = error(parent, TreeError::INVALID_ARGUMENT);
        failure.resource = std::get<TextResourceError>(text.error());
        return std::unexpected(failure);
      }

      auto chrome = SwitchChrome::from(options.offText, options.onText, policy);

      if (!chrome) {
        return std::unexpected(error(parent, chrome.error()));
      }

      return scene.emplace<Switch>(parent, std::move(*text), std::move(*chrome), ControlStyle{}, DEFAULT_CONTENT_POLICY,
                                   context.controlSettings->readOnly.value_or(options.readOnly));
    });
  description = Access::controlArguments(
    Access::configuredControl(std::move(description), "Switch"),
    ControlSettings{.text = std::move(caption),
                    .state =
                      initial ? std::optional(*initial ? CheckState::CHECKED : CheckState::UNCHECKED) : std::nullopt});
  return Access::controlSetup(
    std::move(description),
    [external](ManagedScene &scene, Node<Switch> node,
               const ControlSettings &settings) -> std::expected<void, ManagedError> {
      if (settings.state == CheckState::MIXED || settings.cycle == CheckCycle::THREE_STATE ||
          (external && settings.state)) {
        return std::unexpected(error(node, TreeError::INVALID_ARGUMENT));
      }

      if (external) {
        return scene.bind(Node<CheckBox>{node}, *external);
      }

      auto model = scene.state(settings.state.value_or(CheckState::UNCHECKED) == CheckState::CHECKED);

      return model ? scene.bind(Node<CheckBox>{node}, *model) : std::unexpected(model.error());
    });
}
} // namespace detail

/// @endcond

/** @brief Owning bool Switch recipe with explicit initial state above inherited defaults.
 * @tparam Caption Owning TextValue or bytes convertible to string_view.
 * @param caption Caption copied now. @param initial Initial bool. @param options Cold per-Switch settings.
 * @return Consuming owning recipe, validated under the receiving scene policy.
 */
template <detail::ToggleCaption Caption>
Description<Switch> toggle(Caption &&caption, bool initial, SwitchOptions options = {}) {
  return detail::toggleRecipe(detail::toggleCaption(std::forward<Caption>(caption)), {}, initial, std::move(options));
}

/** @brief Omitted initial state inherits context defaults, otherwise false.
 * @tparam Caption Owning TextValue or bytes convertible to string_view.
 * @param caption Caption copied now. @param options Cold per-Switch settings.
 * @return Consuming internally model-owned Switch recipe.
 */
template <detail::ToggleCaption Caption> Description<Switch> toggle(Caption &&caption, SwitchOptions options = {}) {
  return detail::toggleRecipe(detail::toggleCaption(std::forward<Caption>(caption)), {}, {}, std::move(options));
}

/** @brief Bind an authoritative bool state; explicit context/resource initial values are rejected.
 * @tparam Caption Owning TextValue or bytes convertible to string_view.
 * @param caption Caption copied now. @param model Same-scene authoritative bool handle.
 * @param options Cold per-Switch settings. @return Consuming recipe preserving the model value.
 */
template <detail::ToggleCaption Caption>
Description<Switch> toggle(Caption &&caption, ManagedState<bool> model, SwitchOptions options = {}) {
  return detail::toggleRecipe(detail::toggleCaption(std::forward<Caption>(caption)), model, std::nullopt,
                              std::move(options));
}

/** @brief Preserve a checked owner.state result without requiring a client unwrap staircase.
 * @tparam Caption Owning TextValue or bytes convertible to string_view.
 * @param caption Caption copied now. @param model Checked state result, preserving its original failure.
 * @param options Cold per-Switch settings. @return Consuming recipe with checked setup admission.
 */
template <detail::ToggleCaption Caption>
Description<Switch> toggle(Caption &&caption, std::expected<ManagedState<bool>, ManagedError> model,
                           SwitchOptions options = {}) {
  if (model) {
    return toggle(std::forward<Caption>(caption), *model, std::move(options));
  }

  return detail::Access::failed<Switch>(model.error());
}

/** @brief Publish the unified weak Ref only after the complete recipe mounts.
 * @tparam Caption Owning TextValue or bytes convertible to string_view.
 * @tparam Model Exactly bool, ManagedState<bool>, or its checked creation result.
 * @param reference Original weak destination slot. @param caption Caption copied now.
 * @param model Initial bool or authoritative same-scene state. @param options Cold per-Switch settings.
 * @return Consuming named Switch recipe, without client binding tokens.
 */
template <detail::ToggleCaption Caption, class Model>
  requires(std::same_as<std::remove_cvref_t<Model>, bool> ||
           std::same_as<std::remove_cvref_t<Model>, ManagedState<bool>> ||
           std::same_as<std::remove_cvref_t<Model>, std::expected<ManagedState<bool>, ManagedError>>)
Description<Switch> toggle(Ref<Switch> reference, Caption &&caption, Model model, SwitchOptions options = {}) {
  return toggle(std::forward<Caption>(caption), std::move(model), std::move(options)).reference(std::move(reference));
}

/** @brief Publish a reference with inherited/default false initial state.
 * @tparam Caption Owning TextValue or bytes convertible to string_view.
 * @param reference Original weak destination slot. @param caption Caption copied now.
 * @param options Cold per-Switch settings. @return Consuming internally model-owned recipe.
 */
template <detail::ToggleCaption Caption>
Description<Switch> toggle(Ref<Switch> reference, Caption &&caption, SwitchOptions options = {}) {
  return toggle(std::forward<Caption>(caption), std::move(options)).reference(std::move(reference));
}
} // namespace portals::ui
