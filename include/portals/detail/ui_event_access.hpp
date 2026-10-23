// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_ref.hpp>
#include <portals/detail/quit_command.hpp>

namespace portals::ui {
namespace detail {
/** @brief Checked adapter to managed-owned layout observations; never owns a widget or scene. */
struct LayoutAccess {
  /** @tparam T Observed widget type.
   * @tparam Callback Owning compact geometry action.
   * @param scene Existing receiving managed owner.
   * @param reference Immutable-affinity checked observing reference.
   * @param callback Action transferred after scene identity admission.
   * @param origin Original registration site.
   * @return Success or exact checked admission/registration failure.
   */
  template <class T, class Callback>
  static std::expected<void, ManagedError> attach(ManagedScene &scene, const Ref<T> &reference, Callback callback,
                                                  std::source_location origin) {
    auto owner = eventScene(reference.target());
    if (!owner) {
      return std::unexpected(owner.error());
    }
    if (*owner != &scene) {
      return std::unexpected(
        ManagedError{ManagedPhase::BIND, reference.node().id(), TreeError::INVALID_ARGUMENT, {}, {}});
    }
    return scene.onLayout(reference.node(), std::move(callback), origin);
  }
};

/** @brief Checked adapter to the managed-owned click chain; never owns a widget or scene. */
struct ClickAccess {
  /** @tparam Callback Owning no-argument or ClickParams callback with a managed action result.
   * @param scene Existing owner in which the reference must have been published.
   * @param reference Observing button identity, read only after immutable affinity admission.
   * @param callback Owning callable transferred into the bounded managed registry.
   * @param origin Original registration site retained for callback failures.
   * @return Success or exact affinity, identity, registration or allocation error.
   */
  template <class Callback>
  static std::expected<void, ManagedError> attach(ManagedScene &scene, const Ref<Button> &reference, Callback callback,
                                                  std::source_location origin) {
    commandOrigin(callback, origin);
    auto owner = eventScene(reference.target());

    if (!owner) {
      return std::unexpected(owner.error());
    }

    if (*owner != &scene) {
      return std::unexpected(
        ManagedError{ManagedPhase::BIND, reference.node().id(), TreeError::INVALID_ARGUMENT, {}, {}});
    }

    try {
      return scene.composeButtonClick(reference.node(), scene.clickAdapter(std::move(callback), origin), origin);
    } catch (const std::bad_alloc &) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, reference.node().id(), TreeError::NO_MEMORY, {}, {}});
    } catch (...) {
      return std::unexpected(
        ManagedError{ManagedPhase::BIND, reference.node().id(), TreeError::CALLBACK_FAILED, {}, {}});
    }
  }
};

/** @brief Checked registration bridge to the editor's existing managed-owned binding. */
struct EditorEventAccess {
  /** @tparam Event TextEditedEvent or EditingFinishedEvent.
   * @tparam Callback Owning managed action receiving the event or no arguments.
   * @param scene Existing scene in which the editor reference was published.
   * @param reference Observing identity; no widget or scene ownership is retained.
   * @param callback Owning callable transferred to the combined finite editor registry.
   * @param origin Original registration site retained in failure diagnostics.
   * @return Exact affinity, identity, budget or checked allocation error.
   */
  template <class Event, class Callback>
  static std::expected<void, ManagedError> attach(ManagedScene &scene, const Ref<LineEdit> &reference,
                                                  Callback callback, std::source_location origin) {
    commandOrigin(callback, origin);
    auto owner = eventScene(reference.target());

    if (!owner) {
      return std::unexpected(owner.error());
    }

    if (*owner != &scene) {
      return std::unexpected(
        ManagedError{ManagedPhase::BIND, reference.node().id(), TreeError::INVALID_ARGUMENT, {}, {}});
    }

    if constexpr (std::same_as<Event, TextEditedEvent>) {
      return scene.onTextEdited(reference.node(), std::move(callback), origin);
    } else {
      return scene.onEditingFinished(reference.node(), std::move(callback), origin);
    }
  }
};

/** @brief Checked registration bridge to the checkbox's sole managed binding. */
struct ChangeAccess {
  /** @tparam T Checkbox view type. @tparam Callback Owning value or receipt action.
   * @param scene Existing receiving owner. @param reference Observing checkbox identity.
   * @param callback Action transferred after affinity admission. @param origin Original registration site.
   * @return Exact checked liveness, affinity, backing-model or registration failure.
   */
  template <class T, class Callback>
  static std::expected<void, ManagedError> attach(ManagedScene &scene, const Ref<T> &reference, Callback callback,
                                                  std::source_location origin) {
    commandOrigin(callback, origin);

    auto owner = eventScene(reference.target());

    if (!owner) {
      return std::unexpected(owner.error());
    }

    if (*owner != &scene) {
      return std::unexpected(
        ManagedError{ManagedPhase::BIND, reference.node().id(), TreeError::INVALID_ARGUMENT, {}, {}});
    }

    return scene.onChange(Node<CheckBox>{reference.node()}, std::move(callback), origin);
  }
};

/** @brief Admit callbacks borrowing the mutable chain context, or ordinary no-argument callbacks.
 * @tparam Callback Copyable callable whose result is void, checked TreeError or an editor receipt.
 */
template <class Callback>
concept ClickCallback = std::copy_constructible<Callback> && (requires(Callback &callback, ClickParams &params) {
                          { callback(params) } -> portals::detail::ManagedActionResult;
                        } || (!std::invocable<Callback &, ClickParams &> && requires(Callback &callback) {
                                                                { callback() } -> portals::detail::ManagedActionResult;
                                                              }));
} // namespace detail

} // namespace portals::ui
