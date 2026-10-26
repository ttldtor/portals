// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_action.hpp>

namespace portals::ui::detail {
struct ActionCallback;

struct ActionRecord {
  std::string key = std::string(0, '\0');
  std::shared_ptr<const ActionMetadata> metadata;
  std::shared_ptr<ActionCallback> callback;
  bool retired{};
  bool invoking{};
};

struct ActionPresenter {
  Node<Button> node;
  std::shared_ptr<ActionRecord> record;
  std::weak_ptr<const ActionMetadata> rendered;
};

struct MenuPresenter {
  Node<Menu> node;
  std::size_t entries{};
};

struct ActionStore {
  explicit ActionStore(ActionSetOptions options) : options(options) {
  }

  const std::thread::id owner = std::this_thread::get_id();
  ActionSetOptions options;
  std::vector<std::shared_ptr<ActionRecord>> records = std::vector<std::shared_ptr<ActionRecord>>(0);
  std::vector<ActionPresenter> presenters = std::vector<ActionPresenter>(0);
  std::vector<MenuPresenter> menus = std::vector<MenuPresenter>(0);
  ManagedScene *scene{};
  std::weak_ptr<const void> lifetime;
  bool attached{};
  bool closed{};
  bool changing{};
  mutable bool menusDirty{true};
};

struct ActionRegistry {
  std::vector<std::shared_ptr<ActionStore>> sets = std::vector<std::shared_ptr<ActionStore>>(0);
  std::vector<Node<Menu>> menus = std::vector<Node<Menu>>(0);
};

} // namespace portals::ui::detail
