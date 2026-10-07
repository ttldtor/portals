// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/scheduler.hpp>
#include <portals/widget_types.hpp>

#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace portals {
class WidgetTree;
/** @brief UI-owned extension object. Hooks borrow their arguments for one call.
 * Models are separate explicit owners chosen by the application. Hooks must not block,
 * perform terminal I/O, or retain contexts. Measure/arrange/paint cannot mutate the tree.
 */
class Widget {
public:
  /** @brief Destroy on UI after the last active tree borrow. */
  virtual ~Widget() = default;
  /** @brief Called exactly once after attachment; exceptions are reported. */
  virtual void mount();
  /** @brief Called once before destruction, children first; cleanup continues on exceptions. */
  virtual void unmount();
  /** @brief Default desired size is empty.
   * @param available Normalized cell-space constraints supplied by WidgetTree.
   * @return Desired size (empty by default), or an extension-defined measurement failure.
   */
  virtual std::expected<CellSize, TreeError> measure(CellSize available);
  /** @brief Default accepts the owning rectangle.
   * @param local Parent-relative rectangle selected by the layout owner.
   * @return Success by default, or an extension-defined arrangement failure.
   */
  virtual std::expected<void, TreeError> arrange(CellRect local);
  /** @brief Default emits no commands.
   * @param context Borrowed synchronous sink; neither it nor frame text may escape the frame.
   * @return Success after emitting commands (none by default), or a painting/sink failure.
   */
  virtual std::expected<void, TreeError> paint(PaintContext &context);
};
/** @brief Copyable typed observer; never owns or exposes an unchecked widget pointer.
 * @tparam T Widget or a publicly derived extension type; the handle records identity without owning T.
 */
template <class T = Widget> class Node {
public:
  /** @brief Empty observer; checked access fails. */
  Node() = default;
  /** @brief Safe observing upcast; downcasts are unavailable.
   * @tparam U Publicly derived from T, permitting a safe observing upcast.
   * @param other Observer whose identity is copied; the source remains valid.
   */
  template <class U>
    requires std::derived_from<U, T>
  Node(Node<U> other) : id_(other.id()) {
  }
  /** @brief Copy the stable diagnostic identity.
   * @return Copied session/slot/generation identity; it does not establish liveness.
   */
  NodeId id() const noexcept {
    return id_;
  }
  /** @brief Compare identity.
   * @param other Typed observer to compare.
   * @return True when all compared fields are equal.
   */
  bool operator==(const Node &other) const noexcept = default;

private:
  friend class WidgetTree;
  explicit Node(NodeId id) : id_(id) {
  }

  NodeId id_{};
};
/** @brief Owning snapshot of a live node; handles remain checked after subsequent mutations. */
struct NodeSnapshot {
  Node<Widget> parent; ///< Empty only for the root.
  /** @brief Live children in insertion order. */
  std::vector<Node<Widget>> children = std::vector<Node<Widget>>(0);
  NodeGeometry geometry; ///< Owning geometry snapshot.
  bool visible{true};    ///< Hidden subtrees do not participate in layout, paint or input.
  bool enabled{true};    ///< Disabled subtrees retain layout/paint but do not receive input.
};
/** @brief Exclusive parent/child ownership on Scheduler's UI thread.
 * Scheduler must outlive this object. Destruction on another thread or inside a tree
 * callback terminates; request close instead. All operations check affinity first.
 * Logical removal is immediate and allocation-free, reclamation waits for the outermost
 * borrow. References cannot escape edit/with_scope/frame callbacks. Library allocations
 * become no_memory; callback exceptions become callback_failed without undoing user effects.
 */
class WidgetTree {
public:
  /** @brief Nonthrowing UI error sink; context is borrowed until tree destruction. */
  using ErrorSink = void (*)(void *, NodeId, TreeError) noexcept;
  /** @brief Create a mounted plain root and bounded registry; no partial tree on failure.
   * @param scheduler Borrowed UI executor that must outlive the tree; call on its owner thread.
   * @param options Positive registry and depth bounds; max_depth must not exceed 256.
   * @param sink Optional nonthrowing UI failure sink; recursive reports are suppressed.
   * @param context Borrowed sink context that must outlive the tree.
   * @return Tree with a mounted root, or invalid_argument, no_memory, identity_exhausted,
   * or the translated scheduler scope-creation error.
   */
  static std::expected<std::unique_ptr<WidgetTree>, TreeError>
  create(Scheduler &scheduler, WidgetTreeOptions options = {}, ErrorSink sink = nullptr, void *context = nullptr);
  /** @brief Close and reclaim on UI, outside all callbacks. */
  ~WidgetTree();
  WidgetTree(const WidgetTree &) = delete;
  WidgetTree &operator=(const WidgetTree &) = delete;
  /** @brief Checked observing root; stale after root removal.
   * @return Observing root handle, or wrong_thread, closed, or stale_node if the root was removed.
   */
  std::expected<Node<Widget>, TreeError> root() const;
  /** @brief Copy live structure; may allocate.
   * @param node Checked identity whose live structure is copied.
   * @return Owning snapshot of children, parent, flags and geometry; tree validation errors or no_memory on failure.
   */
  std::expected<NodeSnapshot, TreeError> inspect(Node<Widget> node) const;
  /** @brief Invalidate subtree/scopes now, reclaim at safe point.
   * @param node Root of the subtree to invalidate and eventually destroy.
   * @return Success after logical removal and layout invalidation; tree validation errors, or would_block during a
   * frame.
   */
  std::expected<void, TreeError> remove(Node<Widget> node);
  /** @brief Idempotent coordinated shutdown, including during edit.
   * @return Success once the tree is logically closed; wrong_thread or would_block during a frame otherwise.
   */
  std::expected<void, TreeError> close();

  /** @brief Borrow a const extension object, including inside frame traversal.
   * The external layout owner can read its custom layout interface, then measure/arrange
   * children using inspect snapshots. No reference may escape; callback returns void.
   * @tparam T Widget or a publicly derived extension type represented by the checked observer.
   * @tparam Callback Callable accepting const T& and returning void.
   * @param node Live widget to borrow without permitting tree mutation through this reference.
   * @param callback Synchronous reader; must not retain the widget reference.
   * @return Success after callback return, tree validation errors before invocation, or callback_failed on an
   * exception.
   */
  template <class T, class Callback> std::expected<void, TreeError> read(Node<T> node, Callback &&callback) {
    static_assert(std::is_void_v<std::invoke_result_t<Callback, const T &>>);
    auto value = access(node.id(), false);

    if (!value) {
      return std::unexpected(value.error());
    }

    Borrow guard(*this);

    try {
      std::invoke(std::forward<Callback>(callback), static_cast<const T &>(**value));

      return {};
    } catch (...) {
      report(node.id(), TreeError::callback_failed);

      return std::unexpected(TreeError::callback_failed);
    }
  }

  /** @brief Construct and exclusively attach a new child; constructors run only after validation.
   * No attach/reparent exists, so cycles cannot be formed. Structural insertion during lifecycle
   * or frame callbacks returns would_block. A committed mount exception is reported to the sink;
   * insertion still succeeds. Mount can remove itself, so a successful handle can already be stale.
   * @tparam T Publicly derived Widget constructible from the forwarded Args.
   * @tparam Args Argument types accepted by T's constructor; perfect-forwarded without library copies.
   * @param parent Live parent whose ownership will contain the new child.
   * @param args Constructor arguments forwarded only after insertion checks.
   * @return Observing child handle; tree validation/capacity/depth/identity failures, would_block at forbidden
   * boundaries, no_memory for allocation, or callback_failed for a constructor exception.
   */
  template <class T, class... Args>
    requires std::derived_from<T, Widget>
  std::expected<Node<T>, TreeError> emplace(Node<Widget> parent, Args &&...args) {
    auto allowed = can_insert(parent.id());

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    Borrow guard(*this);

    try {
      auto value = std::make_unique<T>(std::forward<Args>(args)...);
      auto id = attach(parent.id(), std::move(value));

      if (!id) {
        return std::unexpected(id.error());
      }

      return Node<T>(*id);
    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::no_memory);
    } catch (...) {
      report(parent.id(), TreeError::callback_failed);

      return std::unexpected(TreeError::callback_failed);
    }
  }

  /** @brief Borrow T only during a void callback; nested edit/removal is supported.
   * Forbidden during measure/arrange/paint. No automatic invalidation: request it explicitly.
   * @tparam T Widget or a publicly derived extension type represented by the checked observer.
   * @tparam Callback Callable accepting T& and returning void.
   * @param node Live widget to borrow for the duration of the callback.
   * @param callback Synchronous editor; must not retain references and must request its own invalidation.
   * @return Success, tree validation errors, would_block during a frame, or callback_failed on an exception.
   */
  template <class T, class Callback> std::expected<void, TreeError> edit(Node<T> node, Callback &&callback) {
    static_assert(std::is_void_v<std::invoke_result_t<Callback, T &>>);
    auto value = access(node.id(), true);

    if (!value) {
      return std::unexpected(value.error());
    }

    Borrow guard(*this);

    try {
      std::invoke(std::forward<Callback>(callback), static_cast<T &>(**value));

      return {};
    } catch (...) {
      report(node.id(), TreeError::callback_failed);

      return std::unexpected(TreeError::callback_failed);
    }
  }

  /** @brief Borrow the node-owned scope for registration with the existing Scheduler.
   * Registrations borrowing widgets must use edit for their entire external callback, or
   * connect/at below. The const scope must not be retained or moved out of the node.
   * @tparam Callback Callable accepting const UiScope&; its return value is ignored.
   * @param node Live node whose scope is borrowed outside a frame.
   * @param callback Synchronous registration code; must not retain the scope reference.
   * @return Success, tree validation errors, would_block during a frame, or callback_failed on an exception.
   */
  template <class Callback> std::expected<void, TreeError> with_scope(Node<Widget> node, Callback &&callback) {
    auto value = access(node.id(), true);

    if (!value) {
      return std::unexpected(value.error());
    }

    Borrow guard(*this);

    try {
      std::invoke(std::forward<Callback>(callback), scope(node.id()));

      return {};
    } catch (...) {
      report(node.id(), TreeError::callback_failed);

      return std::unexpected(TreeError::callback_failed);
    }
  }

  /** @brief Node-scoped typed delivery; the whole callback borrows T until return.
   * Owns callback captures, returns the ordinary Scheduler token/publisher; no second bus.
   * Exceptions report to the tree sink and disconnect through Scheduler's error boundary.
   * @tparam Value Transitively immutable snapshot type supporting destruction on any releasing thread.
   * @tparam T Widget or a publicly derived extension type represented by node.
   * @tparam Callback Copy-constructible callable accepting T& and std::shared_ptr<const Value>
   * as an lvalue; its return value is ignored.
   * @param node Live receiver whose scope cancels delivery on removal.
   * @param callback Callable copied/moved into the scheduler; widget references must not escape.
   * @param lane Scheduler queue whose capacity and budget apply.
   * @param overflow Queue rejection or sender-local pending data replacement policy.
   * @return Scoped subscription and worker-safe publisher; tree validation errors, would_block during a frame,
   * translated scheduler registration errors, or no_memory.
   * @note Non-allocation exceptions from callback construction/move propagate.
   */
  template <class Value, class T, class Callback>
  std::expected<SchedulerConnection<Value>, TreeError> connect(Node<T> node, Callback &&callback,
                                                               SchedulerLane lane = SchedulerLane::data,
                                                               SchedulerOverflow overflow = SchedulerOverflow::reject) {
    auto value = access(node.id(), true);

    if (!value) {
      return std::unexpected(value.error());
    }

    Borrow guard(*this);

    try {
      auto result = scheduler().connect<Value>(
        scope(node.id()),
        [this, node, function = std::forward<Callback>(callback)](std::shared_ptr<const Value> item) mutable {
          auto status = edit(node, [&](T &widget) {
            std::invoke(function, widget, std::move(item));
          });

          if (!status && status.error() == TreeError::callback_failed) {
            throw TreeError::callback_failed;
          }
        },
        lane, overflow);

      if (!result) {
        return std::unexpected(from_scheduler(result.error()));
      }

      return std::move(*result);
    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::no_memory);
    }
  }

  /** @brief Node-scoped one-shot timer, with the same checked borrow as connect.
   * @tparam T Widget or a publicly derived extension type represented by node.
   * @tparam Callback Copy-constructible callable accepting T& as an lvalue; its return value is ignored.
   * @param node Live receiver whose scope cancels the timer on removal.
   * @param deadline Absolute millisecond deadline in the scheduler clock.
   * @param callback Callable copied/moved into the scheduler; widget references must not escape.
   * @return Scoped timer token; tree validation errors, would_block during a frame, translated scheduler
   * registration errors, or no_memory.
   * @note Non-allocation exceptions from callback construction/move propagate.
   */
  template <class T, class Callback>
  std::expected<Subscription, TreeError> at(Node<T> node, SchedulerTime deadline, Callback &&callback) {
    auto value = access(node.id(), true);

    if (!value) {
      return std::unexpected(value.error());
    }

    Borrow guard(*this);

    try {
      auto result =
        scheduler().at(scope(node.id()), deadline, [this, node, function = std::forward<Callback>(callback)]() mutable {
          auto status = edit(node, [&](T &widget) {
            std::invoke(function, widget);
          });

          if (!status && status.error() == TreeError::callback_failed) {
            throw TreeError::callback_failed;
          }
        });

      if (!result) {
        return std::unexpected(from_scheduler(result.error()));
      }

      return std::move(*result);
    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::no_memory);
    }
  }

  /** @brief No-op if equal; otherwise layout.
   * @param node Live node whose flag is changed.
   * @param visible Whether this node and its subtree participate in layout, paint and input.
   * @return Success, including equal-value no-op, or tree validation errors including would_block during a frame.
   *
   * @note Affinity, lifetime and frame checks also apply when the requested flag already matches.
   */
  std::expected<void, TreeError> set_visible(Node<Widget> node, bool visible);
  /** @brief No-op if equal; otherwise paint.
   * @param node Live node whose flag is changed.
   * @param enabled Whether this node and its subtree can receive input; layout/paint remain available.
   * @return Success, including equal-value no-op, or tree validation errors including would_block during a frame.
   *
   * @note Affinity, lifetime and frame checks also apply when the requested flag already matches.
   */
  std::expected<void, TreeError> set_enabled(Node<Widget> node, bool enabled);
  /** @brief Live/visible/enabled ancestors; hit-test also needs valid geometry.
   * @param node Live node whose ancestry is checked.
   * @return True when the node and all ancestors are visible and enabled, false otherwise;
   * tree validation errors for an invalid observer. Geometry is not consulted.
   */
  std::expected<bool, TreeError> input_eligible(Node<Widget> node) const;
  /** @brief Coalesce one frame request.
   * @param node Live node authorizing invalidation of the tree.
   * @param kind Work to coalesce; layout invalidates all arranged geometry and includes paint.
   * @return Success, tree validation errors, would_block during a frame, or invalid_argument for an unknown kind.
   */
  std::expected<void, TreeError> invalidate(Node<Widget> node, Invalidation kind);
  /** @brief Clear pending flags before processing the frame.
   * @return Previously pending invalidation, cleared to none; wrong_thread, closed, or would_block during a frame.
   */
  std::expected<Invalidation, TreeError> consume_frame_request();
  /** @brief Guarded extension call.
   * @param node Live widget to measure; hidden ancestry returns an empty size without invoking its hook.
   * @param available Cell constraints normalized before calling the extension.
   * @return Normalized desired size; tree validation errors, would_block for nested extension calls,
   * callback_failed for thrown exceptions, or the hook's error.
   */
  std::expected<CellSize, TreeError> measure(Node<Widget> node, CellSize available);
  /** @brief Arrange local geometry against the arranged parent and root viewport; invalidate on failure.
   * The layout owner calls parent before children. Root uses viewport; descendants inherit clip.
   * @param node Live widget whose geometry is replaced.
   * @param local Parent-relative rectangle; for the root it is already in surface coordinates.
   * @param viewport Root clipping rectangle; descendants inherit their parent's clip instead.
   * @return Success (including hidden nodes), tree validation errors, would_block for a nested hook,
   * invalid_geometry for an unarranged parent/translation overflow, callback_failed, or the hook's error.
   */
  std::expected<void, TreeError> arrange(Node<Widget> node, CellRect local, CellRect viewport);
  /** @brief Requires an active frame borrow.
   * @param node Live, arranged widget to paint; hidden ancestry skips the hook.
   * @param context Sink borrowed for this hook and the enclosing frame only.
   * @return Success, tree validation errors, would_block outside a frame or inside another hook,
   * invalid_geometry, callback_failed, or the hook's error.
   */
  std::expected<void, TreeError> paint(Node<Widget> node, PaintContext &context);
  /** @brief Keep widget/text storage alive through traversal AND rendering of borrowed commands.
   * Run layout/paint and render_full_frame before returning. Tree/domain edit is rejected;
   * inspect, measure, arrange and paint remain available. Callback must return void.
   * @tparam Callback Callable without arguments returning void.
   * @param callback Synchronous traversal and rendering operation; frame borrows must not escape.
   * @return Success, wrong_thread, closed, would_block for nesting/active borrows/reclamation,
   * or callback_failed for an exception; previously performed user effects are not rolled back.
   */
  template <class Callback> std::expected<void, TreeError> frame(Callback &&callback) {
    static_assert(std::is_void_v<std::invoke_result_t<Callback>>);
    auto allowed = begin_frame();

    if (!allowed) {
      return allowed;
    }

    Borrow guard(*this, true);

    try {
      std::invoke(std::forward<Callback>(callback));

      return {};
    } catch (...) {
      report({}, TreeError::callback_failed);

      return std::unexpected(TreeError::callback_failed);
    }
  }

private:
  struct State;
  struct Borrow {
    explicit Borrow(WidgetTree &tree, bool frame = false);
    ~Borrow();
    WidgetTree &tree;
    bool frame;
  };
  explicit WidgetTree(std::unique_ptr<State> state);
  std::expected<Widget *, TreeError> access(NodeId id, bool mutation) const;
  std::expected<void, TreeError> can_insert(NodeId parent) const;
  std::expected<NodeId, TreeError> attach(NodeId parent, std::unique_ptr<Widget> widget);
  std::expected<void, TreeError> begin_frame() const;
  const UiScope &scope(NodeId id) const;
  Scheduler &scheduler() const;
  static TreeError from_scheduler(SchedulerError error);
  void report(NodeId id, TreeError error) noexcept;
  void reclaim() noexcept;
  std::unique_ptr<State> state_;
};
} // namespace portals
