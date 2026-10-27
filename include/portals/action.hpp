// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/input.hpp>
#include <portals/widget.hpp>

namespace portals {
/** @brief Frontend-independent commands; text and paste remain distinct data events. */
enum class Action {
  ACTIVATE,       ///< Activate the target's primary command.
  CANCEL,         ///< Cancel the current interaction.
  MOVE_LEFT,      ///< Move left according to the receiving widget.
  MOVE_RIGHT,     ///< Move right according to the receiving widget.
  MOVE_UP,        ///< Move up according to the receiving widget.
  MOVE_DOWN,      ///< Move down according to the receiving widget.
  MOVE_HOME,      ///< Move to the beginning according to the receiving widget.
  MOVE_END,       ///< Move to the end according to the receiving widget.
  ERASE_BACKWARD, ///< Erase before the current position.
  ERASE_FORWARD   ///< Erase after the current position.
};

/** @brief Origin of an admitted activation, without inferred physical input. */
enum class ClickSource {
  PROGRAMMATIC, ///< Explicit semantic action, the default for caller-created ActionEvent.
  KEYBOARD,     ///< Keyboard input, including a key translated into a semantic action.
  MOUSE         ///< Mouse input supplied by the existing router.
};

/** @brief Owning semantic command, usable without terminal input. */
struct ActionEvent {
  Action action{};                               ///< Command identity.
  InputModifiers modifiers{};                    ///< Selection and other modifier intent.
  ClickSource source{ClickSource::PROGRAMMATIC}; ///< Preserved origin when translating keyboard input.
  KeyAction keyAction{KeyAction::PRESS};         ///< Observed keyboard phase; independent of semantic action equality.

  /** @brief Compare semantic command intent, preserving the original equality contract.
   * @param other Command and modifier value to compare; source annotation is inspected separately.
   * @return True when action and modifiers are equal, regardless of observed source or keyboard phase.
   */
  constexpr bool operator==(const ActionEvent &other) const noexcept {
    return action == other.action && modifiers == other.modifiers;
  }
};

/** @brief Synchronous input value; callback borrows it only until return. */
using RoutedEvent = std::variant<ActionEvent, KeyEvent, TextInputEvent, PasteInputEvent, MouseInputEvent>;
/** @brief Explicit bubbling decision; ignored continues toward the original root. */
enum class EventReply {
  IGNORED, ///< Continue bubbling along the original ancestry.
  HANDLED  ///< Stop bubbling; this registration consumed the event.
};

/** @brief Distinguish a miss/ineligible target from an eligible unhandled event. */
enum class DispatchStatus {
  NO_TARGET, ///< No eligible destination was selected.
  UNHANDLED, ///< An eligible destination existed but no handler consumed the event.
  HANDLED    ///< A handler or built-in focus traversal consumed the event.
};

/** @brief Owning routing outcome; empty identities mean no eligible target/handler. */
struct DispatchResult {
  NodeId target{};                                  ///< Initial target, possibly removed during delivery.
  NodeId handler{};                                 ///< Node that consumed the event, possibly removed during delivery.
  DispatchStatus status{DispatchStatus::NO_TARGET}; ///< Delivery outcome; traversal may handle without a handler.
};

/** @brief Bounded router storage; no event queue is created. */
struct EventRouterOptions {
  std::size_t max_registrations{4096}; ///< Positive shared maximum of action and scope records; one of each per node.
};

/** @brief Host-declared input guarantees; this value does not enable terminal reporting modes. */
struct PointerCaptureCapabilities {
  bool motion{};               ///< Host supplies pointer motion while dragging.
  bool release{};              ///< Host supplies matching LEFT release or explicitly cancels a lost sequence.
  bool motionButtonIdentity{}; ///< MOVE button reliably reports held LEFT versus no/different button.
};

namespace detail {
struct ActionRegistration;
struct ActionCancellation;
struct FocusScopeRegistration;
struct PointerCaptureCancellation;
struct EventRouterAccess;
struct FocusChange;
struct DialogRecord;
} // namespace detail

/** @brief Move-only scoped disconnect token; never owns a tree or widget.
 * Disconnect/destruction may run on any thread. It prevents future callback admission;
 * an already admitted callback finishes on UI. Captures are reclaimed on UI at the next
 * collecting UI operation or router destruction. Tokens may safely outlive the router.
 */
class ActionBinding {
public:
  /** @brief Empty disconnected token. */
  ActionBinding() = default;
  /** @brief Disconnect without invoking user code. */
  ~ActionBinding();
  /** @brief Transfer disconnect responsibility.
   * @param other Token to empty and transfer into this token; valid on any thread.
   */
  ActionBinding(ActionBinding &&other) noexcept;
  /** @brief Disconnect old registration, then transfer.
   * @param other Token to empty and transfer after disconnecting this registration; self-assignment is a no-op.
   * @return This token after the transfer.
   */
  ActionBinding &operator=(ActionBinding &&other) noexcept;
  ActionBinding(const ActionBinding &) = delete;
  ActionBinding &operator=(const ActionBinding &) = delete;
  /** @brief Idempotent; active calls retain their captures until return. */
  void disconnect() noexcept;

private:
  friend class EventRouter;
  explicit ActionBinding(std::weak_ptr<detail::ActionCancellation> registration);
  friend struct detail::EventRouterAccess;
  std::weak_ptr<detail::ActionCancellation> registration_;
};

/** @brief Move-only weak cancellation token; owns no callback, widget, tree or router.
 * Destruction/disconnect may run on any thread and prevents future admission. Already admitted
 * delivery keeps its callable and widget alive until return. Tokens may outlive the router.
 * Eligibility and focus retirement are checked on UI, without background polling.
 */
class PointerCapture {
public:
  /** @brief Construct an empty disconnected token. */
  PointerCapture() = default;
  /** @brief Disconnect without user callbacks or tree access, on any thread. */
  ~PointerCapture();
  /** @brief Transfer cancellation responsibility and empty the source.
   * @param other Token to transfer; any thread, without invoking callbacks.
   */
  PointerCapture(PointerCapture &&other) noexcept;
  /** @brief Disconnect the old token, then transfer; self-assignment is a no-op.
   * @param other Token to empty and transfer.
   * @return This token; any thread, without callbacks or tree access.
   */
  PointerCapture &operator=(PointerCapture &&other) noexcept;
  PointerCapture(const PointerCapture &) = delete;
  PointerCapture &operator=(const PointerCapture &) = delete;
  /** @brief Idempotently deny future admission, without cancelling an already admitted call. */
  void disconnect() noexcept;
  /** @brief Observe cancellation flags without consulting live UI/tree eligibility.
   * @return True while capture and its action binding remain connected; false after disconnection,
   * release or router retirement. Use EventRouter::pointerCapture for checked effective eligibility.
   * @note Any thread; no allocation, callbacks, mutation or tree access.
   */
  [[nodiscard]] bool connected() const noexcept;

private:
  friend class EventRouter;
  explicit PointerCapture(std::weak_ptr<detail::PointerCaptureCancellation> cancellation) noexcept;
  std::weak_ptr<detail::PointerCaptureCancellation> cancellation_;
};

/** @brief UI-owned focus and synchronous bubbling over an existing WidgetTree.
 * Tree and Scheduler must outlive the router. Destroy on its creating UI thread outside
 * a router callback; violation terminates. All operations reject wrong-thread calls before
 * touching user state. Dispatch/invoke/bind reject nested dispatch with WOULD_BLOCK.
 * References borrowed by handlers cannot escape. No terminal bytes or queue are produced.
 * Allocation failures become NO_MEMORY; callback exceptions become CALLBACK_FAILED.
 * Focus changes request paint; geometry invalidation never disables keyboard access.
 */
class EventRouter {
public:
  /** @brief Validate the tree's owner thread and create a bounded router.
   * @param tree Borrowed live tree whose UI thread owns this router; it must outlive the router.
   * @param options Positive maximum number of live registrations.
   * @return New router, the tree's root-access error, INVALID_ARGUMENT for zero capacity, or NO_MEMORY.
   */
  static std::expected<std::unique_ptr<EventRouter>, TreeError> create(WidgetTree &tree,
                                                                       EventRouterOptions options = {});
  /** @brief Disconnect and reclaim all captures on UI; tree must still exist. */
  ~EventRouter();
  EventRouter(const EventRouter &) = delete;
  EventRouter &operator=(const EventRouter &) = delete;

  /** @brief Declare host motion/release guarantees, without terminal I/O.
   * @param capabilities Owning capability value; defaults are all false until explicitly declared.
   * @return Success, tree/router validation errors or WOULD_BLOCK during input/focus delivery.
   * A changed capability value cancels an active capture before further delivery. A paint failure
   * preserves the new declaration and retired capture, just like already-published focus effects.
   * @note UI-only; no callback, queue, timer or terminal mode is created.
   */
  std::expected<void, TreeError> setPointerCaptureCapabilities(PointerCaptureCapabilities capabilities);
  /** @brief Capture LEFT continuation from this owner's currently admitted LEFT PRESS handler.
   * @param owner Live full identity of the currently executing action registration, inside any active managed modal.
   * @param focusRoot Optional live ancestor containing owner, defaulting to owner. Only a subsequent
   * focus transition outside this subtree cancels capture; existing external/empty focus is permitted.
   * @return Weak cancellation token; tree/session/thread errors, INVALID_ARGUMENT for absent
   * capabilities/wrong owner/root, WOULD_BLOCK outside that press or while another capture is active,
   * NO_MEMORY, or IDENTITY_EXHAUSTED after the tree's input-generation stamp is exhausted.
   * Failure publishes no new capture. Pure LAYOUT invalidation does not cancel capture. Any
   * visibility/enabled/removal mutation conservatively cancels it, including unrelated nodes;
   * this protects hide/show and disable/enable pairs without a per-ancestor history observer.
   * A newly admitted modal also cancels an outside owner even when focusRoot spans the host.
   * @note UI-only; callable and widget borrows never escape their existing admission.
   */
  std::expected<PointerCapture, TreeError> capturePointer(Node<Widget> owner, Node<Widget> focusRoot = {});
  /** @brief Revalidate action identity and visible/enabled ancestry without consulting geometry.
   * @return Effective captured observer, empty when absent/retired, or a tree/router/paint error.
   * Retired records are unhooked before invalidation; no cancellation callback is invoked.
   * @note UI-only; no terminal focus-loss detection or polling is implied.
   */
  std::expected<Node<Widget>, TreeError> pointerCapture();
  /** @brief Explicitly end capture for host resize, stop, capability loss or application policy.
   * @param requestPaint Whether a surviving owner needs one PAINT; false is appropriate during stop/cleanup.
   * @return Success or tree/router/invalidation error. Capture is unhooked before a paint error;
   * a CLOSED tree also retires it without paint before returning CLOSED.
   * @note UI-only; allowed from the current input handler, without nested dispatch or user callbacks.
   */
  std::expected<void, TreeError> cancelPointerCapture(bool requestPaint = true);

  /** @brief Register one typed handler; the scoped token owns disconnect responsibility.
   * Callback must be copy-constructible and return EventReply. Invocation uses tree.edit,
   * retaining the widget and callback captures even if either is removed during the call.
   * Duplicate registration is INVALID_ARGUMENT; full registry is NODE_LIMIT. Removed or
   * disconnected records are reclaimed before accepting a new registration.
   * @tparam T Widget or a publicly derived extension type represented by node.
   * @tparam Callback Copy-constructible callable invocable as an lvalue with T& and const RoutedEvent&,
   * returning exactly EventReply.
   * @param node Live target; hidden or disabled nodes can register but cannot receive input until eligible.
   * @param callback Owned handler; borrowed widget/event references must not escape the invocation.
   * @param focusable Whether this registration participates in keyboard focus and traversal.
   * @return Disconnect token; tree validation errors, WOULD_BLOCK during dispatch/frame/collection,
   * INVALID_ARGUMENT for a duplicate, NODE_LIMIT for capacity, NO_MEMORY, or CALLBACK_FAILED.
   */
  template <class T, class Callback>
  std::expected<ActionBinding, TreeError> bind(Node<T> node, Callback &&callback, bool focusable = true) {
    return bind(node, std::forward<Callback>(callback), std::function<std::expected<void, TreeError>(T &, bool)>{},
                focusable);
  }

  /** @brief Register input and optional synchronous focus delivery in one scoped registration.
   * Focus loss precedes gain and the next routed input. Delivery does not bubble; a live hidden
   * or disabled previous target still receives loss. Removed/disconnected targets are skipped.
   * Callbacks may remove nodes; nested focus changes or input delivery return WOULD_BLOCK.
   * A callback error does not undo the published focus or model effects.
   * @tparam T Registered widget type.
   * @tparam Callback Owning copyable input handler returning EventReply.
   * @tparam FocusCallback Owning copyable callable returning expected<void, TreeError>.
   * @param node Live target of both callbacks.
   * @param callback Input handler; widget/event borrows expire upon return.
   * @param focusCallback Optional handler taking (T&, bool), true for gain and false for loss.
   * @param focusable Whether the node participates in focus selection.
   * @return One disconnect token or the same checked registration errors as input-only bind.
   */
  template <class T, class Callback, class FocusCallback>
    requires std::invocable<FocusCallback &, T &, bool>
  std::expected<ActionBinding, TreeError> bind(Node<T> node, Callback &&callback, FocusCallback &&focusCallback,
                                               bool focusable = true) {
    static_assert(std::same_as<std::invoke_result_t<Callback &, T &, const RoutedEvent &>, EventReply>);
    static_assert(std::same_as<std::invoke_result_t<FocusCallback &, T &, bool>, std::expected<void, TreeError>>);

    auto allowed = canBind(node);

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    try {
      std::function<std::expected<void, TreeError>(T &, bool)> focus(std::forward<FocusCallback>(focusCallback));
      std::function<std::expected<void, TreeError>(bool)> focusDelivery;

      if (focus) {
        focusDelivery = [this, node, function = std::move(focus)](bool focused) {
          std::expected<void, TreeError> outcome;
          auto status = tree().edit(node, [&](T &widget) {
            outcome = function(widget, focused);
          });

          return status ? outcome : status;
        };
      }

      return add(
        node,
        [this, node, function = std::forward<Callback>(callback)](
          const RoutedEvent &event) mutable -> std::expected<EventReply, TreeError> {
          EventReply reply{EventReply::IGNORED};
          auto status = tree().edit(node, [&](T &widget) {
            reply = std::invoke(function, widget, event);
          });

          if (!status) {
            return std::unexpected(status.error());
          }

          return reply;
        },
        focusable, std::move(focusDelivery));
    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::NO_MEMORY);
    } catch (...) {
      return std::unexpected(TreeError::CALLBACK_FAILED);
    }
  }

  /** @brief Declare a nested focus scope using an existing owning disconnect token.
   * Scope and action records share max_registrations. A scope can share its root with an action.
   * @param root Live subtree root; duplicate scopes return INVALID_ARGUMENT.
   * @param defaultTarget Optional live descendant (including root), skipped while ineligible or stale.
   * @return Scoped token, validation error, NODE_LIMIT or NO_MEMORY; failure publishes no scope.
   */
  std::expected<ActionBinding, TreeError> focusScope(Node<Widget> root, Node<Widget> defaultTarget = {});
  /** @brief Set the default of the nearest live enclosing scope without changing current focus.
   * @param node Live descendant to use when its scope has no eligible remembered target.
   * @return Success, INVALID_ARGUMENT without a scope, or tree/router validation errors.
   */
  std::expected<void, TreeError> focusDefault(Node<Widget> node);
  /** @brief Enter a declared scope at its eligible last/default/preorder target.
   * @param root Live registered scope root; hidden/disabled subtrees select empty focus.
   * @return Selected observer, empty for no eligible member or callback retirement, or typed error.
   * Callback errors preserve already published focus, exactly like setFocus.
   * A managed modal rejects an outside root with INVALID_ARGUMENT; dialog staging reentry is WOULD_BLOCK.
   */
  std::expected<Node<Widget>, TreeError> enterScope(Node<Widget> root);

  /** @brief Enter a scope while saving exact focus and ancestry for a later return.
   * This focus-only stack does not shield input or restrict global Tab traversal.
   * @param root Eligible declared scope, absent from the existing return stack.
   * @return Selected observer (possibly empty), INVALID_ARGUMENT for duplicate/ineligible entry,
   * or typed tree/allocation/callback errors. Before publication failure preserves the stack;
   * after publication callback errors retain both the new focus and its return record.
   */
  std::expected<Node<Widget>, TreeError> enterFocusScope(Node<Widget> root);
  /** @brief Leave the top scope and restore eligible saved focus, scope fallback or global fallback.
   * Ended outer records unwind together. Hidden/disabled/removed/disconnected scope roots also
   * unwind lazily on the next focus query or input dispatch. Saved identities include generations.
   * @param root Exact top activation root; other roots return INVALID_ARGUMENT.
   * @return Selected observer (possibly empty), validation/allocation/callback errors. A published
   * return consumes its record even when a blur callback fails. Nested focus delivery is WOULD_BLOCK.
   */
  std::expected<Node<Widget>, TreeError> leaveFocusScope(Node<Widget> root);

  /** @brief Recover focus if needed; empty Node means no eligible focusable registration.
   * Recovery tries containing scopes, then global history and the first preorder node.
   * If its callbacks retire the selected target, return empty; the next call may recover again.
   * @return Eligible focused node (possibly recovered), an empty observer if none exists,
   * or tree/router/callback errors or NO_MEMORY. Recovery requests paint when focus changes.
   */
  std::expected<Node<Widget>, TreeError> focused();
  /** @brief Focus a live eligible registered node, or return INVALID_ARGUMENT; requests paint.
   * @param node Live, eligible node with a focusable registration.
   * @return Success, INVALID_ARGUMENT for an ineligible/unregistered/nonfocusable node,
   * tree/router validation errors, or the first loss/gain callback error; published focus is not rolled back.
   * Managed modal confinement rejects outside nodes; reentry during dialog staging is WOULD_BLOCK.
   */
  std::expected<void, TreeError> setFocus(Node<Widget> node);
  /** @brief Traverse eligible registrations in tree preorder with wrap; reverse implements Shift+Tab.
   * Scope boundaries preserve the full global order; remembered/default entry is explicit via enterScope.
   * A managed modal restricts traversal to its active subtree.
   * @param reverse Traverse backward when true, forward otherwise; wrap at either end.
   * @return Newly focused node, empty if none exists or callbacks retire it, or tree/router/callback errors.
   */
  std::expected<Node<Widget>, TreeError> traverse(bool reverse = false);
  /** @brief Hit target in paint order; descendants and later sibling subtrees take precedence.
   * Only fresh, clipped geometry participates. Returns an empty Node for a miss.
   * Nonregistered nodes may be targets and bubble to ancestors.
   * @param point Surface cell coordinate tested against arranged rectangles and ancestor clips.
   * @return Last hit in preorder (later siblings cover earlier ones), empty on a miss,
   * or tree/router validation errors or NO_MEMORY.
   */
  std::expected<Node<Widget>, TreeError> hitTest(CellPoint point);
  /** @brief Deliver decoded input synchronously; replies/issues are ignored.
   * Unmodified Tab/Shift+Tab traverse. Enter/Escape/arrows/Home/End/Backspace/Delete map to actions;
   * release and other keys remain KeyEvent. Text (including space), paste and mouse retain type.
   * Left press focuses the nearest focusable target ancestor before bubbling the mouse event.
   * If focus callbacks retire the original hit, suppress delivery without retargeting it.
   * Managed nonmodal pointer input does not steal focus. A modal consumes outside physical input,
   * and layer bubbling stops at the structural dialog root.
   * @param event Decoded input borrowed for this call; handlers must not retain references.
   * @return Initial target, consuming handler and delivery status; NO_TARGET for a miss/ignored decoder event,
   * tree/router errors, WOULD_BLOCK for nested delivery, NO_MEMORY, or CALLBACK_FAILED.
   */
  std::expected<DispatchResult, TreeError> dispatch(const InputEvent &event);
  /** @brief Invoke the same handler path with a semantic command; target need not be focusable.
   * Foreign/stale targets return their tree error. Ineligible targets return DispatchStatus::NO_TARGET.
   * A managed modal rejects an explicit outside target with INVALID_ARGUMENT.
   * @param target Explicit live destination; focus is unchanged.
   * @param action Owning semantic command and modifier intent to bubble along the original ancestry.
   * @return Initial target, consuming handler and delivery status; tree/router errors,
   * WOULD_BLOCK for nested delivery, NO_MEMORY, or CALLBACK_FAILED.
   */
  std::expected<DispatchResult, TreeError> invoke(Node<Widget> target, ActionEvent action);

private:
  friend class ManagedScene;
  friend struct detail::EventRouterAccess;
  std::uint64_t focusGeneration() const noexcept;
  struct State;
  explicit EventRouter(std::unique_ptr<State> state);
  WidgetTree &tree();
  std::expected<void, TreeError> check() const;
  std::expected<void, TreeError> canBind(Node<Widget> node);
  std::expected<ActionBinding, TreeError>
  add(Node<Widget> node, std::function<std::expected<EventReply, TreeError>(const RoutedEvent &)> callback,
      bool focusable, std::function<std::expected<void, TreeError>(bool)> focusCallback);
  std::expected<void, TreeError> collect();
  std::shared_ptr<detail::ActionRegistration> registration(Node<Widget> node) const;
  std::expected<std::vector<Node<Widget>>, TreeError> focusOrder(Node<Widget> reveal = {}, Node<Widget> exclude = {});
  std::shared_ptr<detail::DialogRecord> dialogLayer(Node<Widget> node) const;
  Node<Widget> modalRoot(Node<Widget> exclude = {}) const;
  bool dialogEligible(Node<Widget> node, Node<Widget> reveal = {}, Node<Widget> exclude = {}) const;
  bool notificationHit(Node<Widget> node) const;
  std::expected<void, TreeError> stageFocus(Node<Widget> node, detail::FocusChange &change);
  void publishFocus(detail::FocusChange &change) noexcept;
  std::expected<void, TreeError> notifyFocus(const detail::FocusChange &change);
  detail::FocusScopeRegistration *scope(Node<Widget> root) const;
  bool within(Node<Widget> root, Node<Widget> node) const;
  Node<Widget> scopeEntry(Node<Widget> root, const std::vector<Node<Widget>> &order, bool reverse) const;
  Node<Widget> entering(Node<Widget> node, Node<Widget> previous, const std::vector<Node<Widget>> &order,
                        bool reverse) const;
  bool endedFocusScope(std::size_t index) const;
  std::expected<Node<Widget>, TreeError> returnFocusScopes(std::size_t first);
  std::expected<bool, TreeError> recoverFocusScopes();
  Node<Widget> fallbackFocus(const std::vector<Node<Widget>> &scopes, const std::vector<Node<Widget>> &order) const;
  std::expected<void, TreeError> changeFocus(Node<Widget> node);
  std::expected<void, TreeError> deliverFocus(Node<Widget> node, bool focused);
  std::expected<Node<Widget>, TreeError> checkedFocus();
  std::expected<DispatchResult, TreeError> route(Node<Widget> target, const RoutedEvent &event, bool physical = false);
  std::expected<EventReply, TreeError> deliver(std::shared_ptr<detail::ActionRegistration> entry,
                                               const RoutedEvent &event);
  std::expected<Node<Widget>, TreeError> checkedPointerCapture();
  std::expected<void, TreeError> retirePointerCapture(bool requestPaint);
  std::expected<DispatchResult, TreeError> routeCaptured(const MouseInputEvent &event);
  std::unique_ptr<State> state_;
};
} // namespace portals
