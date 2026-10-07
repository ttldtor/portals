// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/widget.hpp>

#include <algorithm>
#include <limits>
#include <optional>
#include <thread>

namespace portals {
/** @brief Immutable owning value/revision pair; revision zero is the initial value.
 * T must own its data transitively; mutable aliases must not change published snapshots.
 * @tparam T Value owning its data transitively; published reachable storage must remain immutable.
 */
template <class T> struct PropertyChange {
  T value;                  ///< Value captured at this revision, independent of the source lifetime.
  std::uint64_t revision{}; ///< Monotonic revision within this property, never wrapped.
};
/** @brief Finite observer endpoint storage, independent of Scheduler queue capacity. */
struct PropertyOptions {
  std::size_t max_observers{64}; ///< Maximum live or executing observers; must be positive.
};
/** @brief A successful set result; notification rejection does not undo the committed value.
 * Counts describe publication attempts, not callback execution. Accepted events can still
 * be cancelled by source destruction, Subscription disconnect, node removal or scheduler stop.
 * Equal set is a no-op and never retries notifications rejected by an earlier set.
 * @tparam T The property value type; its immutable snapshot owns all transitively reachable data.
 */
template <class T> struct PropertyUpdate {
  std::shared_ptr<const PropertyChange<T>> snapshot;  ///< Owning snapshot of this operation's revision.
  bool changed{};                                     ///< False means all publication counts are zero.
  std::size_t accepted{};                             ///< Events accepted by the existing Scheduler.
  std::size_t rejected{};                             ///< Live endpoints whose publication was rejected.
  std::size_t cancelled{};                            ///< Endpoints found closed during publication.
  std::optional<SchedulerError> notification_error{}; ///< First publication failure, including closed.
};
/** @brief UI-owned typed value with node-scoped observers and explicit partial delivery.
 * WidgetTree and its Scheduler are borrowed and must outlive this object. All operations
 * and destruction use the UI thread. Retained snapshots may outlive every UI owner.
 * Each changed set invalidates the source target synchronously, then publishes in observer
 * registration order on the data lane with reject overflow. No atomic multicast is promised.
 * Callbacks run only through Scheduler between frames; reentrant set appends notifications
 * after already accepted events. Scheduler budgets bound a self-updating callback chain.
 * T construction/move/comparison and callback construction/move must not reenter UI APIs.
 * T destruction may reenter after replacement: the old value is retained until all publication
 * attempts finish. Destructors must not throw or call members of an object being destroyed.
 * @tparam T Equality-comparable value constructible from T&&, owning its data transitively. Published
 * aliases must remain immutable; construction/move/comparison must not reenter UI APIs and destruction must not throw.
 */
template <std::equality_comparable T> class Property {
public:
  /** @brief Create a source bound to one checked invalidation target; no initial notification.
   * The target must belong to tree and be live outside a frame. Invalid kind/zero capacity
   * is invalid_argument; allocation failure is no_memory. T exceptions are callback_failed.
   * Argument construction before this call lies outside this error boundary.
   * @param tree Borrowed UI-owned tree and scheduler, both required to outlive the property.
   * @param target Live node in tree invalidated by each changed set.
   * @param initial Value moved into immutable revision zero.
   * @param kind Invalidation to request on change; none explicitly requests no work.
   * @param options Positive observer endpoint limit.
   * @return New property, tree validation/frame errors, invalid_argument, no_memory, or callback_failed.
   */
  static std::expected<std::unique_ptr<Property>, TreeError> create(WidgetTree &tree, Node<Widget> target, T initial,
                                                                    Invalidation kind, PropertyOptions options = {}) {
    auto allowed = tree.with_scope(target, [](const UiScope &) {
    });

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    if (!options.max_observers ||
        (kind != Invalidation::none && kind != Invalidation::paint && kind != Invalidation::layout)) {
      return std::unexpected(TreeError::invalid_argument);
    }

    try {
      auto state = std::make_shared<State>(tree, target, kind);

      if (options.max_observers > state->observers.max_size()) {
        return std::unexpected(TreeError::invalid_argument);
      }

      state->observers.reserve(options.max_observers);
      state->limit = options.max_observers;
      state->current = std::make_shared<const PropertyChange<T>>(std::move(initial), 0);

      return std::unique_ptr<Property>(new Property(std::move(state)));
    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::no_memory);
    } catch (...) {
      return std::unexpected(TreeError::callback_failed);
    }
  }

  /** @brief Logically close the source; executing callbacks retain their captures until return.
   * Existing Subscription tokens remain ordinary Scheduler tokens and may be disconnected
   * later. Their callbacks are suppressed after source closure. Wrong-thread destruction
   * terminates, matching the UI ownership contract of Subscription and WidgetTree.
   */
  ~Property() {
    if (state_->owner != std::this_thread::get_id()) {
      std::terminate();
    }

    state_->alive = false;
  }
  Property(const Property &) = delete;
  Property &operator=(const Property &) = delete;

  /** @brief Obtain an independent immutable owner, even after the target node was removed.
   * Access on another thread returns wrong_thread. Transfer requires T to support destruction
   * on the thread releasing the last owner; UI reentry from T destruction requires a live UI context.
   * @return Owning immutable value/revision pair, or wrong_thread without accessing the value.
   */
  std::expected<std::shared_ptr<const PropertyChange<T>>, TreeError> snapshot() const {
    if (state_->owner != std::this_thread::get_id()) {
      return std::unexpected(TreeError::wrong_thread);
    }

    return state_->current;
  }

  /** @brief Commit a changed value and invalidate its target before attempting notifications.
   * unexpected means no library-controlled value, revision, queue or invalidation change. A successful
   * changed receipt may contain rejected/cancelled notifications. Never retry set as a way
   * to deliver the missing events: equal set is a no-op. Consumers can read snapshot to resync.
   * Tree affinity/session/lifetime/frame failures propagate; revision overflow is
   * identity_exhausted. Allocation or T comparison/move failures occur before commit.
   * @param value Candidate value compared to the current snapshot and moved into storage only if changed.
   * @return Receipt for this operation's immutable revision and per-observer queue acceptance, including
   * partial failure; an unexpected TreeError means no library-controlled commit occurred.
   */
  std::expected<PropertyUpdate<T>, TreeError> set(T value) {
    auto state = state_;

    if (state->owner != std::this_thread::get_id()) {
      return std::unexpected(TreeError::wrong_thread);
    }

    auto allowed = state->tree.with_scope(state->target, [](const UiScope &) {
    });

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    // Retain the old value through every publication: its final destructor may reenter set.
    // The receipt below must continue to identify this operation even if that reentry commits again.
    auto previous = state->current;
    std::shared_ptr<const PropertyChange<T>> next;

    try {
      if (previous->value == value) {
        return PropertyUpdate<T>{previous};
      }

      if (previous->revision == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(TreeError::identity_exhausted);
      }

      next = std::make_shared<const PropertyChange<T>>(std::move(value), previous->revision + 1);
    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::no_memory);
    } catch (...) {
      return std::unexpected(TreeError::callback_failed);
    }

    auto invalidated = state->tree.invalidate(state->target, state->kind);

    if (!invalidated) {
      return std::unexpected(invalidated.error());
    }

    // Invalidation is the last fallible precommit step. Once current changes, bounded queue
    // rejection is part of the receipt, never a reason to roll back the visible value.
    state->current = next;
    state->prune();

    PropertyUpdate<T> result{next, true};

    for (auto &observer : state->observers) {
      auto published = observer.publisher.publish(next);

      if (published) {
        ++result.accepted;
      } else {
        if (!result.notification_error) {
          result.notification_error = published.error();
        }

        if (published.error() == SchedulerError::closed) {
          ++result.cancelled;
        } else {
          ++result.rejected;
        }
      }
    }

    state->prune();

    return result;
  }

  /** @brief Register an ordinary Subscription tied to a receiving node in the same tree.
   * The callback owns its captures and receives (W&, shared_ptr<const PropertyChange<T>>).
   * The entire callback borrows W through WidgetTree::connect, so removal during invocation
   * defers widget destruction. Source destruction suppresses later callbacks independently.
   * Registration does not send the initial value or invalidate the receiving node; the source
   * target is invalidated by set. Additional receiver changes request their own invalidation.
   * Expired endpoint records are reclaimed before the finite limit is checked (node_limit).
   * Reentrant destruction during registration rechecks capacity and source lifetime (closed).
   * Tree/Scheduler errors propagate; callback construction exceptions become callback_failed.
   * @tparam W Widget or a publicly derived extension type represented by receiver.
   * @tparam Callback Copy-constructible callable accepting W& and std::shared_ptr<const PropertyChange<T>>
   * as an lvalue; its return value is ignored.
   * @param receiver Live receiving node in the property's tree; removal cancels its scoped callback.
   * @param callback Owned callable; widget references must not escape, but the snapshot owner may be retained.
   * @return Subscription token with no initial delivery; tree/scheduler errors, node_limit, closed,
   * no_memory, or callback_failed on registration failure.
   */
  template <class W, class Callback>
  std::expected<Subscription, TreeError> observe(Node<W> receiver, Callback &&callback) {
    auto state = state_;

    if (state->owner != std::this_thread::get_id()) {
      return std::unexpected(TreeError::wrong_thread);
    }

    auto allowed = state->tree.with_scope(receiver, [](const UiScope &) {
    });

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    state->prune();

    if (state->observers.size() >= state->limit) {
      return std::unexpected(TreeError::node_limit);
    }

    try {
      auto lifetime = std::make_shared<Lifetime>();
      auto connection = state->tree.template connect<PropertyChange<T>>(
        receiver, [source = std::weak_ptr<State>(state), lifetime, function = std::forward<Callback>(callback)](
                    W &widget, std::shared_ptr<const PropertyChange<T>> event) mutable {
          auto locked = source.lock();

          if (locked && locked->alive) {
            std::invoke(function, widget, std::move(event));
          }
        });

      if (!connection) {
        return std::unexpected(connection.error());
      }

      // Moved-from callback destructors can reenter while connect materializes its captures.
      if (!state->alive) {
        return std::unexpected(TreeError::closed);
      }

      state->prune();

      if (state->observers.size() >= state->limit) {
        return std::unexpected(TreeError::node_limit);
      }

      state->observers.push_back(Observer{std::move(connection->publisher), lifetime});

      return std::move(connection->subscription);
    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::no_memory);
    } catch (...) {
      return std::unexpected(TreeError::callback_failed);
    }
  }

private:
  struct Lifetime {};
  struct Observer {
    Publisher<PropertyChange<T>> publisher;
    std::weak_ptr<Lifetime> lifetime;
  };
  struct State {
    State(WidgetTree &value, Node<Widget> node, Invalidation invalidation)
        : tree(value), target(node), kind(invalidation) {
    }

    void prune() {
      std::erase_if(observers, [](const Observer &observer) {
        return observer.lifetime.expired();
      });
    }

    WidgetTree &tree;
    Node<Widget> target;
    Invalidation kind;
    const std::thread::id owner{std::this_thread::get_id()};
    bool alive{true};
    std::size_t limit{};
    std::shared_ptr<const PropertyChange<T>> current;
    std::vector<Observer> observers = std::vector<Observer>(0);
  };
  explicit Property(std::shared_ptr<State> state) : state_(std::move(state)) {
  }

  std::shared_ptr<State> state_;
};
} // namespace portals
