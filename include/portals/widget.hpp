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
  virtual ~Widget() = default; ///< Destroy on UI after the last active tree borrow.
  virtual void mount();        ///< Called exactly once after attachment; exceptions are reported.
  virtual void unmount();      ///< Called once before destruction, children first; cleanup continues on exceptions.
  virtual std::expected<CellSize, TreeError> measure(CellSize available); ///< Default desired size is empty.
  virtual std::expected<void, TreeError> arrange(CellRect local);         ///< Default accepts the owning rectangle.
  virtual std::expected<void, TreeError> paint(PaintContext &context);    ///< Default emits no commands.
};
/** @brief Copyable typed observer; never owns or exposes an unchecked widget pointer. */
template <class T = Widget> class Node {
public:
  Node() = default; ///< Empty observer; checked access fails.
  template <class U>
    requires std::derived_from<U, T>
  Node(Node<U> other) : id_(other.id()) {
  } ///< Safe observing upcast; downcasts are unavailable.
  NodeId id() const noexcept {
    return id_;
  } ///< Copy the stable diagnostic identity.
  bool operator==(const Node &) const noexcept = default; ///< Compare identity.
private:
  friend class WidgetTree;
  explicit Node(NodeId id) : id_(id) {
  }

  NodeId id_{};
};
/** @brief Owning snapshot of a live node; handles remain checked after subsequent mutations. */
struct NodeSnapshot {
  Node<Widget> parent;                                               ///< Empty only for the root.
  std::vector<Node<Widget>> children = std::vector<Node<Widget>>(0); ///< Live children in insertion order.
  NodeGeometry geometry;                                             ///< Owning geometry snapshot.
  bool visible{true}; ///< Hidden subtrees do not participate in layout, paint or input.
  bool enabled{true}; ///< Disabled subtrees retain layout/paint but do not receive input.
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
  /** @brief Create a mounted plain root and bounded registry; no partial tree on failure. */
  static std::expected<std::unique_ptr<WidgetTree>, TreeError>
  create(Scheduler &scheduler, WidgetTreeOptions options = {}, ErrorSink sink = nullptr, void *context = nullptr);
  ~WidgetTree(); ///< Close and reclaim on UI, outside all callbacks.
  WidgetTree(const WidgetTree &) = delete;
  WidgetTree &operator=(const WidgetTree &) = delete;
  std::expected<Node<Widget>, TreeError> root() const; ///< Checked observing root; stale after root removal.
  std::expected<NodeSnapshot, TreeError> inspect(Node<Widget> node) const; ///< Copy live structure; may allocate.
  std::expected<void, TreeError> remove(Node<Widget> node); ///< Invalidate subtree/scopes now, reclaim at safe point.
  std::expected<void, TreeError> close();                   ///< Idempotent coordinated shutdown, including during edit.

  /** @brief Borrow a const extension object, including inside frame traversal.
   * The external layout owner can read its custom layout interface, then measure/arrange
   * children using inspect snapshots. No reference may escape; callback returns void.
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

  /** @brief Node-scoped one-shot timer, with the same checked borrow as connect. */
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

  std::expected<void, TreeError> set_visible(Node<Widget> node, bool visible); ///< No-op if equal; otherwise layout.
  std::expected<void, TreeError> set_enabled(Node<Widget> node, bool enabled); ///< No-op if equal; otherwise paint.
  std::expected<bool, TreeError>
  input_eligible(Node<Widget> node) const; ///< Live/visible/enabled ancestors; hit-test also needs valid geometry.
  std::expected<void, TreeError> invalidate(Node<Widget> node, Invalidation kind); ///< Coalesce one frame request.
  std::expected<Invalidation, TreeError> consume_frame_request(); ///< Clear pending flags before processing the frame.
  std::expected<CellSize, TreeError> measure(Node<Widget> node, CellSize available); ///< Guarded extension call.
  /** @brief Arrange local geometry against the arranged parent and root viewport; invalidate on failure.
   * The layout owner calls parent before children. Root uses viewport; descendants inherit clip.
   */
  std::expected<void, TreeError> arrange(Node<Widget> node, CellRect local, CellRect viewport);
  std::expected<void, TreeError> paint(Node<Widget> node, PaintContext &context); ///< Requires an active frame borrow.
  /** @brief Keep widget/text storage alive through traversal AND rendering of borrowed commands.
   * Run layout/paint and render_full_frame before returning. Tree/domain edit is rejected;
   * inspect, measure, arrange and paint remain available. Callback must return void.
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
