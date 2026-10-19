// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

namespace portals {
template <class T, class... Args>
  requires std::derived_from<T, Widget>
std::expected<Node<T>, ManagedError> ManagedScene::emplaceConfigured(Node<Widget> parent,
                                                                     std::optional<bool> scrollWriter, Args &&...args) {
  auto allowed = boundary(ManagedPhase::MOUNT, parent);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  if (parent != root_ && !find(parent)) {
    return std::unexpected(error(ManagedPhase::MOUNT, parent, TreeError::INVALID_ARGUMENT));
  }

  if (entries_.size() >= options_.maxNodes) {
    return std::unexpected(error(ManagedPhase::MOUNT, parent, TreeError::NODE_LIMIT));
  }

  auto node = app_.tree().emplaceChecked<T>(parent, std::forward<Args>(args)...);

  if (!node) {
    return std::unexpected(error(ManagedPhase::MOUNT, parent, node.error()));
  }

  try {
    auto entry = std::make_shared<Entry>();

    entry->node = *node;

    if constexpr (std::derived_from<T, Control> || std::same_as<T, LineEdit> || std::same_as<T, Border> ||
                  std::same_as<T, Gauge> || std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>) {
      const char *type = "Control";

      if constexpr (std::same_as<T, Border>) {
        type = "Border";
      } else if constexpr (std::same_as<T, Gauge>) {
        type = "Gauge";
      } else if constexpr (std::same_as<T, ScrollArea>) {
        type = "ScrollArea";
      } else if constexpr (std::same_as<T, ScrollBar>) {
        type = "ScrollBar";
      }

      entry->target = std::make_shared<StyleTarget>(StyleTarget{.type = type});
      entry->stylePending = true;
      stylesDirty_ = true;
    }

    if constexpr (std::derived_from<T, Control>) {
      entry->sync = [this, node = *node](Node<Widget> focused) {
        return syncControl(app_.tree(), node, focused);
      };
    } else if constexpr (std::same_as<T, Border>) {
      entry->sync = [this, node = *node](Node<Widget>) {
        return syncBorder(app_.tree(), node);
      };
    } else if constexpr (std::same_as<T, Gauge>) {
      entry->sync = [this, node = *node](Node<Widget>) {
        return syncGauge(app_.tree(), node);
      };
    }

    if constexpr (std::same_as<T, ScrollArea> || std::same_as<T, ScrollBar>) {
      auto configured = attachScroll(entry, *node, scrollWriter.value_or(true));
      if (!configured) {
        return rollback(*node, configured.error());
      }
    }

    entries_.push_back(entry);
    publishScroll(entry);

    return *node;
  } catch (const std::bad_alloc &) {
    pruneScroll();
    return rollback(*node, TreeError::NO_MEMORY);
  } catch (...) {
    pruneScroll();
    return rollback(*node, TreeError::CALLBACK_FAILED);
  }
}

} // namespace portals
