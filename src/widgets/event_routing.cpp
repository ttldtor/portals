// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "action_state.hpp"

#include <optional>

namespace portals {
namespace {
std::optional<Action> key_action(InputKey key) {
  switch (key) {
  case InputKey::enter:
    return Action::activate;
  case InputKey::escape:
    return Action::cancel;
  case InputKey::left:
    return Action::move_left;
  case InputKey::right:
    return Action::move_right;
  case InputKey::up:
    return Action::move_up;
  case InputKey::down:
    return Action::move_down;
  case InputKey::home:
    return Action::move_home;
  case InputKey::end:
    return Action::move_end;
  case InputKey::backspace:
    return Action::erase_backward;
  case InputKey::delete_key:
    return Action::erase_forward;
  default:
    return std::nullopt;
  }
}

struct DeliveryGuard {
  bool &active;

  explicit DeliveryGuard(bool &value) : active(value) {
    active = true;
  }

  ~DeliveryGuard() {
    active = false;
  }
};
} // namespace

ActionBinding::ActionBinding(std::weak_ptr<detail::ActionCancellation> registration)
    : registration_(std::move(registration)) {
}

ActionBinding::~ActionBinding() {
  disconnect();
}

ActionBinding::ActionBinding(ActionBinding &&other) noexcept : registration_(std::move(other.registration_)) {
}

ActionBinding &ActionBinding::operator=(ActionBinding &&other) noexcept {
  if (this != &other) {
    disconnect();
    registration_ = std::move(other.registration_);
  }

  return *this;
}

void ActionBinding::disconnect() noexcept {
  if (auto entry = registration_.lock()) {
    entry->connected.store(false);
  }

  registration_.reset();
}

std::expected<void, TreeError> EventRouter::can_bind(Node<Widget> node) {
  auto allowed = check();

  if (!allowed) {
    return allowed;
  }

  if (state_->dispatching) {
    return std::unexpected(TreeError::would_block);
  }

  auto live = tree().input_eligible(node);

  if (!live) {
    return std::unexpected(live.error());
  }

  auto collected = collect();

  if (!collected) {
    return collected;
  }

  // Collecting captures can mutate the tree through their destructors.
  live = tree().input_eligible(node);

  if (!live) {
    return std::unexpected(live.error());
  }

  if (registration(node)) {
    return std::unexpected(TreeError::invalid_argument);
  }

  if (state_->registrations.size() >= state_->options.max_registrations) {
    return std::unexpected(TreeError::node_limit);
  }

  return {};
}

std::expected<ActionBinding, TreeError>
EventRouter::add(Node<Widget> node, std::function<std::expected<EventReply, TreeError>(const RoutedEvent &)> callback,
                 bool focusable) {
  auto entry = std::make_shared<detail::ActionRegistration>();
  entry->cancellation = std::make_shared<detail::ActionCancellation>();
  entry->node = node;
  entry->focusable = focusable;
  entry->callback = std::move(callback);

  // Callback construction/move may reenter, remove the target, or fill the registry.
  auto allowed = can_bind(node);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  state_->registrations.push_back(entry);

  return ActionBinding(entry->cancellation);
}

std::expected<DispatchResult, TreeError> EventRouter::route(Node<Widget> target, const RoutedEvent &event) {
  DispatchResult result;

  if (target.id().session == 0) {
    return result;
  }

  auto eligible = tree().input_eligible(target);

  if (!eligible) {
    return std::unexpected(eligible.error());
  }

  if (!*eligible) {
    return result;
  }

  result.target = target.id();
  result.status = DispatchStatus::unhandled;

  std::vector<Node<Widget>> path(0);
  auto current = target;

  while (current.id().session != 0) {
    path.push_back(current);

    auto snapshot = tree().inspect(current);

    if (!snapshot) {
      return std::unexpected(snapshot.error());
    }

    current = snapshot->parent;
  }

  // Snapshot the ancestry, but validate lifetime and eligibility before every admission.
  for (auto node : path) {
    auto live = tree().input_eligible(node);

    if (!live) {
      if (live.error() == TreeError::stale_node || live.error() == TreeError::closed) {
        continue;
      }

      return std::unexpected(live.error());
    }

    if (!*live) {
      continue;
    }

    auto entry = registration(node);

    if (!entry) {
      continue;
    }

    auto reply = entry->callback(event);

    if (!reply) {
      return std::unexpected(reply.error());
    }

    if (*reply == EventReply::handled) {
      result.handler = node.id();
      result.status = DispatchStatus::handled;

      break;
    }
  }

  return result;
}

std::expected<DispatchResult, TreeError> EventRouter::invoke(Node<Widget> target, ActionEvent action) {
  auto allowed = collect();

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  if (state_->dispatching) {
    return std::unexpected(TreeError::would_block);
  }

  DeliveryGuard guard(state_->dispatching);

  try {
    return route(target, RoutedEvent(action));
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::no_memory);
  }
}

std::expected<DispatchResult, TreeError> EventRouter::dispatch(const InputEvent &event) {
  auto allowed = collect();

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  if (state_->dispatching) {
    return std::unexpected(TreeError::would_block);
  }

  DeliveryGuard guard(state_->dispatching);

  try {
    if (const auto *key = std::get_if<KeyEvent>(&event)) {
      if (key->key == InputKey::tab && key->action != KeyAction::release && !key->modifiers.alt &&
          !key->modifiers.control && !key->modifiers.super) {
        auto next = traverse(key->modifiers.shift);

        if (!next) {
          return std::unexpected(next.error());
        }

        return DispatchResult{
          next->id(), {}, next->id().session == 0 ? DispatchStatus::no_target : DispatchStatus::handled};
      }

      auto target = focused();

      if (!target) {
        return std::unexpected(target.error());
      }

      auto action = key_action(key->key);

      if (action && key->action != KeyAction::release) {
        return route(*target, ActionEvent{*action, key->modifiers});
      }

      return route(*target, *key);
    }

    if (const auto *mouse = std::get_if<MouseInputEvent>(&event)) {
      auto target = hit_test({mouse->column, mouse->row});

      if (!target) {
        return std::unexpected(target.error());
      }

      if (mouse->button == MouseButton::left && mouse->action == MouseAction::press) {
        auto candidate = *target;

        while (candidate.id().session != 0) {
          auto entry = registration(candidate);

          if (entry && entry->focusable) {
            auto changed = change_focus(candidate);

            if (!changed) {
              return std::unexpected(changed.error());
            }

            break;
          }

          auto snapshot = tree().inspect(candidate);

          if (!snapshot) {
            return std::unexpected(snapshot.error());
          }

          candidate = snapshot->parent;
        }
      }

      return route(*target, *mouse);
    }

    if (const auto *text = std::get_if<TextInputEvent>(&event)) {
      auto target = focused();

      if (!target) {
        return std::unexpected(target.error());
      }

      return route(*target, *text);
    }

    if (const auto *paste = std::get_if<PasteInputEvent>(&event)) {
      auto target = focused();

      if (!target) {
        return std::unexpected(target.error());
      }

      return route(*target, *paste);
    }

    return DispatchResult{};
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::no_memory);
  }
}
} // namespace portals
