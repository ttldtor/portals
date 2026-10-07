// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "action_state.hpp"

#include <algorithm>

namespace portals {
EventRouter::EventRouter(std::unique_ptr<State> state) : state_(std::move(state)) {
}

std::expected<std::unique_ptr<EventRouter>, TreeError> EventRouter::create(WidgetTree &tree,
                                                                           EventRouterOptions options) {
  auto root = tree.root();

  if (!root) {
    return std::unexpected(root.error());
  }

  if (options.max_registrations == 0) {
    return std::unexpected(TreeError::invalid_argument);
  }

  try {
    auto state = std::make_unique<State>(tree, options);

    return std::unique_ptr<EventRouter>(new EventRouter(std::move(state)));
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::no_memory);
  }
}

EventRouter::~EventRouter() {
  if (state_->owner != std::this_thread::get_id() || state_->dispatching || state_->collecting || state_->closing) {
    std::terminate();
  }

  state_->closing = true;
  state_->registrations.clear();
}

WidgetTree &EventRouter::tree() {
  return state_->tree;
}

std::expected<void, TreeError> EventRouter::check() const {
  if (state_->owner != std::this_thread::get_id()) {
    return std::unexpected(TreeError::wrong_thread);
  }

  if (state_->closing) {
    return std::unexpected(TreeError::closed);
  }

  if (state_->collecting) {
    return std::unexpected(TreeError::would_block);
  }

  auto root = state_->tree.root();

  if (!root) {
    return std::unexpected(root.error());
  }

  return state_->tree.edit(*root, [](Widget &) {
  });
}

std::shared_ptr<detail::ActionRegistration> EventRouter::registration(Node<Widget> node) const {
  for (const auto &entry : state_->registrations) {
    if (entry->node == node && entry->cancellation->connected.load()) {
      return entry;
    }
  }

  return {};
}

std::expected<void, TreeError> EventRouter::collect() {
  auto allowed = check();

  if (!allowed) {
    return allowed;
  }

  for (auto &entry : state_->registrations) {
    auto live = tree().input_eligible(entry->node);

    if (!live && live.error() != TreeError::stale_node) {
      return std::unexpected(live.error());
    }

    if (!live) {
      entry->cancellation->connected.store(false);
    }
  }

  // Destructors of captures cannot reenter while registry storage is being changed.
  struct CollectionGuard {
    bool &active;

    explicit CollectionGuard(bool &value) : active(value) {
      active = true;
    }

    ~CollectionGuard() {
      active = false;
    }
  } guard(state_->collecting);

  // Active delivery keeps its own shared owner until the callback returns.
  std::erase_if(state_->registrations, [](const auto &entry) {
    return !entry->cancellation->connected.load();
  });
  std::erase_if(state_->history, [&](auto node) {
    return !registration(node);
  });

  return {};
}

std::expected<std::vector<Node<Widget>>, TreeError> EventRouter::focus_order() {
  std::vector<Node<Widget>> result(0);
  std::vector<Node<Widget>> pending(0);
  auto root = tree().root();

  if (!root) {
    return std::unexpected(root.error());
  }

  pending.push_back(*root);

  while (!pending.empty()) {
    const auto node = pending.back();
    auto snapshot = tree().inspect(node);

    pending.pop_back();

    if (!snapshot) {
      return std::unexpected(snapshot.error());
    }

    if (!snapshot->visible || !snapshot->enabled) {
      continue;
    }

    auto entry = registration(node);

    if (entry && entry->focusable) {
      result.push_back(node);
    }

    // Reverse-push onto the LIFO stack visits siblings in insertion order. Skipping an
    // ineligible ancestor prunes the whole subtree without consulting arranged geometry.
    pending.insert(pending.end(), snapshot->children.rbegin(), snapshot->children.rend());
  }

  // Copy inside the allocation boundary; MSVC debug vector moves may allocate in noexcept.
  return std::expected<std::vector<Node<Widget>>, TreeError>(std::in_place, result);
}

std::expected<void, TreeError> EventRouter::change_focus(Node<Widget> node) {
  if (node == state_->focus) {
    return {};
  }

  // Allocate history before publishing the new focus.
  if (node.id().session != 0) {
    auto found = std::find(state_->history.begin(), state_->history.end(), node);

    if (found != state_->history.end()) {
      state_->history.erase(found);
    }

    state_->history.push_back(node);
  }

  const auto previous = state_->focus;

  state_->focus = node;

  if (previous.id().session != 0) {
    auto status = tree().invalidate(previous, Invalidation::paint);

    if (!status && status.error() != TreeError::stale_node) {
      return status;
    }
  }

  if (node.id().session != 0) {
    return tree().invalidate(node, Invalidation::paint);
  }

  return {};
}

std::expected<Node<Widget>, TreeError> EventRouter::focused() {
  auto allowed = collect();

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  try {
    auto order = focus_order();

    if (!order) {
      return std::unexpected(order.error());
    }

    if (std::find(order->begin(), order->end(), state_->focus) != order->end()) {
      return state_->focus;
    }

    Node<Widget> next;

    // Recover from still-eligible focus history before choosing a never-focused preorder node.
    // Hiding a subtree need not erase its history, so restoring it can make that entry useful again.
    for (auto it = state_->history.rbegin(); it != state_->history.rend(); ++it) {
      if (std::find(order->begin(), order->end(), *it) != order->end()) {
        next = *it;

        break;
      }
    }

    if (next.id().session == 0 && !order->empty()) {
      next = order->front();
    }

    auto changed = change_focus(next);

    if (!changed) {
      return std::unexpected(changed.error());
    }

    return next;
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::no_memory);
  }
}

std::expected<void, TreeError> EventRouter::set_focus(Node<Widget> node) {
  auto allowed = collect();

  if (!allowed) {
    return allowed;
  }

  auto eligible = tree().input_eligible(node);

  if (!eligible) {
    return std::unexpected(eligible.error());
  }

  auto entry = registration(node);

  if (!*eligible || !entry || !entry->focusable) {
    return std::unexpected(TreeError::invalid_argument);
  }

  try {
    return change_focus(node);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::no_memory);
  }
}

std::expected<Node<Widget>, TreeError> EventRouter::traverse(bool reverse) {
  auto allowed = collect();

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  try {
    auto order = focus_order();

    if (!order) {
      return std::unexpected(order.error());
    }

    Node<Widget> next;

    if (!order->empty()) {
      auto position = std::find(order->begin(), order->end(), state_->focus);

      if (position == order->end()) {
        next = reverse ? order->back() : order->front();
      } else if (reverse) {
        next = position == order->begin() ? order->back() : *std::prev(position);
      } else {
        ++position;
        next = position == order->end() ? order->front() : *position;
      }
    }

    auto changed = change_focus(next);

    if (!changed) {
      return std::unexpected(changed.error());
    }

    return next;
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::no_memory);
  }
}

std::expected<Node<Widget>, TreeError> EventRouter::hit_test(CellPoint point) {
  auto allowed = check();

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  try {
    auto root = tree().root();
    std::vector<Node<Widget>> pending{*root};
    Node<Widget> hit;

    while (!pending.empty()) {
      const auto node = pending.back();
      auto snapshot = tree().inspect(node);

      pending.pop_back();

      if (!snapshot) {
        return std::unexpected(snapshot.error());
      }

      const auto &geometry = snapshot->geometry;

      if (!snapshot->visible || !snapshot->enabled || !geometry.valid || !geometry.surface.contains(point) ||
          !geometry.clip.contains(point)) {
        continue;
      }

      // Retaining the last preorder hit gives descendants precedence over their ancestors
      // and later sibling subtrees precedence over earlier ones, matching painting order.
      hit = node;
      pending.insert(pending.end(), snapshot->children.rbegin(), snapshot->children.rend());
    }

    return hit;
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::no_memory);
  }
}
} // namespace portals
