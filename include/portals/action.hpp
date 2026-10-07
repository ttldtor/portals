// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/input.hpp>
#include <portals/widget.hpp>

namespace portals {
/** @brief Frontend-independent commands; text and paste remain distinct data events. */
enum class Action {
  activate,
  cancel,
  move_left,
  move_right,
  move_up,
  move_down,
  move_home,
  move_end,
  erase_backward,
  erase_forward
};

/** @brief Owning semantic command, usable without terminal input. */
struct ActionEvent {
  Action action{};                                      ///< Command identity.
  InputModifiers modifiers{};                           ///< Selection and other modifier intent.
  bool operator==(const ActionEvent &) const = default; ///< Value equality.
};

/** @brief Synchronous input value; callback borrows it only until return. */
using RoutedEvent = std::variant<ActionEvent, KeyEvent, TextInputEvent, PasteInputEvent, MouseInputEvent>;
/** @brief Explicit bubbling decision; ignored continues toward the original root. */
enum class EventReply {
  ignored,
  handled
};

/** @brief Distinguish a miss/ineligible target from an eligible unhandled event. */
enum class DispatchStatus {
  no_target,
  unhandled,
  handled
};

/** @brief Owning routing outcome; empty identities mean no eligible target/handler. */
struct DispatchResult {
  NodeId target{};                                  ///< Initial target, possibly removed during delivery.
  NodeId handler{};                                 ///< Node that consumed the event, possibly removed during delivery.
  DispatchStatus status{DispatchStatus::no_target}; ///< Delivery outcome; traversal may handle without a handler.
};

/** @brief Bounded router storage; no event queue is created. */
struct EventRouterOptions {
  std::size_t max_registrations{4096}; ///< Positive maximum; one registration per node.
};
namespace detail {
struct ActionRegistration;
struct ActionCancellation;
} // namespace detail
/** @brief Move-only scoped disconnect token; never owns a tree or widget.
 * Disconnect/destruction may run on any thread. It prevents future callback admission;
 * an already admitted callback finishes on UI. Captures are reclaimed on UI at the next
 * collecting UI operation or router destruction. Tokens may safely outlive the router.
 */
class ActionBinding {
public:
  ActionBinding() = default;                                ///< Empty disconnected token.
  ~ActionBinding();                                         ///< Disconnect without invoking user code.
  ActionBinding(ActionBinding &&other) noexcept;            ///< Transfer disconnect responsibility.
  ActionBinding &operator=(ActionBinding &&other) noexcept; ///< Disconnect old registration, then transfer.
  ActionBinding(const ActionBinding &) = delete;
  ActionBinding &operator=(const ActionBinding &) = delete;
  void disconnect() noexcept; ///< Idempotent; active calls retain their captures until return.
private:
  friend class EventRouter;
  explicit ActionBinding(std::weak_ptr<detail::ActionCancellation> registration);
  std::weak_ptr<detail::ActionCancellation> registration_;
};

/** @brief UI-owned focus and synchronous bubbling over an existing WidgetTree.
 * Tree and Scheduler must outlive the router. Destroy on its creating UI thread outside
 * a router callback; violation terminates. All operations reject wrong-thread calls before
 * touching user state. Dispatch/invoke/bind reject nested dispatch with would_block.
 * References borrowed by handlers cannot escape. No terminal bytes or queue are produced.
 * Allocation failures become no_memory; callback exceptions become callback_failed.
 * Focus changes request paint; geometry invalidation never disables keyboard access.
 */
class EventRouter {
public:
  /** @brief Validate the tree's owner thread and create a bounded router. */
  static std::expected<std::unique_ptr<EventRouter>, TreeError> create(WidgetTree &tree,
                                                                       EventRouterOptions options = {});
  ~EventRouter(); ///< Disconnect and reclaim all captures on UI; tree must still exist.
  EventRouter(const EventRouter &) = delete;
  EventRouter &operator=(const EventRouter &) = delete;

  /** @brief Register one typed handler; the scoped token owns disconnect responsibility.
   * Callback must be copy-constructible and return EventReply. Invocation uses tree.edit,
   * retaining the widget and callback captures even if either is removed during the call.
   * Duplicate registration is invalid_argument; full registry is node_limit. Removed or
   * disconnected records are reclaimed before accepting a new registration.
   */
  template <class T, class Callback>
  std::expected<ActionBinding, TreeError> bind(Node<T> node, Callback &&callback, bool focusable = true) {
    static_assert(std::same_as<std::invoke_result_t<Callback &, T &, const RoutedEvent &>, EventReply>);

    auto allowed = can_bind(node);

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    try {
      return add(
        node,
        [this, node, function = std::forward<Callback>(callback)](
          const RoutedEvent &event) mutable -> std::expected<EventReply, TreeError> {
          EventReply reply{EventReply::ignored};
          auto status = tree().edit(node, [&](T &widget) {
            reply = std::invoke(function, widget, event);
          });

          if (!status) {
            return std::unexpected(status.error());
          }

          return reply;
        },
        focusable);
    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::no_memory);
    } catch (...) {
      return std::unexpected(TreeError::callback_failed);
    }
  }

  /** @brief Recover focus if needed; empty Node means no eligible focusable registration.
   * Recovery chooses the most recently focused eligible node, then the first preorder node.
   */
  std::expected<Node<Widget>, TreeError> focused();
  /** @brief Focus a live eligible registered node, or return invalid_argument; requests paint. */
  std::expected<void, TreeError> set_focus(Node<Widget> node);
  /** @brief Traverse eligible registrations in tree preorder with wrap; reverse implements Shift+Tab. */
  std::expected<Node<Widget>, TreeError> traverse(bool reverse = false);
  /** @brief Deepest hit in reverse sibling order; only fresh, clipped geometry participates.
   * Returns an empty Node for a miss. Nonregistered nodes may be targets and bubble to ancestors.
   */
  std::expected<Node<Widget>, TreeError> hit_test(CellPoint point);
  /** @brief Deliver decoded input synchronously; replies/issues are ignored.
   * Unmodified Tab/Shift+Tab traverse. Enter/Escape/arrows/Home/End/Backspace/Delete map to actions;
   * release and other keys remain KeyEvent. Text (including space), paste and mouse retain type.
   * Left press focuses the nearest focusable target ancestor before bubbling the mouse event.
   */
  std::expected<DispatchResult, TreeError> dispatch(const InputEvent &event);
  /** @brief Invoke the same handler path with a semantic command; target need not be focusable.
   * Foreign/stale targets return their tree error. Ineligible targets return DispatchStatus::no_target.
   */
  std::expected<DispatchResult, TreeError> invoke(Node<Widget> target, ActionEvent action);

private:
  struct State;
  explicit EventRouter(std::unique_ptr<State> state);
  WidgetTree &tree();
  std::expected<void, TreeError> check() const;
  std::expected<void, TreeError> can_bind(Node<Widget> node);
  std::expected<ActionBinding, TreeError>
  add(Node<Widget> node, std::function<std::expected<EventReply, TreeError>(const RoutedEvent &)> callback,
      bool focusable);
  std::expected<void, TreeError> collect();
  std::shared_ptr<detail::ActionRegistration> registration(Node<Widget> node) const;
  std::expected<std::vector<Node<Widget>>, TreeError> focus_order();
  std::expected<void, TreeError> change_focus(Node<Widget> node);
  std::expected<DispatchResult, TreeError> route(Node<Widget> target, const RoutedEvent &event);
  std::unique_ptr<State> state_;
};
} // namespace portals
