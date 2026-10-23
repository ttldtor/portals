// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_description.hpp>

namespace portals::ui {
/** @brief Ordinary observing Ref; copies never own the protected editor or its scene. */
using PasswordRef = Ref<Password>;

/** @brief Optional protected-editor customization; ordinary forms need only password(ref). */
struct PasswordConfig {
  PasswordOptions options{}; ///< Native editor policy; the scene supplies its Unicode width policy.
  SizePolicy policy{};       ///< Owning external layout constraints.
  std::function<std::expected<PasswordValue, PasswordFailure>()>
    initial{}; ///< Checked owning provider, called once at mount.
  std::function<std::expected<void, TreeError>(PasswordResult)> result{}; ///< Optional secret-free result observer.
  std::shared_ptr<Clipboard> clipboard{};     ///< Service used only by explicit ENABLED paste policy.
  std::function<bool(SecretView)> validate{}; ///< Synchronous scoped publication predicate; false retains the draft.
};

/// @cond INTERNAL
namespace detail {
inline std::expected<PasswordOptions, ManagedError> passwordOptions(const ControlSettings &settings,
                                                                    PasswordOptions options, WidthPolicy policy,
                                                                    const TextResolver &resolver, Node<Widget> parent) {
  const auto apply = [](auto &target, const auto &value) {
    if (value) {
      target = *value;
    }
  };

  options.widthPolicy = policy;
  options.limits = controlTextLimits(settings, options.limits);
  apply(options.editMode, settings.editMode);
  apply(options.readOnly, settings.readOnly);
  apply(options.pastePolicy, settings.pastePolicy);
  apply(options.securePolicy.maxBytes, settings.securePolicy.maxBytes);
  apply(options.securePolicy.storage, settings.securePolicy.storage);
  apply(options.securePolicy.lockPages, settings.securePolicy.lockPages);
  apply(options.securePolicy.excludeFromDumps, settings.securePolicy.excludeFromDumps);
  apply(options.securePolicy.encryptWhileIdle, settings.securePolicy.encryptWhileIdle);

  if (settings.displayMode) {
    if (*settings.displayMode == LineEditDisplayMode::NORMAL) {
      return std::unexpected(error(parent, TreeError::INVALID_ARGUMENT));
    }

    options.displayMode = *settings.displayMode == LineEditDisplayMode::NO_ECHO ? PasswordDisplayMode::NO_ECHO
                                                                                : PasswordDisplayMode::PASSWORD;
  }
  if (settings.text && !settings.text->isEmptyLiteral()) {
    return std::unexpected(error(parent, TreeError::INVALID_ARGUMENT));
  }
  const bool validMode = options.editMode == EditMode::IMMEDIATE || options.editMode == EditMode::ON_COMPLETION ||
                         options.editMode == EditMode::EXPLICIT;
  const bool validDisplay =
    options.displayMode == PasswordDisplayMode::PASSWORD || options.displayMode == PasswordDisplayMode::NO_ECHO;
  const bool validPaste =
    options.pastePolicy == PasswordPastePolicy::DISABLED || options.pastePolicy == PasswordPastePolicy::ENABLED;
  if (!validMode || !validDisplay || !validPaste ||
      options.securePolicy.storage != SecureStringStorage::NATIVE_GUARDED || !options.securePolicy.encryptWhileIdle ||
      options.securePolicy.maxBytes > MAX_SECRET_BYTES) {
    return std::unexpected(error(parent, TreeError::INVALID_ARGUMENT));
  }

  if (settings.placeholder) {
    auto text = resolver ? settings.placeholder->resolve(policy, resolver, options.limits)
                         : settings.placeholder->resolve(policy, options.limits);

    if (!text) {
      auto failure = error(parent, TreeError::INVALID_ARGUMENT);

      if (const auto *cause = std::get_if<TextError>(&text.error())) {
        failure.text = *cause;
      } else {
        failure.resource = std::get<TextResourceError>(text.error());
      }

      return std::unexpected(std::move(failure));
    }

    options.placeholder.emplace(text->bytes());
  }

  return options;
}

inline Description<Password> passwordRecipe(std::optional<ManagedState<PasswordValue>> external, PasswordConfig config,
                                            bool inherit, TextResolver resolver = {}) {
  auto model = std::make_shared<ManagedState<PasswordValue>>(external.value_or(ManagedState<PasswordValue>{}));
  auto description =
    Access::cold<Password>([options = config.options, policy = config.policy, initial = std::move(config.initial),
                            resolver = std::move(resolver), model, external = external.has_value()](
                             ManagedScene &scene, Node<Widget> parent,
                             MountContext &context) mutable -> std::expected<Node<Password>, ManagedError> {
      const ControlSettings empty;
      const auto &settings = context.controlSettings ? *context.controlSettings : empty;
      auto effective = passwordOptions(settings, options, scene.widthPolicy(), resolver, parent);

      if (!effective) {
        return std::unexpected(effective.error());
      }
      if (external && initial) {
        return std::unexpected(error(parent, TreeError::INVALID_ARGUMENT));
      }
      if (!external) {
        std::expected<PasswordValue, PasswordFailure> value =
          initial ? initial()
                  : std::expected<PasswordValue, PasswordFailure>(PasswordValue::create({}, effective->securePolicy));

        if (!value) {
          return std::unexpected(
            portals::detail::passwordError(ManagedPhase::BIND, parent.id(), std::move(value.error())));
        }

        auto created = scene.state(std::move(*value));

        if (!created) {
          return std::unexpected(created.error());
        }

        *model = *created;
      }

      return scene.emplace<Password>(parent, *effective, policy);
    });

  return Access::controlSetup(
    Access::configuredControl(std::move(description), "Password", inherit),
    [model, result = std::move(config.result), clipboard = std::move(config.clipboard),
     validate = std::move(config.validate)](ManagedScene &scene, Node<Password> node, const ControlSettings &) mutable {
      return scene.bind(node, *model, std::move(result), std::move(clipboard), std::move(validate));
    });
}
} // namespace detail

/// @endcond

/** @return Empty protected editor with default native security and scene-owned model/binding. */
inline Description<Password> password() {
  return detail::passwordRecipe(std::nullopt, {}, true);
}

/** @param config Explicit native customization. @return Bound protected editor; settings can override config. */
inline Description<Password> password(PasswordConfig config) {
  return detail::passwordRecipe(std::nullopt, std::move(config), false);
}

/** @param model Protected state created by the receiving scene. @return Bound protected editor. */
inline Description<Password> password(ManagedState<PasswordValue> model) {
  return detail::passwordRecipe(model, {}, true);
}

/** @param model Protected scene state. @param config Explicit customization, without an initial provider.
 * @return Bound protected editor or a checked conflicting-provider error at mount.
 */
inline Description<Password> password(ManagedState<PasswordValue> model, PasswordConfig config) {
  return detail::passwordRecipe(model, std::move(config), false);
}

/** @param reference Slot published only after the complete mount. @return Empty protected editor. */
inline Description<Password> password(PasswordRef reference) {
  return password().reference(std::move(reference));
}

/** @param reference Observing slot. @param config Explicit customization. @return Bound protected editor. */
inline Description<Password> password(PasswordRef reference, PasswordConfig config) {
  return password(std::move(config)).reference(std::move(reference));
}

/** @param reference Observing slot. @param model Protected scene state. @return Bound protected editor. */
inline Description<Password> password(PasswordRef reference, ManagedState<PasswordValue> model) {
  return password(model).reference(std::move(reference));
}

/** @param reference Observing slot. @param model Protected scene state. @param config Explicit customization.
 * @return Bound protected editor, preserving the complete mount/ref transaction.
 */
inline Description<Password> password(PasswordRef reference, ManagedState<PasswordValue> model, PasswordConfig config) {
  return password(model, std::move(config)).reference(std::move(reference));
}

/** @brief Owning cold presentation modifier; ordinary LineEdit storage remains plaintext. */
struct NoEchoModifier {
  bool enabled{true}; ///< False restores each editor type's normal presentation.

  /** @tparam T LineEdit or Password. @param description Consumed recipe. @param modifier Owning presentation flag.
   * @return Recipe with the same native noEcho setting as the method form.
   */
  template <class T>
    requires(std::same_as<T, LineEdit> || std::same_as<T, Password>)
  friend Description<T> operator|(Description<T> &&description, NoEchoModifier modifier) {
    return std::move(description).noEcho(modifier.enabled);
  }
};

/** @param enabled Hide value glyphs and fix the caret origin. @return Owning cold presentation modifier. */
constexpr NoEchoModifier noEcho(bool enabled = true) noexcept {
  return {enabled};
}
} // namespace portals::ui
