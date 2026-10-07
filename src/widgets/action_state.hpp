// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <atomic>
#include <portals/action.hpp>
#include <thread>

namespace portals::detail {
struct ActionCancellation {
  std::atomic<bool> connected{true};
};

struct ActionRegistration {
  Node<Widget> node;
  bool focusable{};
  std::shared_ptr<ActionCancellation> cancellation;
  std::function<std::expected<EventReply, TreeError>(const RoutedEvent &)> callback;
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
  Node<Widget> focus;
  bool dispatching{};
  bool collecting{};
  bool closing{};
};
} // namespace portals
