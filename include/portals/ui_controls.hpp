// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_construction.hpp>

namespace portals::ui {

/** @brief Construct a standard text-control recipe with the scene's width policy.
 * @tparam T Label, Button or CheckBox-compatible text constructor.
 * @param text UTF-8 bytes copied now; invalid text returns INVALID_ARGUMENT at mount.
 * @param style Owning manual control appearances.
 * @param policy Owning layout constraints.
 * @return Single-use owning recipe; allocation while copying may throw.
 */
template <class T> Description<T> control(std::string_view text, ControlStyle style, SizePolicy policy) {
  auto description =
    detail::Access::cold<T>([style, policy](ManagedScene &scene, Node<Widget> parent, detail::MountContext &context) {
      auto value = context.controlSettings->text->resolve(scene.widthPolicy());

      if (!value) {
        auto failure = detail::error(parent, TreeError::INVALID_ARGUMENT);
        if (const auto *textError = std::get_if<TextError>(&value.error())) {
          failure = detail::textError(parent, *textError);
        } else {
          failure.resource = std::get<TextResourceError>(value.error());
        }

        return std::expected<Node<T>, ManagedError>(std::unexpected(failure));
      }

      if constexpr (std::same_as<T, Button>) {
        return scene.emplace<T>(parent, std::move(*value), style, policy,
                                context.controlSettings->chrome.value_or(ButtonChrome::BRACKETS));
      } else {
        return scene.emplace<T>(parent, std::move(*value), style, policy);
      }
    });
  return detail::Access::controlArguments(
    detail::Access::configuredControl(std::move(description), detail::Access::controlType<T>()),
    ControlSettings{.text = TextValue(text)});
}

/** @param text UTF-8 copied immediately, validated at mount under the scene's width policy.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return Single-use label recipe.
 */
inline Description<Label> label(std::string_view text, ControlStyle style = {}, SizePolicy policy = {}) {
  return control<Label>(text, style, policy);
}

namespace detail {
/** @brief Resolve an owning caption expression once inside the existing mount transaction.
 * @tparam T Existing text control constructed with text, style and size policy.
 * @param text Owning literal, format result or localization key.
 * @param style Manual appearances copied into the recipe.
 * @param policy Owning layout constraints.
 * @param resolver Owning explicit localization adapter; no locale subscription is created.
 * @param chrome Button constructor decoration; sparse chrome settings override it for Button only.
 * @return Single-use caption recipe with original text/resource errors.
 */
template <class T>
Description<T> textControl(TextValue text, ControlStyle style, SizePolicy policy, TextResolver resolver,
                           ButtonChrome chrome = ButtonChrome::BRACKETS) {
  auto arguments = ControlSettings{.text = std::move(text)};
  auto description = Access::cold<T>(
    [style, policy, chrome, resolver = std::move(resolver)](
      ManagedScene &scene, Node<Widget> parent, MountContext &context) -> std::expected<Node<T>, ManagedError> {
      const auto &caption = *context.controlSettings->text;
      auto value = resolver ? caption.resolve(scene.widthPolicy(), resolver) : caption.resolve(scene.widthPolicy());

      if (!value) {
        if (const auto *failure = std::get_if<TextError>(&value.error())) {
          return std::unexpected(textError(parent, *failure));
        }

        auto failure = error(parent, TreeError::INVALID_ARGUMENT);

        failure.resource = std::get<TextResourceError>(value.error());

        return std::unexpected(failure);
      }

      if constexpr (std::same_as<T, Button>) {
        return scene.emplace<T>(parent, std::move(*value), style, policy,
                                context.controlSettings->chrome.value_or(chrome));
      } else {
        return scene.emplace<T>(parent, std::move(*value), style, policy);
      }
    });
  return Access::controlArguments(Access::configuredControl(std::move(description), Access::controlType<T>()),
                                  std::move(arguments));
}
/** @brief Exact deferred caption inputs; literals and bare braces keep their existing overloads. */
template <class T>
concept CaptionSource =
  std::same_as<std::remove_cvref_t<T>, TextSource> || std::same_as<std::remove_cvref_t<T>, TextProperty>;

template <class T> Description<T> liveCaption(TextSource source, ControlStyle style, SizePolicy policy) {
  auto description = Access::cold<T>(
    [source = std::move(source), style, policy](ManagedScene &scene, Node<Widget> parent,
                                                MountContext &context) -> std::expected<Node<T>, ManagedError> {
      auto admitted = Access::check(scene, parent);
      if (!admitted) {
        return std::unexpected(admitted.error());
      }
      auto graph = std::make_shared<Access::ColdGraph>();
      graph->scene = &scene;
      context.rollback.push_back([graph] {
        graph->retire();
      });
      graph->pending = true;
      Access::beginCold(scene);
      auto empty = GraphemeText::from("", scene.widthPolicy());
      if (!empty) {
        return std::unexpected(textError(parent, empty.error()));
      }
      auto node = [&] {
        if constexpr (std::same_as<T, Button>) {
          return scene.emplace<T>(parent, std::move(*empty), style, policy,
                                  context.controlSettings->chrome.value_or(ButtonChrome::BRACKETS));
        } else {
          return scene.emplace<T>(parent, std::move(*empty), style, policy);
        }
      }();
      if (!node) {
        return node;
      }
      if (context.root == Node<Widget>{}) {
        context.root = *node;
      }
      context.captions.push_back(
        {*node, Access::captionIdentity(source), false, [&scene, &context, node = *node, source, graph] {
           return Access::completeCaption(scene, node, source, context, graph);
         }});
      context.finalize.push_back([&context] {
        return Access::finishCaptions(context);
      });
      return node;
    });
  return Access::configuredControl(std::move(description), Access::controlType<T>());
}

template <CaptionSource Source> TextSource captionSource(Source &&source) {
  if constexpr (std::same_as<std::remove_cvref_t<Source>, TextProperty>) {
    return source.source();
  } else {
    return std::forward<Source>(source);
  }
}
} // namespace detail

/** @tparam Source Exact TextSource or TextProperty observation, read after the complete mount.
 * @param source Visible caption/draft or accepted model identity; never retains a widget.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Live one-way label recipe; cycles reject mount and removed sources never retarget.
 */
template <detail::CaptionSource Source>
Description<Label> label(Source &&source, ControlStyle style = {}, SizePolicy policy = {}) {
  return detail::liveCaption<Label>(detail::captionSource(std::forward<Source>(source)), style, policy);
}

/** @tparam Source Exact TextSource or TextProperty, including an unpublished sibling reference.
 * @param source Visible caption/draft or accepted model identity.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Live one-way button caption; action attachment remains independent.
 */
template <detail::CaptionSource Source>
Description<Button> button(Source &&source, ControlStyle style = {}, SizePolicy policy = {}) {
  return detail::liveCaption<Button>(detail::captionSource(std::forward<Source>(source)), style, policy);
}

/** @param text Owning literal, format result or localization key, resolved once at mount.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @param resolver Explicit owning localization adapter; borrowed services must outlive mount.
 * @return Independent label recipe; expression/resource failures retain their typed payload.
 */
inline Description<Label> label(TextValue text, ControlStyle style = {}, SizePolicy policy = {},
                                TextResolver resolver = {}) {
  return detail::textControl<Label>(std::move(text), style, policy, std::move(resolver));
}

/** @tparam State Exact ManagedState<string> or read-only ManagedSource<string>; bare braces retain literal overload.
 * @param model Scene-owned accepted text state; must belong to the receiving scene.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return One-way caption recipe; changed revisions synchronize before dirty frames without caller tokens.
 */
template <class State>
  requires(std::same_as<std::remove_cvref_t<State>, ManagedState<std::string>> ||
           std::same_as<std::remove_cvref_t<State>, ManagedSource<std::string>>)
inline Description<Label> label(State &&model, ControlStyle style = {}, SizePolicy policy = {}) {
  return control<Label>("", style, policy).setup([model](ManagedScene &scene, Node<Label> node) {
    return scene.bindText(node, model);
  });
}

/** @param text Owning literal, format result or localization key, resolved once at mount.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @param resolver Explicit owning localization adapter; borrowed services must outlive mount.
 * @return Unbound button recipe with an independent caption; onClick can be composed separately.
 */
inline Description<Button> button(TextValue text, ControlStyle style = {}, SizePolicy policy = {},
                                  TextResolver resolver = {}) {
  return detail::textControl<Button>(std::move(text), style, policy, std::move(resolver));
}

/** @tparam State Exact ManagedState<string> or read-only ManagedSource<string>; bare braces retain literal overload.
 * @param model Scene-owned accepted text state; must belong to the receiving scene.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return One-way caption recipe with independent action attachment and existing dirty-frame recovery.
 */
template <class State>
  requires(std::same_as<std::remove_cvref_t<State>, ManagedState<std::string>> ||
           std::same_as<std::remove_cvref_t<State>, ManagedSource<std::string>>)
inline Description<Button> button(State &&model, ControlStyle style = {}, SizePolicy policy = {}) {
  return control<Button>("", style, policy).setup([model](ManagedScene &scene, Node<Button> node) {
    return scene.bindText(node, model);
  });
}

/** @tparam Caption Owning TextValue, ManagedState<string> or read-only ManagedSource<string> observation.
 * @tparam Callback Owning compact managed action, optionally borrowing ClickEvent.
 * @param text Caption expression or explicit accepted text state.
 * @param action Independent activation callback with existing checked result handling.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Button recipe with scene-owned caption/action lifetimes and transactional mount cleanup.
 */
template <class Caption, class Callback>
  requires((std::same_as<std::remove_cvref_t<Caption>, TextValue> ||
            std::same_as<std::remove_cvref_t<Caption>, ManagedState<std::string>> ||
            std::same_as<std::remove_cvref_t<Caption>, ManagedSource<std::string>> || detail::CaptionSource<Caption>) &&
           portals::detail::ManagedAction<Callback, ClickEvent>)
inline Description<Button> button(Caption &&text, Callback action, ControlStyle style = {}, SizePolicy policy = {}) {
  return button(std::forward<Caption>(text), style, policy).onClick(std::move(action));
}

/** @param text UTF-8 caption copied immediately.
 * @param action Standard managed activation callback; must be nonempty.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return Single-use button recipe retaining its callback until mount.
 */
inline Description<Button> button(std::string_view text, std::function<std::expected<void, TreeError>()> action,
                                  ControlStyle style = {}, SizePolicy policy = {}) {
  return control<Button>(text, style, policy)
    .setup([action = std::move(action)](ManagedScene &scene, Node<Button> node) mutable {
      return scene.onClick(node, std::move(action));
    });
}

/** @tparam Callback Owning compact action returning void, expected<void, TreeError> or EditorCommandResult.
 * @param text UTF-8 caption copied immediately.
 * @param action Action optionally borrowing ClickEvent; return field.undo/redo directly to preserve its receipt.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return Single-use button recipe with scene-owned callback and exact command provenance reporting.
 */
template <class Callback>
  requires portals::detail::ManagedAction<Callback, ClickEvent>
inline Description<Button> button(std::string_view text, Callback action, ControlStyle style = {},
                                  SizePolicy policy = {}) {
  return control<Button>(text, style, policy)
    .setup([action = std::move(action)](ManagedScene &scene, Node<Button> node) mutable {
      return scene.onClick(node, std::move(action));
    });
}

/** @tparam Action Deduced shared command reference; braced captions use the text overloads.
 * @param action Weak shared command; its owner is attached automatically during checked mount.
 * @param style Manual appearance. @param policy Layout constraints.
 * @return Ordinary button whose caption/state and callback share the action identity.
 */
template <class Action>
  requires std::same_as<Action, ActionRef>
inline Description<Button> button(Action action, ControlStyle style = {}, SizePolicy policy = {}) {
  return button(TextValue(""), style, policy).onClick(std::move(action));
}

/** @tparam Action Deduced shared command reference; braced captions use the text overloads.
 * @param reference Empty observing slot published after the whole mount succeeds.
 * @param action Shared command. @param style Manual appearance. @param policy Layout constraints.
 * @return Action button with the existing transactional reference publication.
 */
template <class Action>
  requires std::same_as<Action, ActionRef>
inline Description<Button> button(Ref<Button> reference, Action action, ControlStyle style = {},
                                  SizePolicy policy = {}) {
  return button(std::move(action), style, policy).reference(std::move(reference));
}

/** @param text UTF-8 caption copied immediately.
 * @param model Observing managed state from the scene receiving this recipe.
 * @param changed Optional callback after the standard commit; cannot undo the commit.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return Single-use bound checkbox recipe.
 */
inline Description<CheckBox> checkbox(std::string_view text, ManagedState<bool> model,
                                      std::function<std::expected<void, TreeError>(CheckBoxResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}) {
  return detail::Access::controlSetup(
    detail::Access::controlArguments(control<CheckBox>(text, style, policy),
                                     {.text = TextValue(text), .cycle = CheckCycle::TWO_STATE}),
    [model,
     changed = std::move(changed)](ManagedScene &scene, Node<CheckBox> node,
                                   const ControlSettings &settings) mutable -> std::expected<void, ManagedError> {
      if (settings.state == CheckState::MIXED || settings.cycle == CheckCycle::THREE_STATE) {
        return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), TreeError::INVALID_ARGUMENT, {}, {}});
      }

      return scene.bind(node, model, std::move(changed));
    });
}

/** @param text UTF-8 caption copied immediately.
 * @param initial Initial checked value, owned by the receiving scene after mount.
 * @param changed Optional callback after the standard commit.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return Checkbox recipe with internal managed state; no binding token escapes.
 */
inline Description<CheckBox> checkbox(std::string_view text, bool initial,
                                      std::function<std::expected<void, TreeError>(CheckBoxResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}) {
  return detail::Access::controlSetup(
    detail::Access::controlArguments(control<CheckBox>(text, style, policy),
                                     {.text = TextValue(text),
                                      .state = initial ? CheckState::CHECKED : CheckState::UNCHECKED,
                                      .cycle = CheckCycle::TWO_STATE}),
    [initial, changed = std::move(changed)](ManagedScene &scene, Node<CheckBox> node,
                                            const ControlSettings &settings) mutable {
      if (settings.state == CheckState::MIXED || settings.cycle == CheckCycle::THREE_STATE) {
        return std::expected<void, ManagedError>(std::unexpected(detail::error(node, TreeError::INVALID_ARGUMENT)));
      }
      return scene
        .state(settings.state.value_or(initial ? CheckState::CHECKED : CheckState::UNCHECKED) == CheckState::CHECKED)
        .and_then([&scene, node, &changed](ManagedState<bool> model) {
          return scene.bind(node, model, std::move(changed));
        });
    });
}

/** @param text UTF-8 caption copied immediately.
 * @param model Observing managed state from the scene receiving this recipe.
 * @param changed Optional callback after the standard commit; cannot undo the commit.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @param options Typed user activation cycle.
 * @return Single-use bound checkbox recipe.
 */
inline Description<CheckBox> checkbox(std::string_view text, ManagedState<CheckState> model,
                                      std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}, CheckBoxOptions options = {}) {
  return detail::Access::controlSetup(
    control<CheckBox>(text, style, policy),
    [model, changed = std::move(changed), options](ManagedScene &scene, Node<CheckBox> node,
                                                   const ControlSettings &settings) mutable {
      options.cycle = settings.cycle.value_or(options.cycle);
      return scene.bind(node, model, std::move(changed), options);
    });
}

/** @param text UTF-8 caption copied immediately.
 * @param initial Initial checked value, owned by the receiving scene after mount.
 * @param changed Optional callback after the standard commit.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @param options Typed user activation cycle.
 * @return Checkbox recipe with internal managed state; no binding token escapes.
 */
inline Description<CheckBox> checkbox(std::string_view text, CheckState initial,
                                      std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}, CheckBoxOptions options = {}) {
  return detail::Access::controlSetup(
    detail::Access::controlArguments(control<CheckBox>(text, style, policy),
                                     {.text = TextValue(text), .state = initial}),
    [initial, changed = std::move(changed), options](ManagedScene &scene, Node<CheckBox> node,
                                                     const ControlSettings &settings) mutable {
      options.cycle = settings.cycle.value_or(options.cycle);
      return scene.state(settings.state.value_or(initial))
        .and_then([&scene, node, &changed, options](ManagedState<CheckState> model) {
          return scene.bind(node, model, std::move(changed), options);
        });
    });
}

/** @brief Construct a named checkbox with the fixed three-state activation cycle.
 * @param text UTF-8 caption copied now and validated under the receiving scene's width policy.
 * @param model Observing explicit state owned by the receiving scene.
 * @param changed Optional owning callback after the standard commit; cannot undo that commit.
 * @param style Owning manual appearances below selector declarations.
 * @param policy Owning layout constraints.
 * @return Single-use ThreeState recipe using the existing CheckBox binding and checked errors.
 */
inline Description<ThreeState> threeState(std::string_view text, ManagedState<CheckState> model,
                                          std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                          ControlStyle style = {}, SizePolicy policy = {}) {
  return detail::Access::intrinsicStyle(control<ThreeState>(text, style, policy),
                                        StyleTarget{.type = "ThreeState", .control = true, .baseTypes = {"CheckBox"}})
    .setup([model, changed = std::move(changed)](ManagedScene &scene, Node<ThreeState> node) mutable {
      return scene.bind(Node<CheckBox>{node}, model, std::move(changed),
                        CheckBoxOptions{.cycle = CheckCycle::THREE_STATE});
    });
}

/** @brief Construct a ThreeState with scene-owned state and inherited checkbox presentation.
 * @param text UTF-8 caption copied now and validated at mount.
 * @param initial Initial explicit state, UNCHECKED by default; invalid values fail at binding.
 * @param changed Optional owning callback after each standard commit.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return Single-use recipe with natural ThreeState/CheckBox style names and the fixed three-state cycle.
 */
inline Description<ThreeState> threeState(std::string_view text, CheckState initial,
                                          std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                          ControlStyle style = {}, SizePolicy policy = {}) {
  return detail::Access::controlSetup(
    detail::Access::controlArguments(
      detail::Access::intrinsicStyle(control<ThreeState>(text, style, policy),
                                     StyleTarget{.type = "ThreeState", .control = true, .baseTypes = {"CheckBox"}}),
      {.text = TextValue(text), .state = initial}),
    [initial, changed = std::move(changed)](ManagedScene &scene, Node<ThreeState> node,
                                            const ControlSettings &settings) mutable {
      return scene.state(settings.state.value_or(initial))
        .and_then([&scene, node, &changed](ManagedState<CheckState> model) {
          return scene.bind(Node<CheckBox>{node}, model, std::move(changed),
                            CheckBoxOptions{.cycle = CheckCycle::THREE_STATE});
        });
    });
}

/** @param text Explicit caption. @return Checkbox with context-provided initial state and cycle. */
inline Description<CheckBox> checkbox(std::string_view text) {
  return detail::Access::controlArguments(checkbox(text, CheckState::UNCHECKED), {.text = TextValue(text)});
}

/** @param text Explicit caption. @return ThreeState with context-provided initial state and fixed cycle. */
inline Description<ThreeState> threeState(std::string_view text) {
  return detail::Access::controlArguments(threeState(text, CheckState::UNCHECKED), {.text = TextValue(text)});
}

/** @brief Configure an explicit checkbox value without positional callback/style placeholders.
 * @tparam Options Exactly CheckBoxOptions after cv/ref removal; bare braces do not deduce this overload.
 * @param text UTF-8 caption copied immediately.
 * @param initial Initial state owned by the receiving scene after mount.
 * @param options Owning activation policy copied now; invalid values fail through the existing bind boundary.
 * @param changed Optional exact commit receipt callback; cannot undo a committed value.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return The existing checkbox recipe, preserving its ownership, errors and synchronization behavior.
 */
template <class Options>
  requires std::same_as<std::remove_cvref_t<Options>, CheckBoxOptions>
inline Description<CheckBox> checkbox(std::string_view text, CheckState initial, Options &&options,
                                      std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}) {
  return detail::Access::controlArguments(
    checkbox(text, initial, std::move(changed), style, policy, std::forward<Options>(options)),
    {.text = TextValue(text), .state = initial, .cycle = options.cycle});
}

/** @brief Configure shared checkbox state without positional callback/style placeholders.
 * @tparam Options Exactly CheckBoxOptions after cv/ref removal; implicit conversions are not admitted.
 * @param text UTF-8 caption copied immediately.
 * @param model Observing managed explicit state from the receiving scene.
 * @param options Owning activation policy copied now, including from a temporary or const lvalue.
 * @param changed Optional exact commit receipt callback.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return The existing shared-state checkbox recipe; captures and cleanup follow the original factory.
 */
template <class Options>
  requires std::same_as<std::remove_cvref_t<Options>, CheckBoxOptions>
inline Description<CheckBox> checkbox(std::string_view text, ManagedState<CheckState> model, Options &&options,
                                      std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}) {
  return detail::Access::controlArguments(
    checkbox(text, model, std::move(changed), style, policy, std::forward<Options>(options)),
    {.text = TextValue(text), .cycle = options.cycle});
}

namespace detail {
/** @brief One editor constructor path; context settings never replace an explicit shared model.
 * @param external Explicit observing model, or absent for independent internal state.
 * @param initial Owned fallback text; an explicit positional literal is also an instance override.
 * @param config Legacy full options/services, retained by value until mount.
 * @param inherit Whether omitted options inherit the immutable context.
 * @param resolver Owning synchronous text adapter; no callback survives its normal binding lifetime.
 * @return Existing editor/binding recipe with constructor-only settings access.
 */
inline Description<LineEdit> lineEditRecipe(std::optional<ManagedState<std::string>> external, TextValue initial,
                                            LineEditConfig config, bool inherit, TextResolver resolver = {}) {
  auto model = std::make_shared<ManagedState<std::string>>(external.value_or(ManagedState<std::string>{}));
  auto description =
    Access::cold<LineEdit>([initial = std::move(initial), options = config.options, policy = config.policy,
                            resolver = std::move(resolver), model, external = external.has_value()](
                             ManagedScene &scene, Node<Widget> parent,
                             MountContext &context) mutable -> std::expected<Node<LineEdit>, ManagedError> {
      const ControlSettings empty;
      const auto &settings = context.controlSettings ? *context.controlSettings : empty;
      auto effective = controlLineEditOptions(settings, options, scene.widthPolicy(), resolver, parent);
      if (!effective) {
        return std::unexpected(effective.error());
      }
      auto placeholder = lineEditPlaceholder(*effective, parent);
      if (!placeholder) {
        return std::unexpected(placeholder.error());
      }
      if (!external) {
        const auto &text = settings.text ? *settings.text : initial;
        auto resolved = resolver ? text.resolve(scene.widthPolicy(), resolver, effective->limits)
                                 : text.resolve(scene.widthPolicy(), effective->limits);
        if (!resolved) {
          auto failure = error(parent, TreeError::INVALID_ARGUMENT);
          if (const auto *textError = std::get_if<TextError>(&resolved.error())) {
            failure.text = *textError;
            // Initial draft admission remains a binding failure when effective
            // limits reject it before an editor or model has been allocated.
            switch (textError->kind) {
            case TextErrorKind::INPUT_LIMIT:
            case TextErrorKind::CLUSTER_LIMIT:
            case TextErrorKind::GRAPHEME_LIMIT:
            case TextErrorKind::CELL_LIMIT:
              failure.phase = ManagedPhase::BIND;
              break;
            default:
              break;
            }
          } else {
            failure.resource = std::get<TextResourceError>(resolved.error());
          }
          return std::unexpected(failure);
        }
        auto state = scene.state(std::string(resolved->bytes()));
        if (!state) {
          return std::unexpected(state.error());
        }
        *model = *state;
      }
      return scene.emplace<LineEdit>(parent, *effective, policy, std::move(*placeholder));
    });
  return Access::controlSetup(
    Access::configuredControl(std::move(description), "LineEdit", inherit),
    [model, result = std::move(config.result), clipboard = std::move(config.clipboard),
     validate = std::move(config.validate)](ManagedScene &scene, Node<LineEdit> node, const ControlSettings &) mutable {
      return scene.bind(node, *model, std::move(result), std::move(clipboard), std::move(validate));
    });
}
} // namespace detail

/** @param model Explicit scene-owned model. @param config Full legacy options, bypassing context defaults.
 * @return Bound editor; an additional settings modifier explicitly overrides those options.
 */
inline Description<LineEdit> lineEdit(ManagedState<std::string> model, LineEditConfig config) {
  return detail::lineEditRecipe(model, TextValue(""), std::move(config), false);
}

/** @param model Explicit scene-owned model. @return Editor inheriting sparse context options. */
inline Description<LineEdit> lineEdit(ManagedState<std::string> model) {
  return detail::lineEditRecipe(model, TextValue(""), {}, true);
}

/** @param initial Owned explicit initial text. @param config Full legacy editor options.
 * @param resolver Optional synchronous text adapter. @return Independent editor preserving full-config semantics.
 */
inline Description<LineEdit> lineEdit(TextValue initial, LineEditConfig config, TextResolver resolver = {}) {
  return detail::lineEditRecipe(std::nullopt, std::move(initial), std::move(config), false, std::move(resolver));
}

/** @param initial Explicit initial expression. @return Independent editor inheriting context options. */
inline Description<LineEdit> lineEdit(TextValue initial) {
  auto arguments = ControlSettings{.text = initial};
  return detail::Access::controlArguments(detail::lineEditRecipe(std::nullopt, std::move(initial), {}, true),
                                          std::move(arguments));
}

/** @param initial UTF-8 copied immediately. @param config Full legacy editor options.
 * @return Independent editor preserving the explicit full configuration.
 */
inline Description<LineEdit> lineEdit(std::string_view initial, LineEditConfig config) {
  return lineEdit(TextValue(initial), std::move(config));
}

/** @param initial UTF-8 copied immediately. @return Independent editor inheriting context options. */
inline Description<LineEdit> lineEdit(std::string_view initial) {
  return lineEdit(TextValue(initial));
}

/** @return Independent editor whose initial text and options may come from context defaults. */
inline Description<LineEdit> lineEdit() {
  return detail::lineEditRecipe(std::nullopt, TextValue(""), {}, true);
}

/** @tparam Settings Exactly ControlSettings; bare braces keep the legacy full-config overload.
 * @tparam Initial Explicit text expression, string or scene-owned model accepted by lineEdit.
 * @param initial Explicit initial value/model. @param patch Owning sparse final overrides.
 * @return Context-aware editor with only present fields overridden.
 */
template <class Initial, class Settings>
  requires std::same_as<std::remove_cvref_t<Settings>, ControlSettings> &&
           requires(Initial &&initial) { lineEdit(std::forward<Initial>(initial)); }
inline Description<LineEdit> lineEdit(Initial &&initial, Settings &&patch) {
  return lineEdit(std::forward<Initial>(initial)).settings(std::forward<Settings>(patch));
}

/** @tparam Settings Exactly ControlSettings. @param initial Owning explicit expression.
 * @param patch Sparse final overrides. @param resolver Synchronous text adapter.
 * @return Context-aware editor supporting localized text and placeholders.
 */
template <class Settings>
  requires std::same_as<std::remove_cvref_t<Settings>, ControlSettings>
inline Description<LineEdit> lineEdit(TextValue initial, Settings &&patch, TextResolver resolver) {
  auto arguments = ControlSettings{.text = initial};
  return detail::Access::controlArguments(
           detail::lineEditRecipe(std::nullopt, std::move(initial), {}, true, std::move(resolver)),
           std::move(arguments))
    .settings(std::forward<Settings>(patch));
}

/** @param reference Empty observing slot. @param initial Explicit text expression.
 * @param config Full legacy options. @param resolver Optional synchronous text adapter.
 * @return Independent editor with deferred Ref publication.
 */
inline Description<LineEdit> lineEdit(Ref<LineEdit> reference, TextValue initial, LineEditConfig config,
                                      TextResolver resolver = {}) {
  return lineEdit(std::move(initial), std::move(config), std::move(resolver)).reference(std::move(reference));
}

/** @param reference Empty observing slot. @param initial Explicit text expression.
 * @return Context-aware editor with deferred Ref publication.
 */
inline Description<LineEdit> lineEdit(Ref<LineEdit> reference, TextValue initial) {
  return lineEdit(std::move(initial)).reference(std::move(reference));
}

/** @param reference Empty observing slot. @param initial UTF-8 copied now. @param config Full legacy options.
 * @return Independent editor preserving the complete explicit configuration.
 */
inline Description<LineEdit> lineEdit(Ref<LineEdit> reference, std::string_view initial, LineEditConfig config) {
  return lineEdit(initial, std::move(config)).reference(std::move(reference));
}

/** @param reference Empty observing slot. @param initial UTF-8 copied now.
 * @return Context-aware editor with deferred Ref publication.
 */
inline Description<LineEdit> lineEdit(Ref<LineEdit> reference, std::string_view initial) {
  return lineEdit(initial).reference(std::move(reference));
}

/** @param reference Empty observing slot. @return Editor inheriting initial text and options from context. */
inline Description<LineEdit> lineEdit(Ref<LineEdit> reference) {
  return lineEdit().reference(std::move(reference));
}

/** @tparam Settings Exactly ControlSettings. @tparam Initial Existing text/model initial input.
 * @param reference Empty observing slot. @param initial Explicit value/model. @param patch Sparse overrides.
 * @return Context-aware typed editor with deferred publication.
 */
template <class Initial, class Settings>
  requires std::same_as<std::remove_cvref_t<Settings>, ControlSettings> &&
           requires(Initial &&initial) { lineEdit(std::forward<Initial>(initial)); }
inline Description<LineEdit> lineEdit(Ref<LineEdit> reference, Initial &&initial, Settings &&patch) {
  return lineEdit(std::forward<Initial>(initial), std::forward<Settings>(patch)).reference(std::move(reference));
}

/** @param text Caption copied into an unbound button recipe.
 * @return Consuming recipe; add onClick before mount or Ref.onClick after successful mount.
 */
inline Description<Button> button(std::string_view text) {
  return control<Button>(text, {}, {});
}

/** @param reference Shared empty slot, published after the whole mount succeeds.
 * @param text Caption copied now.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Label recipe sharing the existing reference transaction.
 */
inline Description<Label> label(Ref<Label> reference, std::string_view text, ControlStyle style = {},
                                SizePolicy policy = {}) {
  return label(text, style, policy).reference(std::move(reference));
}

/** @param reference Empty observing slot published only after complete mount.
 * @param text Caption copied now.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Unbound button recipe for late Ref.onClick registration before run.
 */
inline Description<Button> button(Ref<Button> reference, std::string_view text, ControlStyle style = {},
                                  SizePolicy policy = {}) {
  return control<Button>(text, style, policy).reference(std::move(reference));
}

/** @tparam Caption Owning TextValue or scene-owned ManagedState<string>.
 * @param reference Empty observing slot published only after the complete mount succeeds.
 * @param text Caption expression or explicit accepted text state.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Label recipe retaining the original transactional reference publication.
 */
template <class Caption>
  requires(std::same_as<std::remove_cvref_t<Caption>, TextValue> ||
           std::same_as<std::remove_cvref_t<Caption>, ManagedState<std::string>> ||
           std::same_as<std::remove_cvref_t<Caption>, ManagedSource<std::string>> || detail::CaptionSource<Caption>)
inline Description<Label> label(Ref<Label> reference, Caption &&text, ControlStyle style = {}, SizePolicy policy = {}) {
  return label(std::forward<Caption>(text), style, policy).reference(std::move(reference));
}

/** @tparam Caption Owning TextValue or scene-owned ManagedState<string>.
 * @param reference Empty observing slot published only after the complete mount succeeds.
 * @param text Caption expression or explicit accepted text state.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Button caption recipe; action registration remains independent.
 */
template <class Caption>
  requires(std::same_as<std::remove_cvref_t<Caption>, TextValue> ||
           std::same_as<std::remove_cvref_t<Caption>, ManagedState<std::string>> ||
           std::same_as<std::remove_cvref_t<Caption>, ManagedSource<std::string>> || detail::CaptionSource<Caption>)
inline Description<Button> button(Ref<Button> reference, Caption &&text, ControlStyle style = {},
                                  SizePolicy policy = {}) {
  return button(std::forward<Caption>(text), style, policy).reference(std::move(reference));
}

/** @tparam Caption Owning TextValue or scene-owned ManagedState<string>.
 * @tparam Callback Owning compact managed action, optionally borrowing ClickEvent.
 * @param reference Empty observing slot published after the whole mount succeeds.
 * @param text Caption expression or explicit accepted text state.
 * @param action Independent checked activation callback.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Bound button recipe preserving existing callback and rollback contracts.
 */
template <class Caption, class Callback>
  requires((std::same_as<std::remove_cvref_t<Caption>, TextValue> ||
            std::same_as<std::remove_cvref_t<Caption>, ManagedState<std::string>> ||
            std::same_as<std::remove_cvref_t<Caption>, ManagedSource<std::string>> || detail::CaptionSource<Caption>) &&
           portals::detail::ManagedAction<Callback, ClickEvent>)
inline Description<Button> button(Ref<Button> reference, Caption &&text, Callback action, ControlStyle style = {},
                                  SizePolicy policy = {}) {
  return button(std::forward<Caption>(text), std::move(action), style, policy).reference(std::move(reference));
}

/** @tparam Callback Owning compact action, optionally taking ClickEvent; void/expected/command result.
 * @param reference Empty observing slot published after complete mount.
 * @param text Caption copied now.
 * @param action Owning callback; captures transfer to the scene at mount.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Bound button recipe; registration errors participate in subtree rollback.
 */
template <class Callback>
  requires portals::detail::ManagedAction<Callback, ClickEvent>
Description<Button> button(Ref<Button> reference, std::string_view text, Callback action, ControlStyle style = {},
                           SizePolicy policy = {}) {
  return button(text, std::move(action), style, policy).reference(std::move(reference));
}

/** @param reference Empty observing slot published after complete mount.
 * @param text Caption copied now.
 * @param model Existing managed bool state.
 * @param changed Optional typed receipt callback.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @return Checkbox recipe retaining its original commit semantics.
 */
inline Description<CheckBox> checkbox(Ref<CheckBox> reference, std::string_view text, ManagedState<bool> model,
                                      std::function<std::expected<void, TreeError>(CheckBoxResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}) {
  return checkbox(text, model, std::move(changed), style, policy).reference(std::move(reference));
}

/** @param reference Empty observing slot published after complete mount.
 * @param text Caption copied now.
 * @param model Existing managed explicit checkbox state.
 * @param changed Optional typed receipt callback.
 * @param style Manual appearances.
 * @param policy Layout constraints.
 * @param options Typed user activation cycle.
 * @return Checkbox recipe retaining its original commit semantics.
 */
inline Description<CheckBox> checkbox(Ref<CheckBox> reference, std::string_view text, ManagedState<CheckState> model,
                                      std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                      ControlStyle style = {}, SizePolicy policy = {}, CheckBoxOptions options = {}) {
  return checkbox(text, model, std::move(changed), style, policy, options).reference(std::move(reference));
}

/** @param reference Empty observing slot published only after complete mount.
 * @param text UTF-8 caption copied now.
 * @param initial Initial scene-owned state, UNCHECKED by default.
 * @param changed Optional owning receipt callback after standard commit.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return Named three-state recipe with checked reference publication and fixed activation cycle.
 */
inline Description<ThreeState> threeState(Ref<ThreeState> reference, std::string_view text, CheckState initial,
                                          std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                          ControlStyle style = {}, SizePolicy policy = {}) {
  return threeState(text, initial, std::move(changed), style, policy).reference(std::move(reference));
}

/** @param reference Empty observing slot. @param text Explicit caption.
 * @return ThreeState inheriting its initial state from the immutable context.
 */
inline Description<ThreeState> threeState(Ref<ThreeState> reference, std::string_view text) {
  return threeState(text).reference(std::move(reference));
}

/** @param reference Empty observing slot. @param text Explicit caption.
 * @return Checkbox inheriting its initial state and activation cycle from context.
 */
inline Description<CheckBox> checkbox(Ref<CheckBox> reference, std::string_view text) {
  return checkbox(text).reference(std::move(reference));
}

/** @param reference Empty observing slot published only after complete mount.
 * @param text UTF-8 caption copied now.
 * @param model Explicit state owned by the receiving scene.
 * @param changed Optional owning receipt callback after standard commit.
 * @param style Owning manual appearances.
 * @param policy Owning layout constraints.
 * @return Named recipe reusing the original checkbox binding and typed failures.
 */
inline Description<ThreeState> threeState(Ref<ThreeState> reference, std::string_view text,
                                          ManagedState<CheckState> model,
                                          std::function<std::expected<void, TreeError>(CheckStateResult)> changed = {},
                                          ControlStyle style = {}, SizePolicy policy = {}) {
  return threeState(text, model, std::move(changed), style, policy).reference(std::move(reference));
}

/** @param reference Empty observing editor slot published after complete mount.
 * @param model Existing managed text state.
 * @param config Owning editor callbacks/options; existing EditMode/history behavior is retained.
 * @return Bound editor recipe using the original factory and reference transaction.
 */
inline Description<LineEdit> lineEdit(Ref<LineEdit> reference, ManagedState<std::string> model, LineEditConfig config) {
  return lineEdit(model, std::move(config)).reference(std::move(reference));
}

/** @param reference Empty observing slot. @param model Explicit scene-owned model.
 * @return Editor inheriting context options while preserving the explicit shared model.
 */
inline Description<LineEdit> lineEdit(Ref<LineEdit> reference, ManagedState<std::string> model) {
  return lineEdit(model).reference(std::move(reference));
}

/** @tparam T Control subtype compatible with the existing caption constructor.
 * @param reference Empty control observation published after complete mount.
 * @param text UTF-8 caption copied into owning storage.
 * @param style Existing manual appearances.
 * @param policy Existing layout constraints.
 * @return Existing control recipe with additive reference publication.
 */
template <class T>
Description<T> control(Ref<T> reference, std::string_view text, ControlStyle style = {}, SizePolicy policy = {}) {
  return control<T>(text, style, policy).reference(std::move(reference));
}

/** @brief Owning existing LineEdit/scroll read-only policy stage. */
struct ReadOnlyModifier {
  bool enabled; ///< User-mutation policy; programmatic commands retain their existing behavior.

  /** @tparam T Exactly LineEdit, ScrollArea or ScrollBar.
   * @param description Consumed receiving recipe.
   * @param modifier Owning local policy.
   * @return Recipe staging its existing readOnly property.
   */
  template <class T>
    requires(std::same_as<T, LineEdit> || std::same_as<T, Password> || std::same_as<T, ScrollArea> ||
             std::same_as<T, ScrollBar>)
  friend Description<T> operator|(Description<T> &&description, ReadOnlyModifier modifier) {
    return std::move(description).readOnly(modifier.enabled);
  }
};

/** @param enabled Block user mutation while preserving existing programmatic commands.
 * @return Owning LineEdit/native scroll policy stage.
 */
inline ReadOnlyModifier readOnly(bool enabled = true) {
  return {enabled};
}

/** @param text UTF-8 caption copied now and checked at mount under the receiving WidthPolicy.
 * @param style Owning manual normal/focused/disabled appearances beneath themes.
 * @param policy Owning layout constraints, checked by the existing solver.
 * @return Existing Button recipe without brackets; activation still uses existing bindings/router.
 */
inline Description<Button> plainButton(std::string_view text, ControlStyle style = {}, SizePolicy policy = {}) {
  return detail::textControl<Button>(TextValue(text), style, policy, {}, ButtonChrome::NONE);
}

/** @tparam Callback Existing compact action accepting optionally const ClickEvent&.
 * @param text UTF-8 caption copied immediately.
 * @param action Owning action transferred to the existing managed click binding at mount.
 * @param style Owning manual appearances beneath themes.
 * @param policy Owning layout constraints.
 * @return Existing unbracketed Button recipe; action errors preserve their checked boundary/provenance.
 */
template <class Callback>
  requires portals::detail::ManagedAction<Callback, ClickEvent>
Description<Button> plainButton(std::string_view text, Callback action, ControlStyle style = {},
                                SizePolicy policy = {}) {
  return plainButton(text, style, policy).onClick(std::move(action));
}

/** @param reference Empty shared observing slot, published only after complete mount.
 * @param text UTF-8 caption copied immediately.
 * @param style Owning manual appearances beneath themes.
 * @param policy Owning layout constraints.
 * @return Existing unbracketed Button recipe sharing normal transactional reference publication.
 */
inline Description<Button> plainButton(Ref<Button> reference, std::string_view text, ControlStyle style = {},
                                       SizePolicy policy = {}) {
  return plainButton(text, style, policy).reference(std::move(reference));
}

/** @brief Sparse checkbox cycle stage preserving the factory's backing model. */
struct ThreeStateModifier {
  /** @param description Consumed checkbox recipe. @return Recipe with THREE_STATE selected. */
  friend Description<CheckBox> operator|(Description<CheckBox> &&description, ThreeStateModifier) {
    return std::move(description).threeState();
  }
};

/** @return Checkbox cycle stage equivalent to the consuming threeState() method. */
inline ThreeStateModifier threeState() {
  return {};
}

} // namespace portals::ui
