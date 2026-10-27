// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "dialog_state.hpp"
#include "scroll_intent.hpp"
#include "toast_state.hpp"
#include <atomic>
#include <limits>
#include <optional>
#include <portals/action.hpp>
#include <thread>

namespace portals::detail {
// Worker-side tokens share only this admission flag. They never own callback captures,
// whose potentially reentrant destructors belong to UI collection or active delivery.
struct ActionCancellation {
  std::atomic<bool> connected{true};
};

struct FocusScopeRegistration {
  Node<Widget> root;
  Node<Widget> defaultTarget;
  Node<Widget> last;
  std::shared_ptr<ActionCancellation> cancellation;
};

struct FocusReturnRegistration {
  Node<Widget> root;
  Node<Widget> previous;
  std::vector<Node<Widget>> previousScopes;
  std::shared_ptr<ActionCancellation> cancellation;
  bool dialog{};
  std::uint64_t activation{};

  // Exact internal receipt. Exhaustion never rejects legacy focus operations or reuses a serial.
  void publish(std::uint64_t &serial) noexcept {
    if (serial != std::numeric_limits<std::uint64_t>::max()) {
      activation = ++serial;
    }
  }
};

struct FocusChange {
  Node<Widget> previous;
  Node<Widget> next;
  std::vector<Node<Widget>> ancestry = std::vector<Node<Widget>>(0);
  std::unique_ptr<FocusReturnRegistration> entry;
};

struct ActionRegistration {
  Node<Widget> node;
  bool focusable{};
  std::shared_ptr<ActionCancellation> cancellation;
  std::function<std::expected<EventReply, TreeError>(const RoutedEvent &)> callback;
  std::function<std::expected<void, TreeError>(bool)> focusCallback;
};

// Worker tokens own neither callbacks nor widgets; registration identity is a separate weak owner.
struct PointerCaptureCancellation {
  std::atomic<bool> connected{true};
  std::weak_ptr<ActionCancellation> binding;
};

struct PointerCaptureRecord {
  Node<Widget> node;
  Node<Widget> focusRoot;
  std::weak_ptr<ActionRegistration> registration;
  std::shared_ptr<PointerCaptureCancellation> cancellation;
  std::uint64_t inputGeneration{};
};

// Internal checked adapter for native installers and contract tests. It keeps the same tree.edit
// admission/pin as public bind; the checked reply uses the router's existing erased callback type.
struct EventRouterAccess {
  static std::weak_ptr<ActionCancellation> bindingCancellation(const ActionBinding &binding) noexcept {
    return binding.registration_;
  }

  static std::expected<std::shared_ptr<const FocusObservation>, TreeError> observeFocus(EventRouter &router);
  static void exhaustFocusReturnSerial(EventRouter &router) noexcept;
  static Node<Widget> requiredFocusReceipt(const EventRouter &router) noexcept;

  template <class T, class Callback>
  static std::expected<ActionBinding, TreeError> bindChecked(EventRouter &router, Node<T> node, Callback callback,
                                                             bool focusable = true) {
    static_assert(
      std::same_as<std::invoke_result_t<Callback &, T &, const RoutedEvent &>, std::expected<EventReply, TreeError>>);

    auto allowed = router.canBind(node);

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    try {
      return router.add(node,
                        [&router, node, function = std::move(callback)](
                          const RoutedEvent &event) mutable -> std::expected<EventReply, TreeError> {
                          std::expected<EventReply, TreeError> reply{EventReply::IGNORED};
                          auto edited = router.tree().edit(node, [&](T &widget) {
                            reply = function(widget, event);
                          });

                          return edited ? reply : std::unexpected(edited.error());
                        },
                        focusable, {});
    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::NO_MEMORY);
    } catch (...) {
      return std::unexpected(TreeError::CALLBACK_FAILED);
    }
  }
};
} // namespace portals::detail

namespace portals {
struct EventRouter::State {
  WidgetTree &tree;
  EventRouterOptions options;
  std::thread::id owner{std::this_thread::get_id()};
  std::vector<std::shared_ptr<detail::ActionRegistration>> registrations =
    std::vector<std::shared_ptr<detail::ActionRegistration>>(0);
  std::vector<Node<Widget>> history = std::vector<Node<Widget>>(0);
  std::vector<detail::FocusScopeRegistration> scopes = std::vector<detail::FocusScopeRegistration>(0);
  // Exact scope ancestry survives removal of the focused node until the next recovery.
  std::vector<Node<Widget>> focusScopes = std::vector<Node<Widget>>(0);
  // Owning records keep exact saved ancestry across subtree retirement and slot reuse.
  std::vector<std::unique_ptr<detail::FocusReturnRegistration>> focusReturns =
    std::vector<std::unique_ptr<detail::FocusReturnRegistration>>(0);
  Node<Widget> focus;
  std::uint64_t focusGeneration{1};
  std::uint64_t focusReturnSerial{};
  Node<Widget> requiredFocusReceipt;
  std::weak_ptr<detail::FocusObservation> focusObservation;
  PointerCaptureCapabilities pointerCapabilities;
  std::optional<detail::PointerCaptureRecord> pointerCapture;
  // Only the dynamically executing handler can acquire capture from its own LEFT PRESS.
  std::weak_ptr<detail::ActionRegistration> admittedAction;
  std::optional<MouseInputEvent> admittedMouse;
  // These UI-only guards reject nested delivery and mutations during capture reclamation;
  // callback disconnect uses the independent atomic flag and remains available on any thread.
  bool dispatching{};
  bool deliveringFocus{};
  bool collecting{};
  bool closing{};
  bool dialogCancelRepeat{};
  bool dialogEnter{};
  bool dialogEnterRepeat{};
  std::weak_ptr<detail::DialogHostRecord> dialogs;
  std::weak_ptr<detail::NotificationRecord> notifications;
};
} // namespace portals

inline void portals::detail::EventRouterAccess::exhaustFocusReturnSerial(EventRouter &router) noexcept {
  router.state_->focusReturnSerial = std::numeric_limits<std::uint64_t>::max();
}

inline portals::Node<portals::Widget>
portals::detail::EventRouterAccess::requiredFocusReceipt(const EventRouter &router) noexcept {
  return router.state_->requiredFocusReceipt;
}
