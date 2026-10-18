// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/widget.hpp>
#include <thread>

namespace portals::detail {
/// @cond INTERNAL
// Metadata alone may outlive the scene. It owns no widget, tree or Property.
struct ManagedOrigin {
  WidgetTree *tree{};
  Node<Widget> node;
  std::weak_ptr<const void> lifetime;
  const std::thread::id owner = std::this_thread::get_id();
  bool retired{};
  bool closed{};

  std::expected<void, TreeError> check() {
    if (owner != std::this_thread::get_id()) {
      return std::unexpected(TreeError::WRONG_THREAD);
    }

    if (lifetime.expired() || closed) {
      return std::unexpected(TreeError::CLOSED);
    }

    if (retired) {
      return std::unexpected(TreeError::STALE_NODE);
    }

    auto live = tree->withScope(node, [](const UiScope &) {
    });

    if (!live && (live.error() == TreeError::STALE_NODE || live.error() == TreeError::CLOSED)) {
      retired = true;
    }

    return live;
  }
};

/// @endcond
} // namespace portals::detail
