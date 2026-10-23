// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_action.hpp>

#include <algorithm>
#include <limits>

namespace portals::ui::detail {
struct ActionCallback {
  explicit ActionCallback(Action::Callback callback) : callback(std::move(callback)) {
  }

  ~ActionCallback() {
    // The final admitted pin can retire under an unrelated caller after invocation scopes have unwound.
    retireSceneCallback(&callback, [](void *storage) noexcept {
      *static_cast<Action::Callback *>(storage) = {};
    });
  }

  Action::Result operator()(const ActionInvocation &invocation) {
    return callback(invocation);
  }

  Action::Callback callback;
};

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

struct ActionStore {
  explicit ActionStore(ActionSetOptions options) : options(options) {
  }

  const std::thread::id owner = std::this_thread::get_id();
  ActionSetOptions options;
  std::vector<std::shared_ptr<ActionRecord>> records = std::vector<std::shared_ptr<ActionRecord>>(0);
  std::vector<ActionPresenter> presenters = std::vector<ActionPresenter>(0);
  ManagedScene *scene{};
  std::weak_ptr<const void> lifetime;
  bool attached{};
  bool closed{};
  bool changing{};
};

struct ActionRegistry {
  std::vector<std::shared_ptr<ActionStore>> sets = std::vector<std::shared_ptr<ActionStore>>(0);
};

std::expected<void, TreeError> ActionAccess::live(const ActionStore &store) {
  if (store.owner != std::this_thread::get_id()) {
    return std::unexpected(TreeError::WRONG_THREAD);
  }

  if (store.closed || (store.attached && (store.lifetime.expired() || !store.scene || store.scene->closed_))) {
    return std::unexpected(TreeError::CLOSED);
  }

  if (store.attached) {
    return store.scene->app_.tree().read(store.scene->root_, [](const Widget &) {
    });
  }

  return {};
}

std::shared_ptr<ActionStore> ActionAccess::retain(const ActionRef &reference) {
  if (reference.owner_ != std::thread::id{} && reference.owner_ != std::this_thread::get_id()) {
    return {};
  }

  return reference.store_.lock();
}

std::expected<void, TreeError> ActionAccess::dirty(const ActionStore &store) {
  auto allowed = live(store);
  if (!allowed) {
    return allowed;
  }

  if (!store.attached) {
    return {};
  }

  return store.scene->app_.tree().invalidate(store.scene->root_, Invalidation::LAYOUT);
}

std::expected<void, TreeError> ActionAccess::write(const ActionStore &store) {
  auto allowed = live(store);
  if (!allowed) {
    return allowed;
  }

  if (store.changing || (store.attached && store.scene->preparing_)) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  return {};
}

EditorCommandResult ActionAccess::finish(ManagedScene &, std::expected<void, TreeError> result) {
  return EditorCommandResult(result, {}, false);
}

EditorCommandResult ActionAccess::finish(ManagedScene &scene, std::expected<void, ManagedError> result) {
  return scene.admitActionResult(std::move(result));
}

EditorCommandResult ActionAccess::finish(ManagedScene &, EditorCommandResult result) {
  return result;
}

EditorCommandResult ActionAccess::report(const ActionRef &reference, std::expected<void, TreeError> result,
                                         std::source_location origin) {
  auto store = reference.store_.lock();

  if (!result && store && live(*store) && store->attached) {
    auto &scene = *store->scene;
    auto failure = ManagedScene::error(ManagedPhase::INPUT, scene.root_, result.error());

    failure.registrationOrigin = origin;

    return scene.admitActionResult(std::unexpected(std::move(failure)));
  }

  return EditorCommandResult(result, {}, false);
}

std::expected<void, ManagedError> ActionAccess::attach(ManagedScene &scene, const ActionRef &reference) {
  auto allowed = scene.boundary(ManagedPhase::BIND, scene.root_);
  if (!allowed) {
    return allowed;
  }

  auto record = reference.lock();
  if (!record) {
    return std::unexpected(ManagedScene::error(ManagedPhase::BIND, scene.root_, record.error()));
  }

  auto store = reference.store_.lock();
  if (store->attached) {
    return store->scene == &scene
             ? std::expected<void, ManagedError>{}
             : std::unexpected(ManagedScene::error(ManagedPhase::BIND, scene.root_, TreeError::WRONG_SESSION));
  }

  const auto lifetime = std::weak_ptr(scene.lifetime_);
  const auto previous = scene.actionSets_;

  try {
    auto incoming = std::make_shared<ActionRegistry>();

    if (previous) {
      incoming->sets = previous->sets;
    }

    incoming->sets.push_back(store);

    if (lifetime.expired()) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::CLOSED, {}, {}});
    }

    allowed = scene.boundary(ManagedPhase::BIND, scene.root_);
    if (!allowed) {
      return allowed;
    }

    auto current = reference.lock();
    if (!current) {
      return std::unexpected(ManagedScene::error(ManagedPhase::BIND, scene.root_, current.error()));
    }

    if (store->attached) {
      return store->scene == &scene
               ? std::expected<void, ManagedError>{}
               : std::unexpected(ManagedScene::error(ManagedPhase::BIND, scene.root_, TreeError::WRONG_SESSION));
    }

    if (scene.actionSets_ != previous) {
      return std::unexpected(ManagedScene::error(ManagedPhase::BIND, scene.root_, TreeError::WOULD_BLOCK));
    }

    scene.actionSets_ = std::move(incoming);
    store->scene = &scene;
    store->lifetime = scene.lifetime_;
    store->attached = true;

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::NO_MEMORY, {}, {}});
  }
}

std::expected<void, ManagedError> attachAction(ManagedScene &scene, const ActionRef &reference) {
  return ActionAccess::attach(scene, reference);
}

std::expected<void, ManagedError> attachActionButton(ManagedScene &scene, Node<Button> node,
                                                     const ActionRef &reference) {
  return ActionAccess::button(scene, node, reference);
}

std::expected<void, ManagedError> ActionAccess::button(ManagedScene &scene, Node<Button> node,
                                                       const ActionRef &reference) {
  auto attached = attach(scene, reference);
  if (!attached) {
    return attached;
  }

  auto store = reference.store_.lock();
  auto &tree = scene.app_.tree();

  std::erase_if(store->presenters, [&](const ActionPresenter &presenter) {
    return !tree.read(presenter.node, [](const Button &) {
    });
  });

  if (store->presenters.size() >= store->options.maxPresenters) {
    return std::unexpected(ManagedScene::error(ManagedPhase::BIND, node, TreeError::NODE_LIMIT));
  }

  try {
    // Reserve before publishing the existing router binding: allocation failure leaves no presenter/binding.
    store->presenters.reserve(store->presenters.size() + 1);

    auto allowed = live(*store);
    if (!allowed) {
      return std::unexpected(ManagedError{ManagedPhase::BIND, node.id(), allowed.error(), {}, {}});
    }

    auto bound = scene.onClick(node, [reference](const ClickEvent &event) {
      return reference(event);
    });
    if (!bound) {
      return bound;
    }

    store->presenters.push_back({node, reference.record_.lock(), {}});

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(ManagedScene::error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
  }
}

std::expected<void, TreeError> ActionAccess::prepare(ManagedScene &scene) {
  struct Preparing {
    ManagedScene &scene;
    bool previous;

    ~Preparing() {
      scene.preparing_ = previous;
    }
  } guard{scene, scene.preparing_};

  scene.preparing_ = true;

  auto &tree = scene.app_.tree();
  const auto registry = scene.actionSets_;

  if (!registry) {
    return {};
  }

  for (const auto &store : registry->sets) {
    for (auto &presenter : store->presenters) {
      const auto metadata = presenter.record->metadata;

      if (presenter.rendered.lock() == metadata && !presenter.record->retired) {
        continue;
      }

      auto liveNode = tree.read(presenter.node, [](const Button &) {
      });
      if (!liveNode) {
        if (liveNode.error() == TreeError::STALE_NODE || liveNode.error() == TreeError::CLOSED) {
          continue;
        }

        return liveNode;
      }

      auto projected = [&]() -> std::expected<void, TreeError> {
        try {
          if (presenter.record->retired) {
            return tree.setEnabled(presenter.node, false);
          }

          auto text = GraphemeText::from(metadata->caption, scene.widthPolicy());
          if (!text) {
            return std::unexpected(TreeError::INVALID_ARGUMENT);
          }

          auto caption = setControlText(tree, presenter.node, std::move(*text));
          if (!caption) {
            return caption;
          }

          auto enabled = tree.setEnabled(presenter.node, metadata->enabled);
          if (!enabled) {
            return enabled;
          }

          auto visible = tree.setVisible(presenter.node, metadata->visible);
          if (!visible) {
            return visible;
          }

          return tree.edit(presenter.node, [&](Button &button) {
            button.showChecked(metadata->checked);
          });
        } catch (const std::bad_alloc &) {
          return std::unexpected(TreeError::NO_MEMORY);
        } catch (...) {
          return std::unexpected(TreeError::CALLBACK_FAILED);
        }
      }();

      if (!projected) {
        scene.remember(ManagedScene::error(ManagedPhase::PREPARE, presenter.node, projected.error()));

        return projected;
      }

      presenter.rendered = metadata;
    }

    std::erase_if(store->presenters, [](const auto &presenter) {
      return presenter.record->retired;
    });
  }

  return {};
}

void ActionAccess::retire(ManagedScene &scene) noexcept {
  const auto registry = std::move(scene.actionSets_);

  if (!registry) {
    return;
  }

  for (const auto &store : registry->sets) {
    store->closed = true;
    store->scene = nullptr;

    for (const auto &record : store->records) {
      record->retired = true;
      record->callback.reset();
    }

    store->presenters.clear();
    store->records.clear();
  }
}
} // namespace portals::ui::detail

namespace portals::ui {
namespace {
struct Change {
  detail::ActionStore &store;
  std::expected<void, TreeError> admission;

  explicit Change(detail::ActionStore &store) : store(store), admission(detail::ActionAccess::write(store)) {
    if (admission) {
      store.changing = true;
    }
  }

  ~Change() {
    if (admission) {
      store.changing = false;
    }
  }
};

std::size_t metadataBytes(const ActionMetadata &value) {
  return value.caption.size() + value.shortcut.size() + value.help.size() + value.exclusiveGroup.size();
}

std::expected<void, TreeError> validate(const detail::ActionStore &store, std::string_view key,
                                        const ActionMetadata &metadata, const detail::ActionRecord *replaced) {
  const auto &options = store.options;
  if (!options.maxActions || !options.maxPresenters || !options.maxMetadataBytes || !options.maxKeyBytes ||
      key.empty() || key.size() > options.maxKeyBytes) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  if (key.size() > options.maxMetadataBytes) {
    return std::unexpected(TreeError::NODE_LIMIT);
  }

  std::size_t bytes = key.size();
  const auto charge = [&](std::size_t count) {
    if (count > options.maxMetadataBytes - std::min(bytes, options.maxMetadataBytes)) {
      return false;
    }

    bytes += count;

    return true;
  };

  for (const auto *text : {&metadata.caption, &metadata.shortcut, &metadata.help, &metadata.exclusiveGroup}) {
    if (!charge(text->size())) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }
  }

  for (const auto &record : store.records) {
    if (!record->retired && record.get() != replaced) {
      if (record->key == key) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }

      if (!charge(record->key.size()) || !charge(metadataBytes(*record->metadata))) {
        return std::unexpected(TreeError::NODE_LIMIT);
      }
    }
  }

  return {};
}

std::expected<void, TreeError> replace(detail::ActionStore &store, const std::shared_ptr<detail::ActionRecord> &record,
                                       const ActionMetadata &metadata,
                                       std::shared_ptr<detail::ActionCallback> callback = {}) {
  auto allowed = detail::ActionAccess::live(store);
  if (!allowed) {
    return allowed;
  }

  auto valid = validate(store, record->key, metadata, record.get());
  if (!valid) {
    return valid;
  }

  try {
    const auto policy = store.attached ? store.scene->widthPolicy() : WidthPolicy{};
    for (const auto *text : {&metadata.caption, &metadata.shortcut, &metadata.help}) {
      if (!GraphemeText::from(*text, policy)) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
    }

    auto incoming = std::make_shared<const ActionMetadata>(metadata);
    std::vector<std::pair<std::shared_ptr<detail::ActionRecord>, std::shared_ptr<const ActionMetadata>>> unchecked(0);

    allowed = detail::ActionAccess::live(store);
    if (!allowed) {
      return allowed;
    }

    if (metadata.checked && !metadata.exclusiveGroup.empty()) {
      unchecked.reserve(store.records.size());
      allowed = detail::ActionAccess::live(store);
      if (!allowed) {
        return allowed;
      }

      for (std::size_t index = 0; index < store.records.size(); ++index) {
        const auto other = store.records[index];

        if (other != record && !other->retired && other->metadata->checked &&
            other->metadata->exclusiveGroup == metadata.exclusiveGroup) {
          const auto previous = other->metadata;
          auto value = std::make_shared<ActionMetadata>(*previous);

          allowed = detail::ActionAccess::live(store);
          if (!allowed) {
            return allowed;
          }

          value->checked = false;
          unchecked.emplace_back(other, std::move(value));
        }
      }
    }

    auto invalidated = detail::ActionAccess::dirty(store);
    if (!invalidated) {
      return invalidated;
    }

    record->metadata = std::move(incoming);
    if (callback) {
      record->callback.swap(callback);
    }

    for (auto &[other, value] : unchecked) {
      other->metadata = std::move(value);
    }

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}
} // namespace

ActionSet::ActionSet(ActionSetOptions options) : store_(std::make_shared<detail::ActionStore>(options)) {
}

ActionSet::ActionSet(ActionSet &&other) noexcept {
  if (other.store_ && other.store_->owner != std::this_thread::get_id()) {
    std::terminate();
  }

  store_ = std::move(other.store_);
}

ActionSet &ActionSet::operator=(ActionSet &&other) noexcept {
  if ((store_ && store_->owner != std::this_thread::get_id()) ||
      (other.store_ && other.store_->owner != std::this_thread::get_id())) {
    std::terminate();
  }

  if (this != &other) {
    store_ = std::move(other.store_);
  }

  return *this;
}

ActionSet::~ActionSet() {
  if (store_ && store_->owner != std::this_thread::get_id()) {
    std::terminate();
  }
}

std::expected<ActionRef, TreeError> ActionSet::add(std::string_view key, Action action) {
  if (!store_) {
    return std::unexpected(TreeError::CLOSED);
  }

  auto allowed = detail::ActionAccess::live(*store_);
  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  Change change(*store_);
  if (!change.admission) {
    return std::unexpected(change.admission.error());
  }

  auto valid = validate(*store_, key, action.metadata, nullptr);
  if (!valid) {
    return std::unexpected(valid.error());
  }

  std::erase_if(store_->records, [](const auto &record) {
    return record->retired;
  });
  if (store_->records.size() >= store_->options.maxActions) {
    return std::unexpected(TreeError::NODE_LIMIT);
  }

  try {
    auto record = std::make_shared<detail::ActionRecord>();

    record->key = key;
    record->callback = std::make_shared<detail::ActionCallback>(std::move(action.callback));
    store_->records.reserve(store_->records.size() + 1);

    auto committed = replace(*store_, record, action.metadata);
    if (!committed) {
      return std::unexpected(committed.error());
    }

    store_->records.push_back(record);

    ActionRef reference;

    reference.owner_ = store_->owner;
    reference.store_ = store_;
    reference.record_ = record;

    return reference;
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

ActionRef ActionSet::action(std::string_view key, Action value) {
  try {
    auto result = add(key, std::move(value));
    if (result) {
      return *result;
    }

    return failed(result.error());
  } catch (const std::bad_alloc &) {
    return failed(TreeError::NO_MEMORY);
  } catch (...) {
    return failed(TreeError::CALLBACK_FAILED);
  }
}

ActionRef ActionSet::failed(TreeError failure) const {
  ActionRef reference;

  reference.owner_ = store_ ? store_->owner : std::this_thread::get_id();
  reference.store_ = store_;
  reference.failure_ = failure;

  return reference;
}

std::expected<std::shared_ptr<detail::ActionRecord>, TreeError> ActionRef::lock() const {
  if (owner_ != std::thread::id{} && owner_ != std::this_thread::get_id()) {
    return std::unexpected(TreeError::WRONG_THREAD);
  }

  if (failure_) {
    return std::unexpected(*failure_);
  }

  auto store = store_.lock();
  if (!store) {
    return std::unexpected(TreeError::CLOSED);
  }

  auto allowed = detail::ActionAccess::live(*store);
  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  auto record = record_.lock();
  if (!record || record->retired) {
    return std::unexpected(TreeError::STALE_NODE);
  }

  return record;
}

std::expected<std::shared_ptr<const ActionMetadata>, TreeError> ActionRef::snapshot() const {
  auto record = lock();

  return record ? std::expected<std::shared_ptr<const ActionMetadata>, TreeError>((*record)->metadata)
                : std::unexpected(record.error());
}

EditorCommandResult ActionRef::set(ActionMetadata metadata, std::source_location origin) const {
  auto record = lock();
  if (!record) {
    return detail::ActionAccess::report(*this, std::unexpected(record.error()), origin);
  }

  const auto store = store_.lock();
  Change change(*store);

  return detail::ActionAccess::report(*this, change.admission ? replace(*store, *record, metadata) : change.admission,
                                      origin);
}

EditorCommandResult ActionRef::rebind(Action action, std::source_location origin) const {
  auto record = lock();
  if (!record) {
    return detail::ActionAccess::report(*this, std::unexpected(record.error()), origin);
  }

  const auto store = store_.lock();
  Change change(*store);
  if (!change.admission) {
    return detail::ActionAccess::report(*this, change.admission, origin);
  }

  try {
    auto callback = std::make_shared<detail::ActionCallback>(std::move(action.callback));

    return detail::ActionAccess::report(*this, replace(*store, *record, action.metadata, std::move(callback)), origin);
  } catch (const std::bad_alloc &) {
    return detail::ActionAccess::report(*this, std::unexpected(TreeError::NO_MEMORY), origin);
  }
}

EditorCommandResult ActionRef::remove(std::source_location origin) const {
  auto record = lock();
  if (!record) {
    return detail::ActionAccess::report(*this, std::unexpected(record.error()), origin);
  }

  const auto store = store_.lock();
  Change change(*store);
  if (!change.admission) {
    return detail::ActionAccess::report(*this, change.admission, origin);
  }

  auto invalidated = detail::ActionAccess::dirty(*store);
  if (invalidated) {
    (*record)->retired = true;
    (*record)->callback.reset();
  }

  return detail::ActionAccess::report(*this, invalidated, origin);
}

EditorCommandResult ActionRef::invoke(ActionInvocation invocation, std::source_location origin) const {
  return detail::ActionAccess::invoke(*this, invocation, origin);
}

EditorCommandResult ActionRef::operator()(const KeyEvent &event) const {
  return invoke({ClickSource::KEYBOARD, event.modifiers});
}

EditorCommandResult ActionRef::operator()(const ClickEvent &event) const {
  ClickSource source = ClickSource::PROGRAMMATIC;

  if (const auto *action = std::get_if<ActionEvent>(&event.event)) {
    source = action->source;
  } else if (std::holds_alternative<KeyEvent>(event.event)) {
    source = ClickSource::KEYBOARD;
  } else if (std::holds_alternative<MouseInputEvent>(event.event)) {
    source = ClickSource::MOUSE;
  }

  return invoke({source, event.modifiers});
}
} // namespace portals::ui

namespace portals::ui::detail {
EditorCommandResult ActionAccess::invoke(const ActionRef &reference, ActionInvocation invocation,
                                         std::source_location origin) {
  auto record = reference.lock();
  if (!record) {
    return report(reference, std::unexpected(record.error()), origin);
  }

  auto store = reference.store_.lock();
  if (!store->attached) {
    return report(reference, std::unexpected(TreeError::CLOSED), origin);
  }

  auto &scene = *store->scene;
  auto admitted = scene.actionBoundary();
  if (!admitted) {
    return report(reference, admitted, origin);
  }

  if (!(*record)->metadata->enabled || !(*record)->metadata->visible) {
    return EditorCommandResult({}, {}, false);
  }

  if ((*record)->invoking) {
    return report(reference, std::unexpected(TreeError::WOULD_BLOCK), origin);
  }

  const auto callback = (*record)->callback;

  struct Invocation {
    ActionRecord &record;

    ~Invocation() {
      record.invoking = false;
    }
  } guard{**record};

  (*record)->invoking = true;

  try {
    SetupFailurePause setup(scene.facadeContext());
    CurrentSceneScope current(scene.facadeContext());
    ManagedScene::ActionAdmission admission(scene);
    auto outcome = (*callback)(invocation);

    if (store->lifetime.expired()) {
      return EditorCommandResult(std::unexpected(TreeError::CLOSED), {}, false);
    }

    auto result = std::visit(
      [&](auto &&value) {
        return finish(scene, std::move(value));
      },
      std::move(outcome));

    if (store->lifetime.expired()) {
      return result;
    }

    if (!result && !scene.commandReported(result, store->lifetime)) {
      return report(reference, result.result(), origin);
    }

    return result;
  } catch (const std::bad_alloc &) {
    return report(reference, std::unexpected(TreeError::NO_MEMORY), origin);
  } catch (...) {
    return report(reference, std::unexpected(TreeError::CALLBACK_FAILED), origin);
  }
}
} // namespace portals::ui::detail
