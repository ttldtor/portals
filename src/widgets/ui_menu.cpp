// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "ui_action_state.hpp"

#include <portals/ui_menu.hpp>

#include <algorithm>
#include <limits>

namespace portals::ui::detail {
struct MenuRegistration {
  struct StoreEntries {
    std::shared_ptr<ActionStore> store;
    std::size_t count{};
    bool cold{};
  };

  std::vector<StoreEntries> stores = std::vector<StoreEntries>(0);
  std::vector<ActionRef> references = std::vector<ActionRef>(0);
  std::vector<std::shared_ptr<ActionRecord>> records = std::vector<std::shared_ptr<ActionRecord>>(0);
  std::shared_ptr<ActionRegistry> previous;
  std::shared_ptr<ActionRegistry> incoming;
  std::weak_ptr<const void> lifetime;
  Node<Menu> node;
  std::uint64_t keyRevision{};
  bool published{};
};

namespace {
std::size_t menuPresenterCount(WidgetTree &tree, const ActionStore &store) {
  std::size_t used{};

  for (const auto &presenter : store.presenters) {
    if (tree.read(presenter.node, [](const Button &) {
        })) {
      ++used;
    }
  }

  for (const auto &presenter : store.menus) {
    if (tree.read(presenter.node, [](const Menu &) {
        })) {
      used += presenter.entries;
    }
  }

  return used;
}
} // namespace

std::expected<void, ManagedError> ActionAccess::stageMenu(ManagedScene &scene, MenuRegistration &registration) {
  const auto node = registration.node;
  auto &tree = scene.app_.tree();
  std::expected<void, TreeError> copied;
  auto read = tree.read(node, [&](const Menu &menu) {
    try {
      registration.references = menu.actions_;
    } catch (const std::bad_alloc &) {
      copied = std::unexpected(TreeError::NO_MEMORY);
    }
  });

  if (!read || !copied) {
    return std::unexpected(ManagedScene::error(ManagedPhase::BIND, node, !read ? read.error() : copied.error()));
  }

  registration.stores.reserve(registration.references.size());
  registration.records.reserve(registration.references.size());

  for (const auto &reference : registration.references) {
    auto record = reference.lock();

    if (!record && !reference.failure_ &&
        (record.error() == TreeError::CLOSED || record.error() == TreeError::STALE_NODE)) {
      registration.records.push_back(nullptr);
      continue;
    }

    if (!record) {
      return std::unexpected(ManagedScene::error(ManagedPhase::BIND, node, record.error()));
    }

    auto store = reference.store_.lock();

    if (store->attached && store->scene != &scene) {
      return std::unexpected(ManagedScene::error(ManagedPhase::BIND, node, TreeError::WRONG_SESSION));
    }

    registration.records.push_back(*record);

    auto found = std::ranges::find_if(registration.stores, [&](const MenuRegistration::StoreEntries &entry) {
      return entry.store == store;
    });

    if (found == registration.stores.end()) {
      registration.stores.push_back({store, 1, !store->attached});
    } else {
      ++found->count;
    }
  }

  auto incoming = std::make_shared<ActionRegistry>();

  if (registration.previous) {
    incoming->sets = registration.previous->sets;
    incoming->menus = registration.previous->menus;
  }

  std::erase_if(incoming->menus, [&](Node<Menu> previousNode) {
    return !tree.read(previousNode, [](const Menu &) {
    });
  });
  incoming->menus.push_back(node);

  for (const auto &entry : registration.stores) {
    auto &store = *entry.store;
    const auto used = menuPresenterCount(tree, store);

    if (entry.count > store.options.maxPresenters || used > store.options.maxPresenters - entry.count) {
      return std::unexpected(ManagedScene::error(ManagedPhase::BIND, node, TreeError::NODE_LIMIT));
    }

    store.menus.reserve(store.menus.size() + 1);

    if (entry.cold) {
      incoming->sets.push_back(entry.store);
    }
  }

  registration.incoming = std::move(incoming);

  return {};
}

void ActionAccess::rollbackMenu(ManagedScene &scene, const MenuRegistration &registration) {
  const auto node = registration.node;

  if (!registration.published || registration.lifetime.expired() || scene.closed_) {
    return;
  }

  const auto current = scene.actionSets_;

  if (!current) {
    return;
  }

  std::erase(current->menus, node);

  for (const auto &entry : registration.stores) {
    auto &store = *entry.store;

    if (!store.attached || store.scene != &scene || store.closed) {
      continue;
    }

    std::erase_if(store.presenters, [&](const ActionPresenter &presenter) {
      return !scene.app_.tree().read(presenter.node, [](const Button &) {
      });
    });
    std::erase_if(store.menus, [&](const MenuPresenter &presenter) {
      return presenter.node == node || !scene.app_.tree().read(presenter.node, [](const Menu &) {
      });
    });

    // Independently published presenters and later explicit key effects retain scene ownership.
    if (!entry.cold || !store.presenters.empty() || !store.menus.empty() ||
        scene.keyRevision_ != registration.keyRevision) {
      continue;
    }

    store.attached = false;
    store.scene = nullptr;
    store.lifetime.reset();
    std::erase(current->sets, entry.store);
  }
}

std::expected<void, ManagedError> ActionAccess::validateMenu(ManagedScene &scene,
                                                             const MenuRegistration &registration) {
  const auto node = registration.node;

  if (registration.lifetime.expired()) {
    return std::unexpected(ManagedScene::error(ManagedPhase::BIND, node, TreeError::CLOSED));
  }

  auto admitted = scene.boundary(ManagedPhase::BIND, node);

  if (!admitted) {
    return admitted;
  }

  auto &tree = scene.app_.tree();

  for (std::size_t index = 0; index < registration.references.size(); ++index) {
    if (!registration.records[index]) {
      continue;
    }

    auto record = registration.references[index].lock();

    if (!record) {
      return std::unexpected(ManagedScene::error(ManagedPhase::BIND, node, record.error()));
    }

    auto store = registration.references[index].store_.lock();

    if (store->attached && store->scene != &scene) {
      return std::unexpected(ManagedScene::error(ManagedPhase::BIND, node, TreeError::WRONG_SESSION));
    }
  }

  if (scene.actionSets_ != registration.previous) {
    return std::unexpected(ManagedScene::error(ManagedPhase::BIND, node, TreeError::WOULD_BLOCK));
  }

  for (const auto &entry : registration.stores) {
    const auto &store = *entry.store;
    const auto used = menuPresenterCount(tree, store);

    if (entry.count > store.options.maxPresenters || used > store.options.maxPresenters - entry.count) {
      return std::unexpected(ManagedScene::error(ManagedPhase::BIND, node, TreeError::NODE_LIMIT));
    }

    if (store.menus.capacity() < store.menus.size() + 1) {
      return std::unexpected(ManagedScene::error(ManagedPhase::BIND, node, TreeError::WOULD_BLOCK));
    }
  }

  return {};
}

EventReply ActionAccess::routeMenu(ManagedScene &scene, Node<Menu> node, Menu &menu, const RoutedEvent &event) {
  NodeGeometry geometry;

  if (std::holds_alternative<MouseInputEvent>(event)) {
    auto observed = scene.geometry(node);

    if (!observed) {
      scene.remember(observed.error());

      return EventReply::IGNORED;
    }

    geometry = *observed;
  }

  auto result = menu.input(scene.app_.tree(), node, event, geometry);

  if (!result) {
    scene.remember(ManagedScene::error(ManagedPhase::INPUT, node, result.error()));

    return EventReply::HANDLED;
  }

  const auto reply = result->handled ? EventReply::HANDLED : EventReply::IGNORED;

  // Nothing borrowed from menu or scene is accessed after the callback enters.
  if (result->action) {
    auto outcome = result->action->invoke(result->invocation);
    (void)outcome;
  }

  return reply;
}

std::expected<void, ManagedError> ActionAccess::menu(ManagedScene &scene, Node<Menu> node, MountContext &context) {
  auto allowed = scene.boundary(ManagedPhase::BIND, node);

  if (!allowed) {
    return allowed;
  }

  const auto lifetime = std::weak_ptr(scene.lifetime_);
  const auto previous = scene.actionSets_;

  try {
    auto registration = std::make_shared<MenuRegistration>();

    if (lifetime.expired()) {
      return std::unexpected(ManagedScene::error(ManagedPhase::BIND, node, TreeError::CLOSED));
    }

    registration->lifetime = lifetime;
    registration->previous = previous;
    registration->node = node;

    auto staged = stageMenu(scene, *registration);

    if (!staged) {
      return staged;
    }

    context.rollback.push_back([registration, &scene] {
      rollbackMenu(scene, *registration);
    });

    auto checked = validateMenu(scene, *registration);

    if (!checked) {
      return checked;
    }

    auto bound = scene.app_.router().bind(node, [&scene, node](Menu &menu, const RoutedEvent &event) {
      return routeMenu(scene, node, menu, event);
    });

    if (!bound) {
      return std::unexpected(ManagedScene::error(ManagedPhase::BIND, node, bound.error()));
    }

    checked = validateMenu(scene, *registration);

    if (!checked) {
      return checked;
    }

    registration->keyRevision = scene.keyRevision_;
    scene.find(node)->binding = std::move(*bound);
    scene.actionSets_ = std::move(registration->incoming);

    for (const auto &entry : registration->stores) {
      auto &store = *entry.store;

      if (entry.cold) {
        store.scene = &scene;
        store.lifetime = registration->lifetime;
        store.attached = true;
      }

      std::erase_if(store.menus, [&](const MenuPresenter &presenter) {
        return !scene.app_.tree().read(presenter.node, [](const Menu &) {
        });
      });
      store.menus.push_back({node, entry.count});
    }

    registration->published = true;

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(ManagedScene::error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
  }
}

Description<Menu> menuRecipe(std::span<const ActionRef> actions, MenuOptions options, ControlStyle style,
                             SizePolicy policy) {
  struct ColdMenu {
    std::vector<ActionRef> actions = std::vector<ActionRef>(0);
    std::vector<std::shared_ptr<ActionStore>> declarations = std::vector<std::shared_ptr<ActionStore>>(0);
    std::size_t count{};
  };

  auto cold = std::make_shared<ColdMenu>();

  cold->count = actions.size();

  if (options.maxEntries > 0 &&
      options.maxEntries <= static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max()) &&
      actions.size() <= options.maxEntries) {
    cold->actions.assign(actions.begin(), actions.end());
    cold->declarations.reserve(actions.size());

    for (const auto &action : actions) {
      cold->declarations.push_back(ActionAccess::retain(action));
    }
  }

  return Access::cold<Menu>(
    [cold, options, style, policy](ManagedScene &scene, Node<Widget> parent,
                                   MountContext &context) -> std::expected<Node<Menu>, ManagedError> {
      if (options.maxEntries == 0 ||
          options.maxEntries > static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max())) {
        return std::unexpected(ManagedError{ManagedPhase::MOUNT, parent.id(), TreeError::INVALID_ARGUMENT, {}, {}});
      }

      if (cold->count > options.maxEntries) {
        return std::unexpected(ManagedError{ManagedPhase::MOUNT, parent.id(), TreeError::NODE_LIMIT, {}, {}});
      }

      auto empty = GraphemeText::from("", scene.widthPolicy());

      if (!empty) {
        return std::unexpected(ManagedError{ManagedPhase::MOUNT, parent.id(), TreeError::INVALID_ARGUMENT, {}, {}});
      }

      auto node = scene.emplace<Menu>(parent, std::move(*empty), cold->actions, options, style, policy);

      if (!node) {
        return node;
      }

      if (context.root == Node<Widget>{}) {
        context.root = *node;
      }

      auto attached = ActionAccess::menu(scene, *node, context);

      if (!attached) {
        return std::unexpected(attached.error());
      }

      return node;
    });
}

} // namespace portals::ui::detail

namespace portals {
std::expected<void, TreeError> ManagedScene::prepareActionMenus() {
  const auto registry = actionSets_;

  // Publish dirty intent before visiting visibility: hidden menus retain it until shown.
  for (const auto &store : registry->sets) {
    if (!store->menusDirty) {
      continue;
    }

    for (const auto &presenter : store->menus) {
      auto marked = app_.tree().edit(presenter.node, [](Menu &menu) {
        menu.invalidatePresentation();
      });

      if (!marked && marked.error() != TreeError::STALE_NODE && marked.error() != TreeError::CLOSED) {
        return marked;
      }
    }
  }

  for (const auto node : registry->menus) {
    auto visible = app_.tree().revealVisible(node);

    if (!visible) {
      if (visible.error() == TreeError::STALE_NODE || visible.error() == TreeError::CLOSED) {
        continue;
      }

      return std::unexpected(visible.error());
    }

    if (!*visible) {
      continue;
    }

    std::expected<void, TreeError> prepared;
    auto edited = app_.tree().edit(node, [&](Menu &menu) {
      prepared = menu.prepare(app_.tree(), node, widthPolicy());
    });

    if (!edited) {
      return edited;
    }

    if (!prepared) {
      remember(error(ManagedPhase::PREPARE, node, prepared.error()));

      return prepared;
    }
  }

  // A failed preparation leaves both source and per-menu dirty intent available for retry.
  for (const auto &store : registry->sets) {
    store->menusDirty = false;
  }

  return {};
}
} // namespace portals
