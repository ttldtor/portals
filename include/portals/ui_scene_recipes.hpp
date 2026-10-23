// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_mount.hpp>
#include <portals/ui_scene.hpp>

namespace portals::ui {

namespace detail {
/** @brief Admit only owning descriptions or their exact managed checked result.
 * @tparam T Factory result after cv/ref normalization.
 */
template <class T> struct IsSceneRecipe : IsDescription<T> {};

/** @brief Admit a factory preserving an original managed error.
 * @tparam T Concrete widget constructed by the returned description.
 */
template <class T> struct IsSceneRecipe<std::expected<Description<T>, ManagedError>> : std::true_type {};

/** @brief Admit owning app results without widening managed root-recipe factories.
 * @tparam T Exact normalized no-argument factory result.
 */
template <class T> struct IsAppResult : IsSceneRecipe<T> {};

/** @brief An already configured owning facade is a complete app result. */
template <> struct IsAppResult<Scene> : std::true_type {};

/** @brief Preserve an exact managed failure before an owning facade exists. */
template <> struct IsAppResult<std::expected<Scene, ManagedError>> : std::true_type {};

/** @brief Admit only no-argument factories returning owning supported app results.
 * @tparam Factory Callable borrowed only through the synchronous upper runner boundary.
 */
template <class Factory>
concept AppFactory = std::invocable<Factory &> && !std::is_reference_v<std::invoke_result_t<Factory &>> &&
                     IsAppResult<std::remove_cvref_t<std::invoke_result_t<Factory &>>>::value;

/** @brief Select the legacy factory signature before considering an Application borrow.
 * @tparam Factory Owning scene factory returning an owning description or exact checked description.
 */
template <class Factory>
concept SceneFactory =
  (std::invocable<Factory &, ManagedScene &> && !std::is_reference_v<std::invoke_result_t<Factory &, ManagedScene &>> &&
   IsSceneRecipe<std::remove_cvref_t<std::invoke_result_t<Factory &, ManagedScene &>>>::value) ||
  (!std::invocable<Factory &, ManagedScene &> && std::invocable<Factory &, ManagedScene &, Application &> &&
   !std::is_reference_v<std::invoke_result_t<Factory &, ManagedScene &, Application &>> &&
   IsSceneRecipe<std::remove_cvref_t<std::invoke_result_t<Factory &, ManagedScene &, Application &>>>::value);

/** @brief Admit exact stationary setup results.
 * @tparam Setup Callable taking Scene& and returning void or exact expected<void,ManagedError>.
 */
template <class Setup>
concept AppSetup = requires(Setup &setup, Scene &owner) {
  { setup(owner) } -> std::same_as<void>;
} || requires(Setup &setup, Scene &owner) {
  { setup(owner) } -> std::same_as<std::expected<void, ManagedError>>;
};

} // namespace detail

/** @tparam T Concrete Widget type described by the consuming recipe.
 * @param description Owning root recipe consumed once by prepare; creation/capture allocation may throw.
 * @param options Existing runtime policies and bounded pending-configuration budget.
 * @param origin Construction call site retained across lazy mount.
 * @return UI-affine lazy facade; neither mount nor terminal I/O occurs during construction.
 */
template <class T>
Scene scene(Description<T> &&description, SceneOptions options = {},
            std::source_location origin = std::source_location::current()) {
  return detail::SceneAccess::create(
    [description = std::move(description),
     controls = options.controls](ManagedScene &owner) mutable -> std::expected<void, ManagedError> {
      auto mounted = mount(owner, std::move(description), controls);

      return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
    },
    options, origin);
}

/** @tparam Factory Owning callable returning Description<T> or expected<Description<T>, ManagedError>.
 * @param factory Invoked once at prepare with ManagedScene&, optionally followed by Application&;
 * the one-argument form has priority. May create states/models and application-backed services.
 * Explicitly borrowed services must outlive their use; construction/capture allocation may throw.
 * Application remains UI-only and facade-owned. It supplies UI services; run this owner through Scene,
 * never
 * Application::run directly from factory, configuration or shutdown. Stop external work with onStop before managed
 * teardown, and release borrowed services before the facade is destroyed.
 * @param options Existing runtime policies and finite facade limits.
 * @param origin Factory registration call site retained in mount diagnostics.
 * @return UI-affine lazy facade; the factory does not execute during construction.
 */
template <detail::SceneFactory Factory>
Scene scene(Factory factory, SceneOptions options = {}, std::source_location origin = std::source_location::current()) {
  return detail::SceneAccess::create(
    [factory = std::move(factory),
     controls = options.controls](ManagedScene &owner) mutable -> std::expected<void, ManagedError> {
      auto effectiveControls = controls;
      auto installed = detail::ControlContextAccess::installFallbackSchema(effectiveControls);

      if (!installed) {
        return std::unexpected(detail::controlContextError(installed.error(), owner.root()));
      }

      if (detail::ControlContextAccess::hasUserConfiguration(effectiveControls)) {
        for (const auto name : {"Border", "Gauge", "Spinner"}) {
          installed = detail::ControlContextAccess::installPrimitiveSchema(effectiveControls, name);

          if (!installed) {
            return std::unexpected(detail::controlContextError(installed.error(), owner.root()));
          }
        }

        for (const auto name : {"ScrollArea", "ScrollBar"}) {
          installed = detail::ControlContextAccess::installScrollSchema(effectiveControls, name);

          if (!installed) {
            return std::unexpected(detail::controlContextError(installed.error(), owner.root()));
          }
        }
        for (const auto name : {"HelpView", "KeyHints"}) {
          installed = detail::ControlContextAccess::installKeyHelpSchema(effectiveControls, name);
          if (!installed) {
            return std::unexpected(detail::controlContextError(installed.error(), owner.root()));
          }
        }
        for (const auto name : {"VirtualList", "VirtualTable", "Chart"}) {
          installed = detail::ControlContextAccess::installVirtualViewSchema(effectiveControls, name);

          if (!installed) {
            return std::unexpected(detail::controlContextError(installed.error(), owner.root()));
          }
        }

        for (const auto name : {"OverlayHost", "Dialog"}) {
          installed = detail::ControlContextAccess::installDialogSchema(effectiveControls, name);

          if (!installed) {
            return std::unexpected(detail::controlContextError(installed.error(), owner.root()));
          }
        }

        installed = detail::ControlContextAccess::installNotificationSchema(effectiveControls);

        if (!installed) {
          return std::unexpected(detail::controlContextError(installed.error(), owner.root()));
        }

        installed = detail::ControlContextAccess::installThemeCatalogSchema(effectiveControls);

        if (!installed) {
          return std::unexpected(detail::controlContextError(installed.error(), owner.root()));
        }

        installed = detail::ControlContextAccess::installMessageSchema(effectiveControls);

        if (!installed) {
          return std::unexpected(detail::controlContextError(installed.error(), owner.root()));
        }

        installed = detail::ControlContextAccess::installDataStateSchema(effectiveControls);

        if (!installed) {
          return std::unexpected(detail::controlContextError(installed.error(), owner.root()));
        }

        installed = detail::ControlContextAccess::installOverlaySchema(effectiveControls);
        if (!installed) {
          return std::unexpected(detail::controlContextError(installed.error(), owner.root()));
        }
      }
      auto prepared = effectiveControls.prepared();
      if (!prepared) {
        return std::unexpected(detail::controlContextError(prepared.error(), owner.root()));
      }
      auto recipe = [&] {
        if constexpr (std::invocable<Factory &, ManagedScene &>) {
          return std::invoke(factory, owner);
        } else {
          return std::invoke(factory, owner, detail::SceneAccess::application(owner));
        }
      }();

      if constexpr (detail::IsDescription<decltype(recipe)>::value) {
        auto mounted = mount(owner, std::move(recipe), *prepared);

        return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
      } else {
        if (!recipe) {
          return std::unexpected(recipe.error());
        }

        auto mounted = mount(owner, std::move(*recipe), *prepared);

        return mounted ? std::expected<void, ManagedError>{} : std::unexpected(mounted.error());
      }
    },
    options, origin);
}

} // namespace portals::ui
