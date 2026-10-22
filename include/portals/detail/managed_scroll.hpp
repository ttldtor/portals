// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

namespace portals {
class KeyHints;

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

    if constexpr (std::derived_from<T, Control> || std::same_as<T, LineEdit> || std::same_as<T, Password> ||
                  std::same_as<T, Border> || std::same_as<T, Gauge> || std::same_as<T, ScrollArea> ||
                  std::same_as<T, ScrollBar> || std::same_as<T, Grid>) {
      const char *type = "Control";

      if constexpr (std::same_as<T, Label>) {
        type = "Label";
      } else if constexpr (std::same_as<T, Button>) {
        type = "Button";
      } else if constexpr (std::same_as<T, CheckBox>) {
        type = "CheckBox";
      } else if constexpr (std::same_as<T, LineEdit>) {
        type = "LineEdit";
      } else if constexpr (std::same_as<T, Password>) {
        type = "Password";
      } else if constexpr (std::same_as<T, KeyHints>) {
        type = "KeyHints";
      } else if constexpr (std::same_as<T, Border>) {
        type = "Border";
      } else if constexpr (std::same_as<T, Gauge>) {
        type = "Gauge";
      } else if constexpr (std::same_as<T, ScrollArea>) {
        type = "ScrollArea";
      } else if constexpr (std::same_as<T, ScrollBar>) {
        type = "ScrollBar";
      } else if constexpr (std::same_as<T, Grid>) {
        type = "Grid";
      }

      entry->target = std::make_shared<StyleTarget>(StyleTarget{
        .type = type,
        .control = std::derived_from<T, Control> || std::same_as<T, LineEdit> || std::same_as<T, Password>});
      entry->stylePending = true;
      stylesDirty_ = true;
    }

    if constexpr (std::derived_from<T, Control>) {
      entry->sync = [this, node = *node](Node<Widget> focused) {
        return syncControl(app_.tree(), node, focused);
      };
    } else if constexpr (std::same_as<T, Border>) {
      entry->sync = [this, node = *node](Node<Widget> focused) {
        return syncBorder(app_.tree(), node, focused);
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
    } else if constexpr (std::same_as<T, Grid>) {
      auto configured = attachScroll(entry, Node<Widget>(*node), true);
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
