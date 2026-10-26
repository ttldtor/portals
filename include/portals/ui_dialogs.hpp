// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_controls.hpp>
#include <portals/ui_layout.hpp>
#include <portals/ui_scene_recipes.hpp>

namespace portals::ui {

/** @brief Compose an application-owned nonmodal notification plane inside ordinary body content.
 * @tparam Body Consuming body description; only this body contributes intrinsic size.
 * @param center Copyable UI-affine observer published only after the complete mount succeeds.
 * @param body Ordinary retained content; dialogs belong outside this composition in overlayHost.
 * @param options Owning immutable host budgets and explicit text resolver.
 * @return Consuming overlay recipe with one private zero-intrinsic notification layer.
 * Ordinary Overlay layout modifiers apply locally; only scoped Notifications defaults apply to this wrapper.
 */
template <detail::DescriptionInput Body>
Description<Overlay> notifications(NotificationCenter center, Body &&body, NotificationOptions options = {}) {
  auto layout = detail::Access::captureLayouts(body);
  auto description = detail::Access::cold<Overlay>(
    [center = std::move(center), body = std::forward<Body>(body),
     options](ManagedScene &scene, Node<Widget> parent,
              detail::MountContext &context) mutable -> std::expected<Node<Overlay>, ManagedError> {
      auto effective = detail::controlOverlayOptions(*context.controlSettings, {});
      if (!effective) {
        return std::unexpected(detail::error(parent, effective.error()));
      }
      auto wrapper = scene.emplace<Overlay>(parent, *effective);
      if (!wrapper) {
        return std::unexpected(wrapper.error());
      }
      if (context.root == Node<Widget>{}) {
        context.root = *wrapper;
      }
      auto content = detail::Access::mount(std::move(body), scene, *wrapper, context);
      if (!content) {
        return std::unexpected(content.error());
      }
      auto effectiveOptions = detail::controlNotificationOptions(*context.controlSettings, options);
      auto bound = detail::Access::notifications(scene, *wrapper, *content, center, effectiveOptions, context);
      return bound ? wrapper : std::unexpected(bound.error());
    });

  // The semantic identity inherits only Notifications defaults, never unrelated ordinary Overlay declarations.
  return detail::Access::notificationControl(detail::Access::withLayout(std::move(description), std::move(layout)));
}

/** @brief Observe the real notification-plane Overlay while keeping its service handle separate.
 * @tparam Body Consuming ordinary-body description, following the existing notification composition contract.
 * @param reference Empty visual observing slot, published only after the complete mount succeeds.
 * @param center Existing weak notification service; post() still returns its exact ToastPostReceipt.
 * @param body Ordinary retained content contributing the host's intrinsic size.
 * @param options Existing owning notification budgets and localization settings.
 * @return The existing notification recipe with one visual reference; no new runtime or service is created.
 */
template <detail::DescriptionInput Body>
Description<Overlay> notifications(Ref<Overlay> reference, NotificationCenter center, Body &&body,
                                   NotificationOptions options = {}) {
  return notifications(std::move(center), std::forward<Body>(body), std::move(options)).reference(std::move(reference));
}

/** @brief Compose a retained host with one ordinary body and initially closed dialog layers.
 * @tparam Body Consuming ordinary-body description.
 * @tparam Layers Consuming dialog descriptions, registered in declaration order.
 * @param options Positive owning layer budget, including closed layers.
 * @param body Ordinary content retained across every open/dismiss cycle.
 * @param layers Dialog recipes mounted in the same complete transaction.
 * @return Single-use host recipe; any child/finalization failure retires the whole unpublished subtree.
 */
template <detail::DescriptionInput Body, detail::DescriptionInput... Layers>
  requires(std::same_as<std::remove_cvref_t<Layers>, Description<Dialog>> && ...)
Description<OverlayHost> overlayHost(OverlayHostOptions options, Body &&body, Layers &&...layers) {
  auto layout = detail::Access::captureLayouts(body, layers...);
  auto description = detail::Access::cold<OverlayHost>(
    [options, body = std::forward<Body>(body), layers = std::tuple(std::forward<Layers>(layers)...)](
      ManagedScene &scene, Node<Widget> parent,
      detail::MountContext &context) mutable -> std::expected<Node<OverlayHost>, ManagedError> {
      const auto effective = detail::controlOverlayHostOptions(*context.controlSettings, options);
      auto host = scene.emplace<OverlayHost>(parent, effective);
      if (!host) {
        return std::unexpected(host.error());
      }
      if (context.root == Node<Widget>{}) {
        context.root = *host;
      }
      auto content = detail::Access::mount(std::move(body), scene, *host, context);
      if (!content) {
        return std::unexpected(content.error());
      }
      const auto bodyRoot = context.mountedOuter != Node<Widget>{} ? context.mountedOuter : Node<Widget>(*content);
      auto bound = scene.overlayHost(*host, bodyRoot);
      if (!bound) {
        return std::unexpected(bound.error());
      }
      auto styled = scene.style(*host, StyleTarget{.type = "OverlayHost", .classes = {"overlay-host"}});
      if (!styled) {
        return std::unexpected(styled.error());
      }
      std::expected<void, ManagedError> result;
      std::apply(
        [&](auto &...layer) {
          [[maybe_unused]] const auto mount = [&](auto &recipe) {
            if (result) {
              auto child = detail::Access::mount(std::move(recipe), scene, *host, context);
              if (!child) {
                result = std::unexpected(child.error());
              }
            }
          };
          (mount(layer), ...);
        },
        layers);
      return result ? host : std::unexpected(result.error());
    });

  return detail::Access::configuredControl(
    detail::Access::choiceHost(detail::Access::withLayout(std::move(description), std::move(layout)), true),
    "OverlayHost");
}

/** @tparam Body Consuming ordinary-body recipe.
 * @tparam Layers Consuming dialog recipes.
 * @param body Retained ordinary content.
 * @param layers Initially closed layers.
 * @return Host with the default maximum of 32 registered layers.
 */
template <detail::DescriptionInput Body, detail::DescriptionInput... Layers>
  requires(std::same_as<std::remove_cvref_t<Layers>, Description<Dialog>> && ...)
Description<OverlayHost> overlayHost(Body &&body, Layers &&...layers) {
  return overlayHost(OverlayHostOptions{}, std::forward<Body>(body), std::forward<Layers>(layers)...);
}

/** @tparam Body Consuming ordinary-body recipe.
 * @tparam Layers Consuming dialog recipes.
 * @param reference Empty host observation published with the complete mount.
 * @param body Retained ordinary content.
 * @param layers Initially closed layers.
 * @return Default host recipe publishing the optional observation.
 */
template <detail::DescriptionInput Body, detail::DescriptionInput... Layers>
  requires(std::same_as<std::remove_cvref_t<Layers>, Description<Dialog>> && ...)
Description<OverlayHost> overlayHost(Ref<OverlayHost> reference, Body &&body, Layers &&...layers) {
  return overlayHost(std::forward<Body>(body), std::forward<Layers>(layers)...).reference(std::move(reference));
}

/** @brief Compose a reusable initially closed dialog with a standard title and opaque panel.
 * @tparam Body Consuming application content recipe.
 * @param title Owning text expression resolved once with the scene's complete width policy.
 * @param body Retained content; opening never remounts it.
 * @param options Immutable panel geometry/input settings.
 * @return Single-use layer recipe, requiring an enclosing declared overlayHost.
 */
template <detail::DescriptionInput Body>
Description<Dialog> dialog(TextValue title, Body &&body, DialogOptions options = {}) {
  auto heading = label(std::move(title)).style(StyleTarget{.type = "Label", .classes = {"dialog-title"}});
  auto content = column(std::forward<Body>(body)).style(StyleTarget{.type = "Column", .classes = {"dialog-body"}});
  auto layout = detail::Access::captureLayouts(heading, content);
  auto description =
    detail::Access::cold<Dialog>([options, heading = std::move(heading), content = std::move(content)](
                                   ManagedScene &scene, Node<Widget> parent,
                                   detail::MountContext &context) mutable -> std::expected<Node<Dialog>, ManagedError> {
      const auto effective = detail::controlDialogOptions(*context.controlSettings, options);

      if (context.controlSettings->title) {
        heading = std::move(heading).settings({.text = *context.controlSettings->title});

        if (context.controlPrepared) {
          detail::Access::preparedDialogHeading(heading, *context.controlSettings->title);
        }
      }

      auto valid = Dialog::validate(effective);
      if (!valid) {
        return std::unexpected(detail::error(parent, valid.error()));
      }
      auto root = scene.emplace<Dialog>(parent, effective);
      if (!root) {
        return std::unexpected(root.error());
      }
      if (context.root == Node<Widget>{}) {
        context.root = *root;
      }
      auto panel = detail::Access::dialogPanel(scene, *root, effective.frame);
      if (!panel) {
        return std::unexpected(panel.error());
      }
      context.nodes.push_back(*panel);
      auto column = scene.emplace<Column>(*panel);
      if (!column) {
        return std::unexpected(column.error());
      }
      context.nodes.push_back(*column);
      auto titleNode = detail::Access::mount(std::move(heading), scene, *column, context);
      if (!titleNode) {
        return std::unexpected(titleNode.error());
      }
      auto mounted = detail::Access::mount(std::move(content), scene, *column, context);
      if (!mounted) {
        return std::unexpected(mounted.error());
      }
      auto bound = scene.dialog(*root, *panel, *mounted);
      if (!bound) {
        return std::unexpected(bound.error());
      }
      auto styled = scene.style(
        *root, StyleTarget{.type = "Dialog",
                           .classes = {"dialog", effective.mode == DialogMode::MODAL ? "modal" : "nonmodal"}});
      return styled ? root : std::unexpected(styled.error());
    });

  return detail::Access::configuredControl(detail::Access::withLayout(std::move(description), std::move(layout)),
                                           "Dialog");
}

/** @tparam Body Consuming application content recipe.
 * @param reference Empty weak observation published after complete mount and anchor finalization.
 * @param title Owning title expression.
 * @param body Retained panel content.
 * @param options Immutable panel/input settings.
 * @return Standard reusable dialog recipe with its optional Ref.
 */
template <detail::DescriptionInput Body>
Description<Dialog> dialog(Ref<Dialog> reference, TextValue title, Body &&body, DialogOptions options = {}) {
  return dialog(std::move(title), std::forward<Body>(body), options).reference(std::move(reference));
}

/** @tparam Body Consuming application content recipe.
 * @param title Literal UTF-8 title copied into an owning expression immediately.
 * @param body Retained content inside the panel.
 * @param options Immutable geometry/input settings.
 * @return Same standard initially closed dialog recipe.
 */
template <detail::DescriptionInput Body>
Description<Dialog> dialog(std::string_view title, Body &&body, DialogOptions options = {}) {
  return dialog(TextValue(title), std::forward<Body>(body), options);
}

/** @tparam Body Consuming application content recipe.
 * @param reference Weak observation published only with a complete mount.
 * @param title Literal UTF-8 title copied immediately.
 * @param body Retained content.
 * @param options Immutable panel/input settings.
 * @return Same standard dialog recipe with its optional observation.
 */
template <detail::DescriptionInput Body>
Description<Dialog> dialog(Ref<Dialog> reference, std::string_view title, Body &&body, DialogOptions options = {}) {
  return dialog(std::move(reference), TextValue(title), std::forward<Body>(body), options);
}

/** @tparam Body Owning single-use content recipe; mounted exactly once.
 * @param reference Cold result facade published after complete mount.
 * @param title Owning caption. @param body Retained custom content.
 * @param buttons Standard action set. @param options Existing bounded modal settings.
 * @return Initially hidden resultful dialog; Escape and close chrome request CANCEL.
 */
template <detail::DescriptionInput Body>
Description<Dialog> dialog(Ref<MessageDialog> reference, TextValue title, Body &&body, DialogButtons buttons,
                           MessageOptions options = {}) {
  auto layout = detail::Access::captureLayouts(body);
  auto description =
    detail::MessageAccess::describe(std::move(reference), std::move(title), {},
                                    detail::Access::erase(std::forward<Body>(body)), std::move(options), buttons);

  return detail::Access::withLayout(std::move(description), std::move(layout));
}

/** @tparam Body Owning cold content. @param reference Result observation. @param title Copied caption.
 * @param body Retained content. @param buttons Standard action set. @param options Modal settings.
 * @return Initially hidden resultful dialog recipe.
 */
template <detail::DescriptionInput Body>
Description<Dialog> dialog(Ref<MessageDialog> reference, std::string_view title, Body &&body, DialogButtons buttons,
                           MessageOptions options = {}) {
  return dialog(std::move(reference), TextValue(title), std::forward<Body>(body), buttons, std::move(options));
}

/** @param reference Cold result observation. @param title Owning caption. @param body Owning wrapped paragraph.
 * @param buttons Standard action set. @param options Modal settings. @return Resultful paragraph recipe.
 */
inline Description<Dialog> dialog(Ref<MessageDialog> reference, TextValue title, TextValue body, DialogButtons buttons,
                                  MessageOptions options = {}) {
  return detail::MessageAccess::describe(std::move(reference), std::move(title), std::move(body), {},
                                         std::move(options), buttons);
}

/** @param reference Cold result observation. @param title Copied caption. @param body Copied paragraph.
 * @param buttons Standard action set. @param options Modal settings. @return Resultful paragraph recipe.
 */
inline Description<Dialog> dialog(Ref<MessageDialog> reference, std::string_view title, std::string_view body,
                                  DialogButtons buttons, MessageOptions options = {}) {
  return dialog(std::move(reference), TextValue(title), TextValue(body), buttons, std::move(options));
}

/// @cond INTERNAL
namespace detail {
Description<Dialog> defaultThemeSelector(ThemeSelectorRef reference, std::span<const ThemeChoice> choices,
                                         const ThemeSelectionOptions &selection,
                                         const std::optional<std::string> &initialId, ThemeSelectorOptions options);
} // namespace detail

template <std::size_t N>
  requires(N > 0)
Description<Dialog> themeSelector(ThemeSelectorRef reference, const ThemeChoice (&choices)[N],
                                  ThemeSelectorOptions options) {
  return detail::defaultThemeSelector(std::move(reference), choices, ThemeSelectionOptions{}, {}, std::move(options));
}

template <class Catalog>
  requires std::same_as<std::remove_cvref_t<Catalog>, ThemeSelectorCatalog>
Description<Dialog> themeSelector(ThemeSelectorRef reference, Catalog &&catalog, ThemeSelectorOptions options) {
  return detail::defaultThemeSelector(std::move(reference), catalog.choices, catalog.selection, catalog.initialId,
                                      std::move(options));
}

/// @endcond

/** @brief Exact host stage; does not opt into ordinary Overlay layout modifiers. */
struct OverlayHostModifier {
  std::size_t value; ///< Positive layer-registration cap, including hidden layers.

  /** @param description Consumed native host. @param modifier Positive registration cap.
   * @return Same host recipe with sparse checked settings.
   */
  friend Description<OverlayHost> operator|(Description<OverlayHost> &&description, OverlayHostModifier modifier) {
    return std::move(description).maxLayers(modifier.value);
  }
};

/** @param value Positive registration cap, including hidden layers. @return Exact native host stage. */
inline OverlayHostModifier maxLayers(std::size_t value) {
  return {value};
}

/** @brief Owning exact Dialog stage; no title resolution occurs while staging. */
struct DialogModifier {
  ~DialogModifier() = default;

  ControlSettings patch{}; ///< Only title and the nine dialog leaves are staged by these helpers.

  /** @param description Consumed native dialog. @param modifier Owning sparse caption/options.
   * @return Same native dialog; ui::frame retains its existing Border-wrapper semantics.
   */
  friend Description<Dialog> operator|(Description<Dialog> &&description, DialogModifier modifier) {
    if (modifier.patch.title) {
      (void)std::move(description).title(std::move(*modifier.patch.title));
    }

    return std::move(description).dialogSettings(modifier.patch.dialog);
  }
};

/** @brief Owning MessageBox-only stage, checked against actual native capability at preparation. */
struct MessageModifier {
  ~MessageModifier() = default;

  ControlSettings patch{}; ///< Owning body and grouped native message options.
  bool options{};          ///< Distinguish an explicit empty message group from a body stage.

  /** @param description Consumed actual native message. @param modifier Owning sparse stage.
   * @return Same native recipe; ordinary Dialog is rejected during checked preparation.
   */
  friend Description<Dialog> operator|(Description<Dialog> &&description, MessageModifier modifier) {
    if (modifier.patch.body) {
      (void)std::move(description).body(std::move(*modifier.patch.body));
    }
    if (modifier.options) {
      (void)std::move(description).messageSettings(std::move(modifier.patch.message));
    }

    return std::move(description);
  }
};

/** @param value Owning paragraph expression. @return Exact native MessageBox stage. */
inline MessageModifier body(TextValue value) {
  return {{.body = std::move(value)}, false};
}

/** @param value Literal paragraph bytes copied immediately. @return Exact native MessageBox stage. */
inline MessageModifier body(std::string_view value) {
  return body(TextValue(value));
}

/** @param value Owning sparse options, merged per leaf. @return Exact native MessageBox stage. */
inline MessageModifier messageSettings(ControlMessageSettings value) {
  return {{.message = std::move(value)}, true};
}

/** @param value Owning caption expression. @return Exact native Dialog stage. */
inline DialogModifier title(TextValue value) {
  return {{.title = std::move(value)}};
}

/** @param value Literal caption bytes, copied immediately. @return Exact native Dialog stage. */
inline DialogModifier title(std::string_view value) {
  return title(TextValue(value));
}

/** @param value Native dialog input policy. @return Exact native Dialog stage. */
inline DialogModifier mode(DialogMode value) {
  return {{.dialog = {.mode = value}}};
}

/** @brief Keep the main form available while the dialog is open.
 * @return Exact native Dialog stage selecting NON_MODAL input policy.
 * @details Equivalent to mode(DialogMode::NON_MODAL); preserves all other settings.
 * Later mode settings override this choice in declaration order.
 */
inline DialogModifier nonModal() {
  return mode(DialogMode::NON_MODAL);
}

/** @param value Native dialog placement. @return Exact native Dialog stage. */
inline DialogModifier placement(DialogPlacement value) {
  return {{.dialog = {.placement = value}}};
}

/** @param value Independent positive panel caps. @return Exact native Dialog stage. */
inline DialogModifier maximumPanelSize(CellSize value) {
  return {{.dialog = {.maximumPanelSize = {.width = value.width, .height = value.height}}}};
}

/** @param value Nonnegative viewport inset. @return Exact native Dialog stage. */
inline DialogModifier viewportMargin(std::int32_t value) {
  return {{.dialog = {.viewportMargin = value}}};
}

/** @param value Nonnegative anchor distance. @return Exact native Dialog stage. */
inline DialogModifier anchorGap(std::int32_t value) {
  return {{.dialog = {.anchorGap = value}}};
}

/** @param value Existing Escape cancellation policy. @return Exact native Dialog stage. */
inline DialogModifier closeOnEscape(bool value = true) {
  return {{.dialog = {.closeOnEscape = value}}};
}

/** @param options Opt-in top-frame close settings. @return Exact native Dialog stage. */
inline DialogModifier closeButton(DialogCloseOptions options = {}) {
  return {{.dialog = {.closeButton = {
                        .enabled = options.enabled, .placement = options.placement, .focusable = options.focusable}}}};
}

/** @param value Sparse options, including the mandatory frame. @return Exact native Dialog stage. */
inline DialogModifier dialogSettings(ControlDialogSettings value) {
  return {{.dialog = value}};
}

/** @brief Owning sparse native help presentation stage. */
struct KeyHelpModifier {
  ControlKeyFormatSettings formatter{};     ///< Sparse owning formatter leaves.
  std::optional<std::string> empty{};       ///< Optional literal empty presentation.
  std::optional<std::string> unavailable{}; ///< Optional literal unavailable presentation.

  /** @tparam T Exact native help presentation type.
   * @param description Consumed recipe. @param modifier Owning sparse patch.
   * @return Recipe staging the same checked leaves as its methods.
   */
  template <class T>
    requires(std::same_as<T, HelpView> || std::same_as<T, KeyHints>)
  friend Description<T> operator|(Description<T> &&description, KeyHelpModifier modifier) {
    (void)std::move(description).format(std::move(modifier.formatter));
    if (modifier.empty) {
      (void)std::move(description).emptyText(std::move(*modifier.empty));
    }
    if (modifier.unavailable) {
      (void)std::move(description).unavailableText(std::move(*modifier.unavailable));
    }
    return std::move(description);
  }
};

/** @param values Owning sparse formatter literals. @return Applicable native help stage. */
inline KeyHelpModifier format(ControlKeyFormatSettings values) {
  return {std::move(values), {}, {}};
}

/** @param value Owning literal empty text. @return Applicable native help stage. */
inline KeyHelpModifier emptyText(std::string value) {
  return {{}, std::move(value), {}};
}

/** @param value Owning literal unavailable text. @return Applicable native help stage. */
inline KeyHelpModifier unavailableText(std::string value) {
  return {{}, {}, std::move(value)};
}

/** @brief Owning exact HelpView initial-open stage. */
struct HelpOpenModifier {
  bool value; ///< Initial open state.

  /** @param description Consumed help recipe. @param modifier Initial state.
   * @return Recipe staging its checked initial-open leaf.
   */
  friend Description<HelpView> operator|(Description<HelpView> &&description, HelpOpenModifier modifier) {
    return std::move(description).initiallyOpen(modifier.value);
  }
};

/** @param value Initial open state. @return Exact HelpView stage. */
inline HelpOpenModifier initiallyOpen(bool value = true) {
  return {value};
}

/** @brief Owning exact KeyHints literal strip stage. */
struct KeyHintsModifier {
  ~KeyHintsModifier() = default;

  std::optional<std::string> separator; ///< Complete-entry separator.
  std::optional<std::string> overflow;  ///< Nonempty overflow marker.

  /** @param description Consumed hints recipe. @param modifier Owning literal strip patch.
   * @return Recipe staging its checked strip leaves.
   */
  friend Description<KeyHints> operator|(Description<KeyHints> &&description, KeyHintsModifier modifier) {
    if (modifier.separator) {
      (void)std::move(description).entrySeparator(std::move(*modifier.separator));
    }
    if (modifier.overflow) {
      (void)std::move(description).overflowMarker(std::move(*modifier.overflow));
    }
    return std::move(description);
  }
};

/** @param value Literal complete-entry separator. @return Exact KeyHints stage. */
inline KeyHintsModifier entrySeparator(std::string value) {
  return {std::move(value), {}};
}

/** @param value Nonempty literal overflow marker. @return Exact KeyHints stage. */
inline KeyHintsModifier overflowMarker(std::string value) {
  return {{}, std::move(value)};
}

/** @brief Constructor-only stage for a real owning default-catalog selector. */
struct ThemeCatalogModifier {
  ~ThemeCatalogModifier() = default;
  ControlThemeCatalogSettings patch{}; ///< Owning sparse admission and startup inputs.

  /** @param description Actual owning native catalog recipe.
   * @param modifier Owning constructor patch.
   * @return Consuming recipe; a view-only recipient fails typed validation.
   */
  friend Description<Dialog> operator|(Description<Dialog> &&description, ThemeCatalogModifier modifier) {
    return std::move(description).themeCatalogSettings(std::move(modifier.patch));
  }
};

/** @param values Sparse constructor inputs; absence preserves captured native options.
 * @return Owning constructor-only modifier, without a model or runtime.
 */
inline ThemeCatalogModifier themeCatalogSettings(ControlThemeCatalogSettings values) {
  return {std::move(values)};
}

/** @brief Sparse native ThemeSelector view stage; model/catalog inputs remain code-owned. */
struct ThemeSelectorModifier {
  ~ThemeSelectorModifier() = default;

  ControlThemeSelectorSettings patch{}; ///< Owning captions, limits and optional shortcut.

  /** @param description Actual code-bound selector. @param modifier Sparse owning view stage.
   * @return Same dialog recipe; actual semantic capability is checked during prepare.
   */
  friend Description<Dialog> operator|(Description<Dialog> &&description, ThemeSelectorModifier modifier) {
    return std::move(description).themeSelectorSettings(std::move(modifier.patch));
  }
};

/** @param values Sparse view options preserving actual captured values. @return Checked native view stage. */
inline ThemeSelectorModifier themeSelectorSettings(ControlThemeSelectorSettings values) {
  return {std::move(values)};
}

/** @brief Sparse immutable native notification-center construction stage. */
struct NotificationModifier {
  ControlNotificationSettings patch{}; ///< Numeric/native enum literals only; no runtime resolver is added.

  /** @param description Actual code-owned notifications recipe. @param modifier Sparse owning stage.
   * @return Same wrapper; wrong native recipes fail during checked preparation.
   */
  friend Description<Overlay> operator|(Description<Overlay> &&description, NotificationModifier modifier) {
    return std::move(description).notificationSettings(modifier.patch);
  }
};

/** @param values Sparse native center options; unspecified captured values survive.
 * @return Actual notifications construction stage, never a runtime post request.
 */
inline NotificationModifier notificationSettings(ControlNotificationSettings values) {
  return {values};
}
namespace detail {
/** @brief Compose declared layers through the existing host and one ordinary mount transaction.
 * @tparam Body Consuming ordinary body or exact explicit OverlayHost recipe.
 * @tparam Layers Consuming typed dialogs or erased recipes checked before publication.
 * @param body Existing ordinary content; an exact typed host takes priority over automatic wrapping.
 * @param layers Initially closed layers, appended in declaration order to that same host.
 * @return Owning erased recipe; every node/reference remains subject to the original transaction rollback.
 */
template <DescriptionInput Body, SceneLayer... Layers> Description<Widget> layered(Body &&body, Layers &&...layers) {
  auto hosted = [&] {
    if constexpr (std::same_as<std::remove_cvref_t<Body>, Description<OverlayHost>>) {
      return std::forward<Body>(body);
    } else {
      return overlayHost(std::forward<Body>(body));
    }
  }();

  auto layout = Access::captureLayouts(hosted, layers...);
  auto description =
    Access::transaction([hosted = std::move(hosted), layers = std::tuple(std::forward<Layers>(layers)...)](
                          ManagedScene &owner, Node<Widget> parent,
                          MountContext &context) mutable -> std::expected<Node<Widget>, ManagedError> {
      auto root = Access::mount(std::move(hosted), owner, parent, context);

      if (!root) {
        return std::unexpected(root.error());
      }

      auto host = Access::layerHost(owner, *root);

      if (!host) {
        return std::unexpected(host.error());
      }

      std::expected<void, ManagedError> result;

      std::apply(
        [&](auto &...layer) {
          const auto mountLayer = [&](auto &recipe) {
            if (!result) {
              return;
            }

            auto mounted = Access::mount(std::move(recipe), owner, *host, context);

            result = mounted ? Access::checkLayer(owner, *mounted) : std::unexpected(mounted.error());
          };

          (mountLayer(layer), ...);
        },
        layers);

      return result ? std::expected<Node<Widget>, ManagedError>(*root) : std::unexpected(result.error());
    });

  return Access::withLayout(std::move(description), std::move(layout));
}

} // namespace detail

/** @brief Declare scene layers without manually composing an OverlayHost.
 * @tparam Body Consuming body recipe, or the existing owning SceneFactory signature.
 * @tparam First First typed Dialog or erased resource layer; at least one layer is required.
 * @tparam Layers Further consuming layers of the same admitted kinds.
 * @param options Existing scene/runtime policies; ordinary scene(body,options,origin) remains unchanged.
 * @param origin Explicit construction site retained in mount diagnostics.
 * @param body Ordinary content, or its factory; an exact typed root OverlayHost is reused.
 * @param first Initially closed layer; erased roots must produce an exact bound Dialog.
 * @param layers Further initially closed layers, appended in declaration order.
 * @return Lazy owner of one existing host and one mount transaction. Failed composition publishes no refs.
 * Dialogs inside ordinary layout children are not hoisted. An erased body hiding a host is not a typed override.
 * Registered resource factory effects retain their existing contract and are not rolled back.
 */
template <class Body, detail::SceneLayer First, detail::SceneLayer... Layers>
  requires(detail::DescriptionInput<Body> || detail::SceneFactory<std::remove_cvref_t<Body>>)
Scene scene(SceneOptions options, std::source_location origin, Body &&body, First &&first, Layers &&...layers) {
  if constexpr (detail::DescriptionInput<Body>) {
    return scene(detail::layered(std::forward<Body>(body), std::forward<First>(first), std::forward<Layers>(layers)...),
                 options, origin);
  } else {
    return scene(
      [factory = std::forward<Body>(body),
       layers = std::tuple(std::forward<First>(first), std::forward<Layers>(layers)...)](
        ManagedScene &owner) mutable -> std::expected<Description<Widget>, ManagedError> {
        auto recipe = [&] {
          if constexpr (std::invocable<decltype(factory) &, ManagedScene &>) {
            return std::invoke(factory, owner);
          } else {
            return std::invoke(factory, owner, detail::SceneAccess::application(owner));
          }
        }();

        const auto compose = [&](auto &&content) {
          return std::apply(
            [&](auto &...layer) {
              return detail::layered(std::move(content), std::move(layer)...);
            },
            layers);
        };

        if constexpr (detail::IsDescription<decltype(recipe)>::value) {
          return compose(std::move(recipe));
        } else {
          if (!recipe) {
            return std::unexpected(recipe.error());
          }

          return compose(std::move(*recipe));
        }
      },
      options, origin);
  }
}

/** @brief Declare layers with explicit existing scene policies.
 * @tparam Body Consuming body recipe or existing SceneFactory.
 * @tparam First Consuming typed Dialog or erased recipe checked as an exact bound Dialog.
 * @tparam Layers Further consuming dialog layers.
 * @param options Existing scene/runtime policies.
 * @param body Ordinary content or its factory; a typed root OverlayHost is reused.
 * @param first First initially closed layer.
 * @param layers Further initially closed layers in declaration order.
 * @return The same lazy scene composition; use the explicit-origin overload for diagnostic attribution.
 */
template <class Body, detail::SceneLayer First, detail::SceneLayer... Layers>
  requires(detail::DescriptionInput<Body> || detail::SceneFactory<std::remove_cvref_t<Body>>)
Scene scene(SceneOptions options, Body &&body, First &&first, Layers &&...layers) {
  return scene(options, std::source_location::current(), std::forward<Body>(body), std::forward<First>(first),
               std::forward<Layers>(layers)...);
}

/** @brief Declare initially closed layers with the existing default scene policies.
 * @tparam Body Consuming body recipe or existing SceneFactory.
 * @tparam First Consuming typed Dialog or erased recipe checked as an exact bound Dialog.
 * @tparam Layers Further consuming dialog layers.
 * @param body Ordinary content or its factory; a typed root OverlayHost is reused.
 * @param first First initially closed layer.
 * @param layers Further initially closed layers in declaration order.
 * @return The same lazy scene composition; a scene without declared layers retains its original path.
 */
template <class Body, detail::SceneLayer First, detail::SceneLayer... Layers>
  requires(detail::DescriptionInput<Body> || detail::SceneFactory<std::remove_cvref_t<Body>>)
Scene scene(Body &&body, First &&first, Layers &&...layers) {
  return scene(SceneOptions{}, std::source_location::current(), std::forward<Body>(body), std::forward<First>(first),
               std::forward<Layers>(layers)...);
}

} // namespace portals::ui
