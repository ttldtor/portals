// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "key_binding_state.hpp"
#include "managed_error_helpers.hpp"
#include "message_box_state.hpp"
#include "scroll_preparation_state.hpp"
#include <portals/detail/setup_failure.hpp>
#include <portals/managed.hpp>
#include <portals/ui_action.hpp>

#include <algorithm>
#include <limits>

namespace portals::ui::detail {
SetupFailurePause::SetupFailurePause(const std::weak_ptr<SceneAnchor> &scene) noexcept
    : previous_(SetupFailureScope::current_) {
  if (!previous_ || !previous_->sink_ || scene.expired() || previous_->owner_.owner_before(scene) ||
      scene.owner_before(previous_->owner_)) {
    SetupFailureScope::current_ = nullptr;
  }
}

SetupFailurePause::~SetupFailurePause() {
  SetupFailureScope::current_ = previous_;
}

void retireSceneCallback(void *storage, void (*reset)(void *) noexcept) noexcept {
  SetupFailureScope mask({}, nullptr, nullptr);

  reset(storage);
}

} // namespace portals::ui::detail

namespace portals {
namespace {
template <class T> std::size_t maxElementCount() noexcept {
  const auto differenceLimit = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(T);
  const auto allocatorLimit = std::allocator_traits<std::allocator<T>>::max_size(std::allocator<T>{});

  return std::min(differenceLimit, allocatorLimit);
}

} // namespace

ManagedScene::ManagedScene(Application &app, ManagedOptions options)
    : app_(app), owner_(std::this_thread::get_id()), options_(options), lifetime_(std::make_shared<const bool>(true)),
      asyncActivities_(0), styles_(app.tree(), options.maxNodes), entries_(0), properties_(0), models_(0),
      errors_(options.diagnostics.maxErrors) {
}

ManagedError ManagedScene::error(ManagedPhase phase, Node<Widget> node, TreeError cause) {
  return {phase, node.id(), cause, {}, {}};
}

ManagedError detail::passwordError(ManagedPhase phase, NodeId node, PasswordFailure failure) {
  const auto security = [](const SecureStringError &cause) {
    return cause.code == SecureStringErrorCode::NO_MEMORY ? TreeError::NO_MEMORY : TreeError::CALLBACK_FAILED;
  };
  const auto cause = std::visit(
    [&](const auto &value) -> TreeError {
      using T = std::remove_cvref_t<decltype(value)>;

      if constexpr (std::same_as<T, TreeError>) {
        return value;
      } else if constexpr (std::same_as<T, TextError> || std::same_as<T, PasswordInputError>) {
        return TreeError::INVALID_ARGUMENT;
      } else if constexpr (std::same_as<T, SecureStringFailure>) {
        return security(value.primary);
      } else {
        if (value.ownership) {
          return *value.ownership;
        }
        if (value.access) {
          if (value.access->primary) {
            return security(*value.access->primary);
          }
          if constexpr (std::same_as<T, PasswordValueAccessFailure<SecureStringFailure>>) {
            if (value.access->callbackError) {
              return security(value.access->callbackError->primary);
            }
          } else if constexpr (std::same_as<T, PasswordValueAccessFailure<TextError>>) {
            if (value.access->callbackError) {
              return TreeError::INVALID_ARGUMENT;
            }
          }
        }

        return TreeError::CALLBACK_FAILED;
      }
    },
    failure);
  ManagedError result{phase, node, cause, {}, {}};

  result.password.emplace(std::move(failure));

  return result;
}

std::expected<std::unique_ptr<ManagedScene>, ManagedError> ManagedScene::create(Application &app,
                                                                                ManagedOptions options) {
  auto allowed = app.tree().withScope(app.root(), [](const UiScope &) {
  });

  if (!allowed) {
    return std::unexpected(error(ManagedPhase::MOUNT, app.root(), allowed.error()));
  }

  const auto diagnostics = options.diagnostics;

  for (const auto support :
       {options.keyCapabilities.press, options.keyCapabilities.repeat, options.keyCapabilities.release}) {
    if (support != KeySupport::UNKNOWN && support != KeySupport::SUPPORTED && support != KeySupport::UNSUPPORTED) {
      return std::unexpected(error(ManagedPhase::MOUNT, app.root(), TreeError::INVALID_ARGUMENT));
    }
  }

  if (!options.maxNodes || !options.maxStates || !options.maxModels || !options.maxKeyHandlers ||
      !options.maxClickHandlers || !options.maxClickHandlersPerNode || !options.keySequenceTimeout ||
      !options.maxEditingHandlers || !options.maxEditingHandlersPerNode || !options.maxChangeHandlers ||
      !options.maxChangeHandlersPerNode || !options.maxMessageRequests || !options.maxKeyLabelBytes ||
      !options.maxKeyDescriptionBytes || !options.maxKeyProjectionBytes || !options.maxLayoutHandlers ||
      !options.maxLayoutPublicationRetries || !options.maxObservers || !options.maxObserversPerNode ||
      !options.maxObserverRecoveryPasses || !diagnostics.maxErrors || !diagnostics.maxContexts ||
      !diagnostics.maxContextBytes || diagnostics.maxErrors > maxElementCount<ManagedDiagnostic>() ||
      diagnostics.maxContexts > maxElementCount<ManagedDiagnosticContext>() ||
      diagnostics.maxContextBytes > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
    return std::unexpected(error(ManagedPhase::MOUNT, app.root(), TreeError::INVALID_ARGUMENT));
  }

  try {
    auto scene = std::unique_ptr<ManagedScene>(new ManagedScene(app, options));
    auto root = app.tree().emplaceChecked<Widget>(app.root());

    if (!root) {
      scene->closed_ = true;

      return std::unexpected(error(ManagedPhase::MOUNT, app.root(), root.error()));
    }

    scene->root_ = *root;

    return scene;
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::MOUNT, app.root(), TreeError::NO_MEMORY));
  }
}

ManagedScene::~ManagedScene() {
  ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
  if (owner_ != std::this_thread::get_id() || preparing_ || reporting_ || recoveringObservers_ || observerCallbacks_) {
    std::terminate();
  }

  if (sessionOperating_) {
    std::terminate();
  }

  if (sessionActive_) {
    app_.requestStop();
    (void)step();
  }

  if (dataStateOwner_) {
    dataStateOwner_->scene = nullptr;
  }
  for (const auto &record : dataStates_) {
    record->retired = true;
    record->timer = {};
  }
  stopSpinners();
  // Router/scheduler may retain disconnected captures until their next collection.
  // Weak state handles end with this owner even while those internal borrows drain.
  lifetime_.reset();

  auto result = close();

  if (!result && bridge(result.error()) == TreeError::WOULD_BLOCK) {
    std::terminate();
  }

  // State payload deleters belong to this retired owner, never an unrelated entered facade.
  properties_.clear();
}

std::expected<void, ManagedError> ManagedScene::boundary(ManagedPhase phase, Node<Widget> node) const {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(error(phase, node, TreeError::WRONG_THREAD));
  }

  if (closed_) {
    return std::unexpected(error(phase, node, TreeError::CLOSED));
  }

  if (preparing_ || reporting_ || styling_ || pruningObservers_) {
    return std::unexpected(error(phase, node, TreeError::WOULD_BLOCK));
  }

  if ((phase == ManagedPhase::PREPARE || phase == ManagedPhase::RUN) &&
      (recoveringObservers_ || observerCallbacks_ || observerMountPending() || servicingActivities_)) {
    return std::unexpected(error(phase, node, TreeError::WOULD_BLOCK));
  }

  auto allowed = app_.tree().withScope(node, [](const UiScope &) {
  });

  if (!allowed) {
    return std::unexpected(error(phase, node, allowed.error()));
  }

  return {};
}

std::shared_ptr<ManagedScene::Entry> ManagedScene::find(Node<Widget> node) const {
  const auto found = std::ranges::find(entries_, node, &Entry::node);

  return found == entries_.end() ? nullptr : *found;
}

std::expected<std::shared_ptr<ManagedScene::Entry>, ManagedError> ManagedScene::bindingEntry(Node<Widget> node) {
  auto allowed = boundary(ManagedPhase::BIND, node);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  auto entry = find(node);

  if (!entry || !std::holds_alternative<std::monostate>(entry->binding)) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
  }

  return entry;
}

std::unexpected<ManagedError> ManagedScene::rollback(Node<Widget> node, TreeError cause) {
  auto result = error(ManagedPhase::MOUNT, node, cause);
  auto removed = app_.tree().remove(node);

  if (!removed && !retired(removed.error())) {
    result.cleanup = removed.error();
  }

  return std::unexpected(result);
}

std::expected<void, ManagedError> ManagedScene::focusScope(Node<Widget> node) {
  ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
  auto allowed = boundary(ManagedPhase::BIND, node);

  if (!allowed) {
    return allowed;
  }

  auto entry = find(node);

  if (!entry) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
  }

  auto bound = app_.router().focusScope(node);

  if (!bound) {
    return std::unexpected(error(ManagedPhase::BIND, node, bound.error()));
  }

  entry->focusScope = std::move(*bound);

  return {};
}

std::expected<void, ManagedError> ManagedScene::focusDefault(Node<Widget> node) {
  ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
  auto allowed = boundary(ManagedPhase::BIND, node);

  if (!allowed) {
    return allowed;
  }

  if (!find(node)) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
  }

  auto result = app_.router().focusDefault(node);

  return result ? std::expected<void, ManagedError>{}
                : std::unexpected(error(ManagedPhase::BIND, node, result.error()));
}

struct ManagedScene::FocusReport {
  static thread_local FocusReport *active;
  Application *application;
  std::optional<ManagedError> reported;
  FocusReport *previous;

  explicit FocusReport(ManagedScene &owner) : application(&owner.app_), previous(active) {
    active = this;
  }

  ~FocusReport() {
    // Only call-local values survive callbacks; no scene is touched during restoration.
    active = previous;
  }

  static std::optional<ManagedError> *destination(Application &application) noexcept {
    for (auto *entry = active; entry; entry = entry->previous) {
      if (entry->application == &application) {
        return &entry->reported;
      }
    }

    return nullptr;
  }

  bool matches(TreeError cause) const noexcept {
    const auto *reportedCause = reported ? std::get_if<TreeError>(&reported->cause) : nullptr;

    return reportedCause && *reportedCause == cause;
  }

  ManagedError failure(Node<Widget> node, TreeError cause) const {
    if (matches(cause)) {
      return *reported;
    }

    return ManagedScene::error(ManagedPhase::INPUT, node, cause);
  }
};

thread_local ManagedScene::FocusReport *ManagedScene::FocusReport::active{};

std::expected<Node<Widget>, TreeError> ManagedScene::repairDataFocus() {
  FocusReport report(*this);
  auto focused = app_.router().focused();
  if (!focused && !report.matches(focused.error())) {
    remember(error(ManagedPhase::PREPARE, root_, focused.error()));
  }
  return focused;
}

std::expected<void, ManagedError> ManagedScene::openDialog(Node<Widget> node) {
  auto allowed = boundary(ManagedPhase::INPUT, node);
  if (!allowed) {
    return allowed;
  }
  return dialogCommand(node, true, DialogDismissReason::PROGRAMMATIC);
}

std::expected<void, ManagedError> ManagedScene::dismissDialog(Node<Widget> node) {
  auto allowed = boundary(ManagedPhase::INPUT, node);
  if (!allowed) {
    return allowed;
  }
  return dialogCommand(node, false, DialogDismissReason::PROGRAMMATIC);
}

std::expected<void, ManagedError> ManagedScene::dialogCommand(Node<Widget> node, bool opening,
                                                              DialogDismissReason reason) {
  auto message = findMessage(node);
  if (opening && message && (!message->current || !message->current->starting || message->current->done)) {
    return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::INVALID_ARGUMENT));
  }

  FocusReport report(*this);
  const auto before = message ? message->dialog->activationRevision : 0;
  auto result = changeDialog(node, opening, reason);
  std::expected<void, ManagedError> full =
    result ? std::expected<void, ManagedError>{} : std::unexpected(report.failure(node, result.error()));

  completeMessage(message, full, opening, message && message->dialog->activationRevision != before);

  return full;
}

std::expected<void, ManagedError> ManagedScene::focus(Node<Widget> node) {
  ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
  auto allowed = boundary(ManagedPhase::INPUT, node);

  if (!allowed) {
    return allowed;
  }

  if (!find(node)) {
    return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::INVALID_ARGUMENT));
  }

  FocusReport report(*this);

  if (app_.router().scope(node)) {
    auto result = app_.router().enterScope(node);

    return result ? std::expected<void, ManagedError>{} : std::unexpected(report.failure(node, result.error()));
  }

  auto result = app_.router().setFocus(node);

  return result ? std::expected<void, ManagedError>{} : std::unexpected(report.failure(node, result.error()));
}

std::expected<void, ManagedError> ManagedScene::enterFocusScope(Node<Widget> node) {
  ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
  auto allowed = boundary(ManagedPhase::INPUT, node);

  if (!allowed) {
    return allowed;
  }

  if (!find(node)) {
    return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::INVALID_ARGUMENT));
  }

  FocusReport report(*this);

  auto result = app_.router().enterFocusScope(node);

  return result ? std::expected<void, ManagedError>{} : std::unexpected(report.failure(node, result.error()));
}

std::expected<void, ManagedError> ManagedScene::leaveFocusScope(Node<Widget> node) {
  ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
  auto allowed = boundary(ManagedPhase::INPUT, node);

  if (!allowed) {
    return allowed;
  }

  if (!find(node)) {
    return std::unexpected(error(ManagedPhase::INPUT, node, TreeError::INVALID_ARGUMENT));
  }

  FocusReport report(*this);

  auto result = app_.router().leaveFocusScope(node);

  return result ? std::expected<void, ManagedError>{} : std::unexpected(report.failure(node, result.error()));
}

std::expected<void, ManagedError> ManagedScene::remove(Node<Widget> node) {
  ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
  auto allowed = boundary(ManagedPhase::CLEANUP, root_);

  if (!allowed) {
    return allowed;
  }

  if (node == root_) {
    return std::unexpected(error(ManagedPhase::CLEANUP, node, TreeError::INVALID_ARGUMENT));
  }

  if (!find(node)) {
    auto live = app_.tree().withScope(node, [](const UiScope &) {
    });

    if (!live && retired(live.error())) {
      return {};
    }

    return std::unexpected(error(ManagedPhase::CLEANUP, node, live ? TreeError::INVALID_ARGUMENT : live.error()));
  }

  struct Removing {
    bool &flag;

    ~Removing() {
      flag = false;
    }
  } guard{preparing_};

  preparing_ = true;

  auto removed = app_.tree().remove(node);

  if (!removed && !retired(removed.error())) {
    return std::unexpected(error(ManagedPhase::CLEANUP, node, removed.error()));
  }

  // Logical retirement is immediate even when the router still borrows an admitted callback.
  // Do not call style hooks during rollback: pruning on its next transaction is allocation-safe.
  retireThemeSelector();
  std::erase_if(entries_, [&](const auto &entry) {
    auto live = app_.tree().withScope(entry->node, [](const UiScope &) {
    });

    if (!live && retired(live.error())) {
      retireClicks(*entry);
      retireEditing(*entry);
      retireChanges(*entry);
      retireLayouts(*entry);

      return true;
    }

    return false;
  });
  pruneLiveText();
  pruneObservers();
  pruneScroll();
  pruneDataStates();
  retireDialogs();
  retireNotifications();
  stylesDirty_ = true;

  return {};
}

std::expected<void, ManagedError> ManagedScene::onClick(Node<Button> node,
                                                        std::function<std::expected<void, TreeError>()> action) {
  ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
  decltype(action) incoming;

  incoming.swap(action);

  try {
    return onButtonClick(node, actionAdapter<ClickEvent>(std::move(incoming)), false);
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::CALLBACK_FAILED));
  }
}

std::expected<void, ManagedError>
ManagedScene::onButtonClick(Node<Button> node, std::function<EditorCommandResult(const ClickEvent &)> action,
                            bool allowModified) {
  auto entry = bindingEntry(node);

  if (!entry) {
    return std::unexpected(entry.error());
  }

  if (!action) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
  }

  try {
    auto bound = bindButtonClick(
      app_.router(), node,
      [this, node, lifetime = std::weak_ptr(lifetime_), action = std::move(action)](const ClickEvent &event) {
        const auto report = [this, node, &lifetime](TreeError cause) {
          if (!lifetime.expired()) {
            remember(error(ManagedPhase::INPUT, node, cause));
          }
        };

        try {
          const auto command = action(event);

          if (!command && !commandReported(command, lifetime)) {
            report(command.result_.error());
          }
        } catch (const std::bad_alloc &) {
          report(TreeError::NO_MEMORY);
        } catch (...) {
          report(TreeError::CALLBACK_FAILED);
        }
      },
      allowModified);

    if (!bound) {
      return std::unexpected(error(ManagedPhase::BIND, node, bound.error()));
    }

    (*entry)->binding.emplace<ActionBinding>(std::move(*bound));

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::CALLBACK_FAILED));
  }
}

void ManagedScene::retireClicks(Entry &entry) noexcept {
  if (!entry.clicks || entry.clicks->retired) {
    return;
  }

  entry.clicks->retired = true;
  clickHandlers_ -= entry.clicks->records.size();

  // Revoke the registry and release its budget before capture destructors can reenter.
  // Retiring callers already guard reentry. clear() avoids MSVC Debug's allocating noexcept vector move.
  entry.clicks->records.clear();
}

void ManagedScene::retireEditing(Entry &entry) noexcept {
  if (!entry.editing || entry.editing->retired) {
    return;
  }

  entry.editing->retired = true;
  editingHandlers_ -= entry.editing->records.size();
  entry.editing->records.clear();
}

void ManagedScene::retireChanges(Entry &entry) noexcept {
  if (!entry.changes || entry.changes->retired) {
    return;
  }

  entry.changes->retired = true;
  changeHandlers_ -= entry.changes->records.size();
  entry.changes->records.clear();
}

std::expected<bool, ManagedError> ManagedScene::changeModel(Node<CheckBox> node) {
  auto allowed = boundary(ManagedPhase::BIND, node);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  auto entry = find(node);

  if (!entry || !entry->changes || entry->changes->retired) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
  }

  return entry->changes->boolean;
}

std::expected<void, ManagedError> ManagedScene::registerChange(Node<CheckBox> node, ChangeRecord record) {
  auto model = changeModel(node);

  if (!model) {
    return std::unexpected(model.error());
  }

  auto chain = find(node)->changes;
  const bool callable = std::visit(
    [](const auto &action) {
      return static_cast<bool>(action);
    },
    record.action);

  if (record.action.index() != (*model ? 0U : 1U) || !callable) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
  }

  if (changeHandlers_ >= options_.maxChangeHandlers || chain->records.size() >= options_.maxChangeHandlersPerNode) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NODE_LIMIT));
  }

  const auto lifetime = std::weak_ptr<const void>(lifetime_);
  const auto perNodeLimit = options_.maxChangeHandlersPerNode;

  try {
    auto admitted = std::make_shared<ChangeRecord>(std::move(record));

    if (lifetime.expired()) {
      return std::unexpected(error(ManagedPhase::BIND, node, TreeError::CLOSED));
    }

    // Allocate local capacity before final admission: allocator reentry may mutate or destroy the scene.
    // The count constructor keeps debug iterator-proxy allocation inside this throwing boundary.
    std::vector<std::shared_ptr<ChangeRecord>> grown(0);

    if (lifetime.expired()) {
      return std::unexpected(error(ManagedPhase::BIND, node, TreeError::CLOSED));
    }

    if (chain->records.size() == chain->records.capacity()) {
      const auto capacity = chain->records.capacity();
      const auto nextCapacity = capacity ? capacity + std::min(capacity, perNodeLimit - capacity) : 1;

      grown.reserve(nextCapacity);

      if (lifetime.expired()) {
        return std::unexpected(error(ManagedPhase::BIND, node, TreeError::CLOSED));
      }
    }

    model = changeModel(node);

    if (!model) {
      return std::unexpected(model.error());
    }

    chain = find(node)->changes;

    if (admitted->action.index() != (*model ? 0U : 1U)) {
      return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
    }

    if (changeHandlers_ >= options_.maxChangeHandlers || chain->records.size() >= options_.maxChangeHandlersPerNode) {
      return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NODE_LIMIT));
    }

    // Publication copies only owning pointers and cannot allocate or release the last capture.
    if (chain->records.size() == chain->records.capacity()) {
      if (chain->records.size() >= grown.capacity()) {
        return std::unexpected(error(ManagedPhase::BIND, node, TreeError::WOULD_BLOCK));
      }

      for (const auto &existing : chain->records) {
        grown.push_back(existing);
      }

      chain->records.swap(grown);
    }

    chain->records.push_back(std::move(admitted));
    ++changeHandlers_;

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
  }
}

template <class T>
void ManagedScene::dispatchChange(Node<CheckBox> node, const std::shared_ptr<ChangeChain> &chain,
                                  const std::weak_ptr<const void> &lifetime, const PropertyUpdate<T> &receipt,
                                  std::size_t count) {
  auto origin = std::source_location::current();

  try {
    for (std::size_t index = 0; index < count; ++index) {
      if (lifetime.expired() || chain->retired) {
        return;
      }

      if (closed_ || !app_.tree().inspect(node)) {
        return;
      }

      // Pin the record: appending can reallocate, while removal clears the registry.
      const auto record = chain->records[index];
      const auto &action = std::get<std::function<EditorCommandResult(const PropertyUpdate<T> &)>>(record->action);

      origin = record->origin;

      const auto result = action(receipt);

      if (!result) {
        if (!lifetime.expired() && !commandReported(result, lifetime)) {
          remember(error(ManagedPhase::INPUT, node, result.result_.error()), origin);
        }

        return;
      }
    }
  } catch (const std::bad_alloc &) {
    if (!lifetime.expired()) {
      remember(error(ManagedPhase::INPUT, node, TreeError::NO_MEMORY), origin);
    }
  } catch (...) {
    if (!lifetime.expired()) {
      remember(error(ManagedPhase::INPUT, node, TreeError::CALLBACK_FAILED), origin);
    }
  }
}

template <class T>
std::function<void(std::expected<PropertyUpdate<T>, TreeError>)> ManagedScene::checkboxCallback(
  Node<CheckBox> node, std::shared_ptr<ChangeChain> chain,
  std::function<std::expected<void, TreeError>(std::expected<PropertyUpdate<T>, TreeError>)> changed) {
  return [this, node, chain = std::move(chain), lifetime = std::weak_ptr<const void>(lifetime_),
          changed = ui::detail::currentSceneCallback(facadeContext(), std::move(changed))](
           std::expected<PropertyUpdate<T>, TreeError> receipt) {
    // Both survive legacy reentry. The exact original snapshot is copied in O(1), without allocation.
    static_assert(std::is_nothrow_copy_constructible_v<PropertyUpdate<T>>);

    const auto delivery = chain;
    const auto live = lifetime;
    const auto count = delivery->records.size();
    const auto original = receipt;

    if (live.expired()) {
      return;
    }

    try {
      if (!receipt) {
        remember(error(ManagedPhase::INPUT, node, receipt.error()));
      }

      if (live.expired()) {
        return;
      }

      if (changed) {
        auto result = changed(std::move(receipt));

        if (live.expired()) {
          return;
        }

        if (!result) {
          remember(error(ManagedPhase::INPUT, node, result.error()));

          return;
        }
      }

      if (!live.expired() && !delivery->retired && original && original->changed) {
        dispatchChange(node, delivery, live, *original, count);
      }
    } catch (const std::bad_alloc &) {
      if (!live.expired()) {
        remember(error(ManagedPhase::INPUT, node, TreeError::NO_MEMORY));
      }
    } catch (...) {
      if (!live.expired()) {
        remember(error(ManagedPhase::INPUT, node, TreeError::CALLBACK_FAILED));
      }
    }
  };
}

std::expected<void, ManagedError> ManagedScene::registerEditing(Node<LineEdit> node, EditorEventRecord record) {
  auto allowed = boundary(ManagedPhase::BIND, node);

  if (!allowed) {
    return allowed;
  }

  auto entry = find(node);

  if (!entry || !entry->editing || entry->editing->retired ||
      !std::visit(
        [](const auto &action) {
          return static_cast<bool>(action);
        },
        record.action)) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
  }

  if (editingHandlers_ >= options_.maxEditingHandlers ||
      entry->editing->records.size() >= options_.maxEditingHandlersPerNode) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NODE_LIMIT));
  }

  try {
    entry->editing->records.push_back(std::make_shared<EditorEventRecord>(std::move(record)));
    ++editingHandlers_;

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
  }
}

template <class Event>
void ManagedScene::dispatchEditing(Node<LineEdit> node, const std::shared_ptr<EditorEventChain> &chain,
                                   const std::weak_ptr<const void> &lifetime, const Event &event) {
  if (lifetime.expired() || chain->retired || closed_) {
    return;
  }

  const auto count = chain->records.size();
  auto origin = std::source_location::current();

  try {
    for (std::size_t index = 0; index < count; ++index) {
      if (lifetime.expired() || chain->retired) {
        return;
      }

      if (closed_ || !app_.tree().inspect(node)) {
        return;
      }

      // Appending may reallocate and removal may clear the registry during this call.
      const auto record = chain->records[index];
      const auto *action = std::get_if<std::function<EditorCommandResult(const Event &)>>(&record->action);

      if (!action) {
        continue;
      }

      origin = record->origin;
      const auto result = (*action)(event);

      if (!result) {
        if (!lifetime.expired() && !commandReported(result, lifetime)) {
          remember(error(ManagedPhase::INPUT, node, result.result_.error()), origin);
        }

        return;
      }
    }
  } catch (const std::bad_alloc &) {
    if (!lifetime.expired()) {
      remember(error(ManagedPhase::INPUT, node, TreeError::NO_MEMORY), origin);
    }
  } catch (...) {
    if (!lifetime.expired()) {
      remember(error(ManagedPhase::INPUT, node, TreeError::CALLBACK_FAILED), origin);
    }
  }
}

namespace {
bool sameGeometry(const NodeGeometry &left, const NodeGeometry &right) noexcept {
  return left.local == right.local && left.surface == right.surface && left.clip == right.clip;
}

ManagedError layoutSchedulerFailure(Node<Widget> node, SchedulerError cause) {
  return {ManagedPhase::LAYOUT, node.id(), ApplicationError{ApplicationPhase::DISPATCH, cause, {}}, {}, {}};
}
} // namespace

std::expected<NodeGeometry, ManagedError> ManagedScene::geometry(Node<Widget> node) const {
  auto allowed = boundary(ManagedPhase::LAYOUT, node);
  if (!allowed) {
    return std::unexpected(allowed.error());
  }
  if (!find(node)) {
    return std::unexpected(error(ManagedPhase::LAYOUT, node, TreeError::INVALID_ARGUMENT));
  }
  auto revision = app_.tree().geometryRevision();
  if (!revision) {
    return std::unexpected(error(ManagedPhase::LAYOUT, node, revision.error()));
  }
  if (*revision != layoutRevision_) {
    return std::unexpected(error(ManagedPhase::LAYOUT, node, TreeError::INVALID_GEOMETRY));
  }
  auto snapshot = app_.tree().observeGeometry(node);
  if (!snapshot) {
    return std::unexpected(error(ManagedPhase::LAYOUT, node, snapshot.error()));
  }
  if (!snapshot->geometry.valid) {
    return std::unexpected(error(ManagedPhase::LAYOUT, node, TreeError::INVALID_GEOMETRY));
  }
  return snapshot->geometry;
}

std::expected<void, ManagedError>
ManagedScene::registerLayout(Node<Widget> node, std::function<EditorCommandResult(const NodeGeometry &)> action,
                             std::source_location origin) {
  auto entry = find(node);
  if (!entry || !action) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
  }
  if (layouts_.size() >= options_.maxLayoutHandlers) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NODE_LIMIT));
  }

  auto record = std::make_shared<LayoutRecord>();
  record->node = node;
  record->action.swap(action);
  record->origin = origin;
  std::expected<SchedulerConnection<LayoutValue>, SchedulerError> connection = std::unexpected(SchedulerError::CLOSED);
  auto scoped = app_.tree().withScope(node, [&](const UiScope &scope) {
    connection = app_.scheduler().connect<LayoutValue>(
      scope,
      [this, weak = std::weak_ptr(record),
       lifetime = std::weak_ptr(lifetime_)](std::shared_ptr<const LayoutValue> value) {
        // The executor owns only weak scene/registration observations; no widget borrow spans user code.
        if (!lifetime.expired()) {
          if (auto live = weak.lock()) {
            deliverLayout(live, std::move(value));
          }
        }
      },
      SchedulerLane::DATA, SchedulerOverflow::REPLACE_LATEST);
  });
  if (!scoped) {
    return std::unexpected(error(ManagedPhase::BIND, node, scoped.error()));
  }
  if (!connection) {
    return std::unexpected(layoutSchedulerFailure(node, connection.error()));
  }
  record->connection = std::move(*connection);
  auto revision = app_.tree().geometryRevision();
  if (!revision) {
    return std::unexpected(error(ManagedPhase::BIND, node, revision.error()));
  }
  if (*revision == layoutRevision_) {
    auto snapshot = app_.tree().observeGeometry(node);
    if (!snapshot) {
      return std::unexpected(error(ManagedPhase::BIND, node, snapshot.error()));
    }
    record->visibilityRevision = snapshot->visibilityRevision;
    if (snapshot->geometry.valid) {
      record->pending = std::make_shared<const LayoutValue>(snapshot->geometry, layoutRevision_);
    }
  }
  layouts_.push_back(record);
  // Runtime registrations are first admitted after dispatch, never in the dispatch that registered them.
  return {};
}

void ManagedScene::retireLayouts(Entry &entry) noexcept {
  // Publish retirement and budgets before releasing captures that can reenter user code.
  std::erase_if(layouts_, [&](const auto &record) {
    if (record->node != entry.node) {
      return false;
    }
    record->retired = true;
    record->pending.reset();
    (void)record->connection.subscription.disconnect();
    return true;
  });
}

std::expected<void, TreeError> ManagedScene::publishLayout(const std::shared_ptr<LayoutRecord> &record) {
  if (record->retired || !record->pending || record->queued) {
    return {};
  }
  auto published = record->connection.publisher.publish(record->pending);
  if (published) {
    record->queued = true;
    record->rejected = 0;
    return {};
  }
  auto failure = layoutSchedulerFailure(record->node, published.error());
  if (published.error() == SchedulerError::WOULD_BLOCK) {
    ++record->rejected;
    if (record->rejected < options_.maxLayoutPublicationRetries) {
      return {};
    }
    record->pending.reset();
    record->rejected = 0;
    (void)reportError(std::move(failure), {}, record->origin);
    return {};
  }
  remember(std::move(failure), record->origin);
  return std::unexpected(published.error() == SchedulerError::NO_MEMORY ? TreeError::NO_MEMORY : TreeError::CLOSED);
}

void ManagedScene::deliverLayout(const std::shared_ptr<LayoutRecord> &record,
                                 std::shared_ptr<const LayoutValue> value) {
  if (closed_ || record->retired || record->pending != value) {
    return;
  }
  record->queued = false;
  record->pending.reset();
  auto current = geometry(record->node);
  if (!current || value->revision != layoutRevision_) {
    return;
  }
  // Commit before invoking: failed user side effects are never replayed automatically.
  record->delivered = value->geometry;
  const auto lifetime = std::weak_ptr(lifetime_);
  try {
    const auto result = record->action(value->geometry);
    if (!result && !lifetime.expired() && !commandReported(result, lifetime)) {
      remember(error(ManagedPhase::LAYOUT, record->node, result.result_.error()), record->origin);
    }
  } catch (const std::bad_alloc &) {
    if (!lifetime.expired()) {
      remember(error(ManagedPhase::LAYOUT, record->node, TreeError::NO_MEMORY), record->origin);
    }
  } catch (...) {
    if (!lifetime.expired()) {
      remember(error(ManagedPhase::LAYOUT, record->node, TreeError::CALLBACK_FAILED), record->origin);
    }
  }
}

std::expected<void, ManagedError> ManagedScene::admitActivity(std::shared_ptr<ui::detail::SceneActivity> activity) {
  auto allowed = boundary(ManagedPhase::BIND, root_);

  if (!allowed) {
    return allowed;
  }

  if (!activity) {
    return std::unexpected(error(ManagedPhase::BIND, root_, TreeError::INVALID_ARGUMENT));
  }

  if (servicingActivities_) {
    return std::unexpected(error(ManagedPhase::BIND, root_, TreeError::WOULD_BLOCK));
  }

  std::erase_if(asyncActivities_, [](const auto &weak) {
    const auto current = weak.lock();

    return !current || !current->active();
  });

  if (asyncActivities_.size() >= options_.maxNodes) {
    return std::unexpected(error(ManagedPhase::BIND, root_, TreeError::WOULD_BLOCK));
  }

  try {
    asyncActivities_.push_back(std::move(activity));

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, root_, TreeError::NO_MEMORY));
  }
}

std::expected<void, TreeError> ManagedScene::visitActivities(bool prepared) {
  if (closed_) {
    return {};
  }
  if (servicingActivities_) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }
  // Only admitted active invocations are visited; idle bindings and controls add no cycle scan.
  const auto activeCount = asyncActivities_.size();
  servicingActivities_ = true;

  for (std::size_t index = 0; index < activeCount && !closed_; ++index) {
    auto activity = asyncActivities_[index].lock();

    if (activity && activity->active()) {
      if (prepared) {
        activity->prepared();
      } else {
        activity->service();
      }
    }
  }

  servicingActivities_ = false;

  std::erase_if(asyncActivities_, [](const auto &weak) {
    const auto activity = weak.lock();

    return !activity || !activity->active();
  });

  return {};
}

std::expected<void, TreeError> ManagedScene::afterDispatch(Application &app) {
  if (&app != &app_ || owner_ != std::this_thread::get_id()) {
    return std::unexpected(owner_ != std::this_thread::get_id() ? TreeError::WRONG_THREAD
                                                                : TreeError::INVALID_ARGUMENT);
  }

  auto activities = visitActivities(false);

  if (!activities) {
    return activities;
  }

  if (closed_) {
    return {};
  }

  for (const auto &record : layouts_) {
    if (record->pending && !record->queued) {
      auto current = geometry(record->node);
      if (!current || record->pending->revision != layoutRevision_) {
        record->pending.reset();
        continue;
      }
      auto published = publishLayout(record);
      if (!published) {
        return published;
      }
    }
  }
  auto messages = prepareMessages();
  if (!messages) {
    return messages;
  }
  return prepareNotifications();
}

bool ManagedScene::hasPendingLayout() const noexcept {
  if (owner_ != std::this_thread::get_id() || closed_) {
    return false;
  }
  return std::ranges::any_of(layouts_, [](const auto &record) {
    return !record->retired && static_cast<bool>(record->pending);
  });
}

std::expected<void, TreeError> ManagedScene::completedLayout(Application &app) {
  auto allowed = boundary(ManagedPhase::LAYOUT, root_);
  if (!allowed) {
    return std::unexpected(bridge(allowed.error()));
  }
  if (&app != &app_) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  auto revision = app_.tree().geometryRevision();
  if (!revision) {
    return std::unexpected(revision.error());
  }
  // The final stamp is constant work without observers. Checked Ref reads inspect only
  // their own node on demand; copying a complete tree here would tax unrelated clients.
  layoutRevision_ = *revision;
  try {
    for (const auto &record : layouts_) {
      auto snapshot = app_.tree().observeGeometry(record->node);
      if (!snapshot) {
        record->pending.reset();
        record->delivered.reset();
        record->queued = false;
        if (!retired(snapshot.error())) {
          return std::unexpected(snapshot.error());
        }
        continue;
      }
      if (record->visibilityRevision != snapshot->visibilityRevision) {
        record->delivered.reset();
        record->visibilityRevision = snapshot->visibilityRevision;
      }
      if (!snapshot->geometry.valid) {
        record->pending.reset();
        record->delivered.reset();
        record->queued = false;
        continue;
      }
      if (record->delivered && sameGeometry(*record->delivered, snapshot->geometry)) {
        record->pending.reset();
        record->queued = false;
        continue;
      }
      record->pending = std::make_shared<const LayoutValue>(snapshot->geometry, layoutRevision_);
      record->queued = false;
      auto published = publishLayout(record);
      if (!published) {
        return published;
      }
    }
    completeScrollFrame();
    return prepareNotifications(true);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(TreeError::CALLBACK_FAILED);
  }
}

std::expected<void, ManagedError>
ManagedScene::composeButtonClick(Node<Button> node, std::function<EditorCommandResult(ui::ClickParams &)> action,
                                 std::source_location origin) {
  auto allowed = boundary(ManagedPhase::BIND, node);

  if (!allowed) {
    return allowed;
  }

  auto entry = find(node);

  if (!entry || !action || (!entry->clicks && !std::holds_alternative<std::monostate>(entry->binding))) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
  }

  if (clickHandlers_ >= options_.maxClickHandlers ||
      (entry->clicks && entry->clicks->records.size() >= options_.maxClickHandlersPerNode)) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NODE_LIMIT));
  }

  try {
    auto record = std::make_shared<ClickRecord>(std::move(action), origin);

    if (entry->clicks) {
      entry->clicks->records.push_back(std::move(record));
      ++clickHandlers_;

      return {};
    }

    auto chain = std::make_shared<ClickChain>();

    chain->records.push_back(std::move(record));

    auto bound = bindButtonClick(app_.router(), node,
                                 [this, node, chain, lifetime = std::weak_ptr(lifetime_)](const ClickEvent &event) {
                                   // Test weak lifetime before invoking any member on the borrowed owner.
                                   if (!lifetime.expired()) {
                                     dispatchClicks(node, chain, lifetime, event);
                                   }
                                 });

    if (!bound) {
      return std::unexpected(error(ManagedPhase::BIND, node, bound.error()));
    }

    entry->binding.emplace<ActionBinding>(std::move(*bound));
    entry->clicks = std::move(chain);
    ++clickHandlers_;

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::CALLBACK_FAILED));
  }
}

void ManagedScene::dispatchClicks(Node<Button> node, const std::shared_ptr<ClickChain> &chain,
                                  const std::weak_ptr<const void> &lifetime, const ClickEvent &event) {
  // The router retains this chain through self-removal. It never retains the scene.
  if (closed_ || chain->retired) {
    return;
  }

  const auto count = chain->records.size();
  auto origin = chain->records.front()->origin;

  try {
    ui::ClickParams params(event);

    for (std::size_t index = 0; index < count; ++index) {
      if (lifetime.expired() || chain->retired) {
        return;
      }

      if (closed_) {
        return;
      }

      auto eligible = app_.tree().inputEligible(node);

      if (!eligible || !*eligible) {
        if (!eligible && !retired(eligible.error())) {
          remember(error(ManagedPhase::INPUT, node, eligible.error()), origin);
        }

        return;
      }

      // Never borrow an iterator/reference across user code: appending can reallocate,
      // and removal clears the registry while this admitted record must stay alive.
      const auto record = chain->records[index];

      origin = record->origin;

      const auto command = record->action(params);

      if (!command) {
        if (!lifetime.expired() && !commandReported(command, lifetime)) {
          remember(error(ManagedPhase::INPUT, node, command.result_.error()), origin);
        }

        return;
      }

      if (params.propagationStopped()) {
        return;
      }
    }
  } catch (const std::bad_alloc &) {
    if (!lifetime.expired()) {
      remember(error(ManagedPhase::INPUT, node, TreeError::NO_MEMORY), origin);
    }
  } catch (...) {
    if (!lifetime.expired()) {
      remember(error(ManagedPhase::INPUT, node, TreeError::CALLBACK_FAILED), origin);
    }
  }
}

std::expected<void, ManagedError>
ManagedScene::bind(Node<CheckBox> node, ManagedState<bool> model,
                   std::function<std::expected<void, TreeError>(CheckBoxResult)> changed) {
  ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
  decltype(changed) incomingChanged;

  incomingChanged.swap(changed);

  auto entry = bindingEntry(node);

  if (!entry) {
    return std::unexpected(entry.error());
  }

  auto property = model.lock();

  if (!property || model.scene_ != root_.id()) {
    return std::unexpected(error(ManagedPhase::BIND, node, property ? TreeError::INVALID_ARGUMENT : property.error()));
  }

  try {
    auto changes = std::make_shared<ChangeChain>();

    changes->boolean = true;

    auto bound = bindCheckbox(app_.tree(), app_.router(), node, *property,
                              checkboxCallback<bool>(node, changes, std::move(incomingChanged)));

    if (!bound) {
      return std::unexpected(error(ManagedPhase::BIND, node, bound.error()));
    }

    (*entry)->binding.emplace<CheckBoxBinding>(std::move(*bound));
    (*entry)->changes = std::move(changes);

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::CALLBACK_FAILED));
  }
}

std::expected<void, ManagedError>
ManagedScene::bind(Node<CheckBox> node, ManagedState<CheckState> model,
                   std::function<std::expected<void, TreeError>(CheckStateResult)> changed, CheckBoxOptions options) {
  ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
  decltype(changed) incomingChanged;

  incomingChanged.swap(changed);

  auto entry = bindingEntry(node);

  if (!entry) {
    return std::unexpected(entry.error());
  }

  auto property = model.lock();

  if (!property || model.scene_ != root_.id()) {
    return std::unexpected(error(ManagedPhase::BIND, node, property ? TreeError::INVALID_ARGUMENT : property.error()));
  }

  try {
    auto changes = std::make_shared<ChangeChain>();

    changes->boolean = false;

    auto bound = bindCheckbox(app_.tree(), app_.router(), node, *property,
                              checkboxCallback<CheckState>(node, changes, std::move(incomingChanged)), options);

    if (!bound) {
      return std::unexpected(error(ManagedPhase::BIND, node, bound.error()));
    }

    (*entry)->binding.emplace<CheckStateBinding>(std::move(*bound));
    (*entry)->changes = std::move(changes);

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::CALLBACK_FAILED));
  }
}

std::expected<void, ManagedError> ManagedScene::syncCaption(Node<Control> node, CaptionBinding &binding,
                                                            ManagedPhase phase) try {
  if (binding.model.origin_) {
    auto live = binding.model.origin_->check();
    if (!live && (binding.model.origin_->retired || binding.model.origin_->closed)) {
      return {};
    }
  }
  auto snapshot = binding.model.snapshot();

  if (!snapshot) {
    return std::unexpected(error(phase, node, snapshot.error()));
  }

  if (binding.revision == (*snapshot)->revision) {
    return {};
  }

  auto text = GraphemeText::from((*snapshot)->value, widthPolicy());

  if (!text) {
    const auto kind = text.error().kind;
    const bool limited = kind == TextErrorKind::INPUT_LIMIT || kind == TextErrorKind::CLUSTER_LIMIT ||
                         kind == TextErrorKind::GRAPHEME_LIMIT || kind == TextErrorKind::CELL_LIMIT;
    auto failure = error(phase, node, limited ? TreeError::NODE_LIMIT : TreeError::INVALID_ARGUMENT);

    failure.text = text.error();

    return std::unexpected(failure);
  }

  auto applied = setControlText(app_.tree(), node, std::move(*text));

  if (!applied) {
    return std::unexpected(error(phase, node, applied.error()));
  }

  binding.revision = (*snapshot)->revision;

  return {};
} catch (const std::bad_alloc &) {
  return std::unexpected(error(phase, node, TreeError::NO_MEMORY));
} catch (const std::length_error &) {
  return std::unexpected(error(phase, node, TreeError::NODE_LIMIT));
} catch (...) {
  return std::unexpected(error(phase, node, TreeError::CALLBACK_FAILED));
}

std::expected<void, ManagedError> ManagedScene::bindText(Node<Control> node, ManagedState<std::string> model) {
  ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
  auto allowed = boundary(ManagedPhase::BIND, node);

  if (!allowed) {
    return allowed;
  }

  auto entry = find(node);

  if (!entry || entry->caption) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
  }

  auto property = model.lock();

  if (!property || model.scene_ != root_.id()) {
    return std::unexpected(error(ManagedPhase::BIND, node, property ? TreeError::INVALID_ARGUMENT : property.error()));
  }

  try {
    auto candidate = std::make_unique<CaptionBinding>();

    candidate->node = node;
    candidate->model = model;

    auto synced = syncCaption(node, *candidate, ManagedPhase::BIND);

    if (!synced) {
      return synced;
    }

    // Allocate metadata before changing the caption; publication itself cannot allocate.
    entry->caption = std::move(candidate);

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
  } catch (const std::length_error &) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NODE_LIMIT));
  } catch (...) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::CALLBACK_FAILED));
  }
}

std::expected<void, ManagedError>
ManagedScene::bind(Node<LineEdit> node, ManagedState<std::string> model,
                   std::function<std::expected<void, TreeError>(LineEditResult)> result,
                   std::shared_ptr<Clipboard> clipboard, std::function<bool(std::string_view)> validate) {
  ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
  decltype(result) incomingResult;
  decltype(validate) incomingValidate;

  incomingResult.swap(result);
  incomingValidate.swap(validate);

  auto entry = bindingEntry(node);

  if (!entry) {
    return std::unexpected(entry.error());
  }

  auto property = model.lock();

  if (!property || model.scene_ != root_.id()) {
    return std::unexpected(error(ManagedPhase::BIND, node, property ? TreeError::INVALID_ARGUMENT : property.error()));
  }

  std::shared_ptr<detail::EditorCommandFeedback> feedback;
  std::unique_ptr<ManagedState<std::string>> incomingModel;
  std::shared_ptr<EditorEventChain> editing;

  try {
    incomingModel = std::make_unique<ManagedState<std::string>>(model);
    feedback = std::make_shared<detail::EditorCommandFeedback>();
    editing = std::make_shared<EditorEventChain>();
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
  }

  try {
    auto bound = bindLineEditWithEvents(
      app_.tree(), app_.router(), node, *property,
      [this, node, lifetime = std::weak_ptr(lifetime_), feedback,
       result = ui::detail::currentSceneCallback(facadeContext(), std::move(incomingResult))](LineEditResult receipt) {
        const auto report = [this, node, &lifetime, feedback, &receipt, trigger = receipt.trigger](TreeError cause,
                                                                                                   bool commandFailure = false) {
          if (lifetime.expired()) {
            return;
          }

          const auto phase = trigger == LineEditTrigger::FLUENT ? ManagedPhase::TEXT : ManagedPhase::INPUT;

          const auto operation = commandFailure && feedback->active   ? feedback->operation
                                 : trigger == LineEditTrigger::BLUR   ? "lineEdit.blur"
                                 : trigger == LineEditTrigger::FLUENT ? "lineEdit.fluent"
                                                                      : "lineEdit.input";

          const auto origin = commandFailure && feedback->active ? feedback->origin : std::source_location::current();

          auto failure = error(phase, node, cause);

          failure.operation = trigger == LineEditTrigger::BLUR ? "lineEdit.blur" : nullptr;

          if (commandFailure) {
            failure.async = receipt.async;
            failure.background = receipt.background;
            failure.text = receipt.text;

            if (receipt.operation) {
              failure.operation = receipt.operation;
            }
          }

          failure.operationOrigin = origin;
          failure.reportReceipt.collector_ = lifetime_;

          auto *destination = FocusReport::destination(app_);

          if (commandFailure && trigger == LineEditTrigger::BLUR && destination && !*destination) {
            // Only this Application's failed loss receipt returns through the active focus call.
            // Independent result failures and other Applications never populate its destination.
            destination->emplace(failure);
          }

          const bool recoverableValidation = commandFailure && receipt.recoverable;
          const auto recordFailure = [&](std::span<const ManagedDiagnosticContext> contexts = {}) {
            if (recoverableValidation) {
              (void)reportError(failure, contexts, origin);
            } else {
              remember(failure, origin, contexts);
            }
          };

          try {
            const ManagedDiagnosticContext context{std::string(operation), std::string(0, '\0'), std::string(0, '\0'),
                                                   std::string("LineEdit"), origin};

            recordFailure(std::span(&context, 1));
          } catch (const std::bad_alloc &) {
            recordFailure();
          } catch (const std::length_error &) {
            recordFailure();
          }
        };

        try {
          const bool commandFailure = receipt.error &&
                                      (receipt.status == LineEditStatus::FAILED || receipt.recoverable);

          if (commandFailure) {
            const bool canReport = !lifetime.expired();

            report(*receipt.error, true);

            if (canReport && feedback->active) {
              feedback->reported = true;
            }
          }

          if (result) {
            auto status = result(std::move(receipt));

            if (!status) {
              report(status.error());
            }
          }
        } catch (const std::bad_alloc &) {
          report(TreeError::NO_MEMORY);
        } catch (...) {
          report(TreeError::CALLBACK_FAILED);
        }
      },
      std::move(clipboard),
      incomingValidate ? std::function<bool(std::string_view)>{ui::detail::currentSceneCallback(
                           facadeContext(), std::move(incomingValidate))}
                       : std::function<bool(std::string_view)>{},
      LineEditEvents{
        [this, node, editing, lifetime = std::weak_ptr<const void>(lifetime_)](const TextEditedEvent &event) {
          if (!lifetime.expired()) {
            dispatchEditing(node, editing, lifetime, event);
          }
        },
        [this, node, editing, lifetime = std::weak_ptr<const void>(lifetime_)](const EditingFinishedEvent &event) {
          if (!lifetime.expired()) {
            dispatchEditing(node, editing, lifetime, event);
          }
        }});

    if (!bound) {
      return std::unexpected(error(ManagedPhase::BIND, node, bound.error()));
    }

    bound->commandFeedback(std::move(feedback));
    (*entry)->binding.emplace<LineEditBinding>(std::move(*bound));
    (*entry)->editing = std::move(editing);
    (*entry)->textModel = std::move(incomingModel);

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::CALLBACK_FAILED));
  }
}

std::expected<void, ManagedError>
ManagedScene::bind(Node<Password> node, ManagedState<PasswordValue> model,
                   std::function<std::expected<void, TreeError>(PasswordResult)> result,
                   std::shared_ptr<Clipboard> clipboard, std::function<bool(SecretView)> validate) {
  ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
  auto entry = bindingEntry(node);

  if (!entry) {
    return std::unexpected(entry.error());
  }

  auto property = model.lock();

  if (!property || model.scene_ != root_.id()) {
    return std::unexpected(error(ManagedPhase::BIND, node, property ? TreeError::INVALID_ARGUMENT : property.error()));
  }

  try {
    auto feedback = std::make_shared<PasswordFeedback>();
    auto bound = bindPassword(
      app_.tree(), app_.router(), node, *property,
      [this, node, lifetime = std::weak_ptr<const void>(lifetime_), feedback,
       result = ui::detail::currentSceneCallback(facadeContext(), std::move(result))](PasswordResult receipt) {
        const auto phase = receipt.trigger == LineEditTrigger::FLUENT ? ManagedPhase::TEXT : ManagedPhase::INPUT;
        const auto report = [&](ManagedError failure, const char *operation = "password.input",
                                bool commandFailure = false) {
          if (lifetime.expired()) {
            return;
          }

          failure.operation = operation;
          failure.reportReceipt.collector_ = lifetime_;

          if (commandFailure && receipt.trigger == LineEditTrigger::BLUR) {
            if (auto *destination = FocusReport::destination(app_); destination && !*destination) {
              // Preserve this application's typed loss failure through the active focus command.
              destination->emplace(failure);
            }
          }

          if (feedback->active && !feedback->reported) {
            feedback->reported = failure;
          }

          remember(std::move(failure));
        };

        if (receipt.status == PasswordStatus::FAILED && receipt.failure) {
          report(detail::passwordError(phase, node.id(), *receipt.failure),
                 receipt.trigger == LineEditTrigger::BLUR     ? "password.blur"
                 : receipt.trigger == LineEditTrigger::FLUENT ? "password.fluent"
                                                              : "password.input",
                 true);
        }

        if (lifetime.expired() || !result) {
          return;
        }

        try {
          auto delivered = result(std::move(receipt));

          if (!delivered) {
            report(error(phase, node, delivered.error()), "password.callback");
          }
        } catch (const std::bad_alloc &) {
          report(error(phase, node, TreeError::NO_MEMORY), "password.callback");
        } catch (...) {
          report(error(phase, node, TreeError::CALLBACK_FAILED), "password.callback");
        }
      },
      std::move(clipboard),
      validate ? std::function<bool(SecretView)>{ui::detail::currentSceneCallback(facadeContext(), std::move(validate))}
               : std::function<bool(SecretView)>{});

    if (!bound) {
      return std::unexpected(detail::passwordError(ManagedPhase::BIND, node.id(), std::move(bound.error())));
    }

    (*entry)->passwordFeedback = std::move(feedback);
    (*entry)->binding.emplace<PasswordBinding>(std::move(*bound));

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::CALLBACK_FAILED));
  }
}

std::expected<PasswordValue, ManagedError> ManagedScene::passwordValue(Node<Password> node) const {
  auto allowed = boundary(ManagedPhase::TEXT, node);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  PasswordValue value;
  auto read = app_.tree().read(node, [&](const Password &field) {
    value = field.draft_;
  });

  if (!read) {
    return std::unexpected(error(ManagedPhase::TEXT, node, read.error()));
  }
  if (!value.available()) {
    return std::unexpected(detail::passwordError(
      ManagedPhase::TEXT, node.id(),
      SecureStringFailure{{SecureStringOperation::BORROW, SecureStringErrorCode::UNAVAILABLE}, {}}));
  }

  return value;
}

std::expected<void, ManagedError> ManagedScene::setPasswordValue(Node<Password> node, PasswordValue value) {
  auto allowed = boundary(ManagedPhase::TEXT, node);

  if (!allowed) {
    return allowed;
  }

  auto entry = find(node);
  auto *binding = entry ? std::get_if<PasswordBinding>(&entry->binding) : nullptr;

  if (!binding || !entry->passwordFeedback) {
    return std::unexpected(error(ManagedPhase::TEXT, node, TreeError::INVALID_ARGUMENT));
  }

  auto feedback = entry->passwordFeedback;

  if (feedback->active) {
    return std::unexpected(error(ManagedPhase::TEXT, node, TreeError::WOULD_BLOCK));
  }

  struct Reset {
    std::shared_ptr<PasswordFeedback> feedback;

    ~Reset() {
      feedback->active = false;
      feedback->reported.reset();
    }
  } reset{feedback};

  feedback->active = true;
  auto changed = binding->setValue(std::move(value));

  if (!changed) {
    return std::unexpected(
      feedback->reported.value_or(detail::passwordError(ManagedPhase::TEXT, node.id(), std::move(changed.error()))));
  }
  if (changed->failure) {
    return std::unexpected(
      feedback->reported.value_or(detail::passwordError(ManagedPhase::TEXT, node.id(), std::move(*changed->failure))));
  }
  if (changed->callbackError) {
    return std::unexpected(feedback->reported.value_or(error(ManagedPhase::TEXT, node, *changed->callbackError)));
  }
  if (feedback->reported) {
    return std::unexpected(*feedback->reported);
  }

  return {};
}

void ManagedScene::resetKeys(KeyPending &pending) noexcept {
  // Unsigned exhaustion produces a permanent invalid stamp instead of reusing an old generation.
  if (pending.generation) {
    ++pending.generation;
  }

  pending.candidates.clear();
  pending.scratch.clear();
  pending.depth = 0;
  pending.timer = {};
}

std::expected<void, ManagedError>
ManagedScene::registerLegacyKey(KeyEvent chord, std::function<EditorCommandResult(const KeyEvent &)> action) {
  auto record = std::make_shared<KeyRecord>();

  record->legacy = chord;
  record->action = std::move(action);

  return publishKey(std::move(record));
}

std::expected<void, ManagedError>
ManagedScene::registerShortcut(Shortcut &&shortcut, std::function<EditorCommandResult(const KeyEvent &)> action,
                               detail::ShortcutActions actions, std::source_location origin) {
  auto record = std::make_shared<KeyRecord>();

  record->shortcut.emplace(std::move(shortcut));
  record->action = std::move(action);
  record->actions = actions;
  record->origin = origin;

  return publishKey(std::move(record));
}

std::expected<void, ManagedError> ManagedScene::publishKey(std::shared_ptr<KeyRecord> record) {
  auto snapshot = std::make_shared<KeySnapshot>();

  if (keys_) {
    snapshot->records = keys_->records;
    snapshot->fallback = keys_->fallback;
  } else {
    snapshot->fallback = input_;
  }

  snapshot->records.push_back(std::move(record));
  snapshot->annotated = std::ranges::any_of(snapshot->records, [](const auto &entry) {
    return bool(entry->metadata);
  });
  if (snapshot->annotated) {
    snapshot->scopeConnected.emplace(0);
    snapshot->scopeConnected->reserve(snapshot->records.size());
    for (const auto &entry : snapshot->records) {
      snapshot->scopeConnected->push_back(entry->metadata ? entry->metadata->observation() : 0);
    }
  }
  snapshot->pending = std::make_shared<KeyPending>();

  std::size_t capacity{};

  for (const auto &entry : snapshot->records) {
    const auto count = entry->shortcut ? entry->shortcut->sequences.size() : 0;

    if (count > maxElementCount<KeyPending::Candidate>() - capacity) {
      return std::unexpected(error(ManagedPhase::BIND, root_, TreeError::NODE_LIMIT));
    }

    capacity += count;
  }

  snapshot->pending->candidates.reserve(capacity);
  snapshot->pending->scratch.reserve(capacity);

  return setInput(
    [this, snapshot](const InputEvent &event) {
      return dispatchKeys(snapshot, event);
    },
    snapshot->records.size(), snapshot);
}

std::expected<bool, ManagedError> ManagedScene::invokeKey(const std::shared_ptr<KeyRecord> &record,
                                                          const KeyEvent &event) {
  const auto lifetime = std::weak_ptr(lifetime_);
  const auto failure = [&](TreeError cause) -> std::expected<bool, ManagedError> {
    auto result = error(ManagedPhase::INPUT, root_, cause);

    result.registrationOrigin = record->origin;

    return std::unexpected(std::move(result));
  };

  try {
    if (record->metadata) {
      if (!keys_ ||
          std::ranges::none_of(keys_->records,
                               [&](const auto &current) {
                                 return current == record;
                               }) ||
          keyEligibility(*record, app_.router().state_->focus) != KeyEligibility::ELIGIBLE) {
        return false;
      }
    }
    const auto action = record->annotatedAction;
    const auto command = action ? (*action)(event) : record->action(event);

    if (!command && !commandReported(command, lifetime)) {
      return failure(command.result_.error());
    }
  } catch (const std::bad_alloc &) {
    return failure(TreeError::NO_MEMORY);
  } catch (...) {
    return failure(TreeError::CALLBACK_FAILED);
  }

  return true;
}

std::expected<bool, ManagedError> ManagedScene::waitForKey(const std::shared_ptr<KeySnapshot> &snapshot,
                                                           const std::shared_ptr<KeyRecord> &winner,
                                                           std::size_t depth) {
  auto &pending = *snapshot->pending;
  const auto focus = app_.router().focusGeneration();
  const auto input = app_.tree().inputGeneration();
  const auto failure = [&](TreeError cause) -> std::expected<bool, ManagedError> {
    resetKeys(pending);

    auto result = error(ManagedPhase::INPUT, root_, cause);

    result.registrationOrigin = winner->origin;

    return std::unexpected(std::move(result));
  };

  if (!pending.generation || !focus || !input) {
    return failure(TreeError::IDENTITY_EXHAUSTED);
  }

  // Disconnect before rearming so the existing scheduler never needs two live prefix timers.
  ++pending.generation;
  pending.timer = {};

  if (!pending.generation) {
    return failure(TreeError::IDENTITY_EXHAUSTED);
  }

  const auto now = app_.now();
  const auto deadline = now + std::min(options_.keySequenceTimeout, std::numeric_limits<SchedulerTime>::max() - now);
  const auto generation = pending.generation;
  std::expected<Subscription, SchedulerError> timer;
  const auto scoped = app_.tree().withScope(root_, [&](const UiScope &scope) {
    timer = app_.scheduler().at(scope, deadline, [weak = std::weak_ptr(snapshot->pending), generation] {
      if (auto state = weak.lock(); state && state->generation == generation) {
        resetKeys(*state);
      }
    });
  });

  if (!scoped) {
    return failure(scoped.error());
  }

  if (!timer) {
    return failure(WidgetTree::fromScheduler(timer.error()));
  }

  pending.candidates.swap(pending.scratch);
  pending.scratch.clear();
  pending.depth = depth;
  pending.deadline = deadline;
  pending.focusGeneration = focus;
  pending.inputGeneration = input;
  pending.timer = std::move(*timer);

  return true;
}

std::expected<bool, ManagedError> ManagedScene::dispatchKeys(const std::shared_ptr<KeySnapshot> &snapshot,
                                                             const InputEvent &event) {
  Node<Widget> focus;
  if (snapshot->annotated) {
    const auto revision = keyRevision_;
    const auto input = input_;
    auto recovered = app_.router().focused();
    if (!recovered) {
      return std::unexpected(error(ManagedPhase::INPUT, root_, recovered.error()));
    }
    if (keys_ != snapshot || keyRevision_ != revision || input_ != input) {
      return false;
    }
    focus = *recovered;
    auto observed = observeKeyScopes();
    if (!observed) {
      return std::unexpected(observed.error());
    }
  }
  const auto eligible = [&](const std::shared_ptr<KeyRecord> &record) {
    if (!record->metadata) {
      return true;
    }
    if (keyEligibility(*record, focus) != KeyEligibility::ELIGIBLE) {
      return false;
    }
    const auto *key = std::get_if<KeyEvent>(&event);
    const auto phase = key ? key->action : KeyAction::PRESS;
    const auto support = phase == KeyAction::PRESS    ? options_.keyCapabilities.press
                         : phase == KeyAction::REPEAT ? options_.keyCapabilities.repeat
                                                      : options_.keyCapabilities.release;
    return support != KeySupport::UNSUPPORTED;
  };
  auto &pending = *snapshot->pending;

  if (!pending.candidates.empty() &&
      (app_.now() >= pending.deadline || pending.focusGeneration != app_.router().focusGeneration() ||
       pending.inputGeneration != app_.tree().inputGeneration())) {
    resetKeys(pending);
  }

  if (!pending.candidates.empty()) {
    const auto *cancel = std::get_if<KeyEvent>(&event);

    if (cancel && cancel->action == KeyAction::PRESS && cancel->key == InputKey::ESCAPE &&
        cancel->modifiers == InputModifiers{}) {
      resetKeys(pending);

      return true;
    }

    std::shared_ptr<KeyRecord> winner;
    KeyEvent admitted;
    bool terminal{};
    bool pressAdmitted{};

    pending.scratch.clear();

    for (const auto candidate : pending.candidates) {
      const auto &record = snapshot->records[candidate.record];
      if (!eligible(record)) {
        continue;
      }
      const auto key = detail::shortcutEvent(event, record->actions);

      if (!key) {
        continue;
      }

      pressAdmitted = pressAdmitted || key->action == KeyAction::PRESS;

      const auto strokes = detail::shortcutAlternative(*record->shortcut, candidate.alternative);

      if (!detail::shortcutChordMatches(strokes[pending.depth], *key)) {
        continue;
      }

      const bool complete = pending.depth + 1 == strokes.size();

      if (!winner) {
        winner = record;
        admitted = *key;
        terminal = complete;
      }

      if (!complete) {
        pending.scratch.push_back(candidate);
      }
    }

    if (winner) {
      if (terminal) {
        resetKeys(pending);

        return invokeKey(winner, admitted);
      }

      return waitForKey(snapshot, winner, pending.depth + 1);
    }

    if (pressAdmitted) {
      resetKeys(pending);
    }
  }

  // A mismatching or ignored-phase event reaches this root pass exactly once; old strokes are never replayed.
  std::shared_ptr<KeyRecord> winner;
  KeyEvent admitted;
  bool terminal{};

  pending.scratch.clear();

  for (auto index = snapshot->records.size(); index-- > 0;) {
    const auto &record = snapshot->records[index];
    if (!eligible(record)) {
      continue;
    }

    if (record->legacy) {
      const auto *key = std::get_if<KeyEvent>(&event);
      const auto chord = *record->legacy;

      if (key && key->action != KeyAction::RELEASE && key->key == chord.key && key->scalar == chord.scalar &&
          key->modifiers == chord.modifiers && !winner) {
        return invokeKey(record, *key);
      }

      continue;
    }

    const auto key = detail::shortcutEvent(event, record->actions);

    if (!key) {
      continue;
    }

    const auto count = record->shortcut->alternatives.size() + record->shortcut->sequences.size();

    for (std::size_t alternative{}; alternative < count; ++alternative) {
      const auto strokes = detail::shortcutAlternative(*record->shortcut, alternative);

      if (!detail::shortcutChordMatches(strokes.front(), *key)) {
        continue;
      }

      const bool complete = strokes.size() == 1;

      if (!winner) {
        winner = record;
        admitted = *key;
        terminal = complete;
      }

      if (!complete) {
        pending.scratch.push_back({index, alternative});
      }
    }

    if (winner && terminal) {
      return invokeKey(winner, admitted);
    }
  }

  if (winner) {
    return waitForKey(snapshot, winner, 1);
  }

  return snapshot->fallback ? (*snapshot->fallback)(event) : std::expected<bool, ManagedError>{false};
}

std::expected<void, ManagedError>
ManagedScene::onInput(std::function<std::expected<bool, ManagedError>(const InputEvent &)> callback) {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(error(ManagedPhase::BIND, root_, TreeError::WRONG_THREAD));
  }
  if (keyStaging_) {
    return std::unexpected(error(ManagedPhase::BIND, root_, TreeError::WOULD_BLOCK));
  }
  ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
  decltype(callback) incoming;

  incoming.swap(callback);

  return setInput(std::move(incoming), 0);
}

std::expected<void, ManagedError>
ManagedScene::setInput(std::function<std::expected<bool, ManagedError>(const InputEvent &)> callback,
                       std::size_t keyHandlers, std::shared_ptr<KeySnapshot> keys) {
  ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
  decltype(callback) incoming;

  incoming.swap(callback);

  auto allowed = boundary(ManagedPhase::BIND, root_);

  if (!allowed) {
    return allowed;
  }

  try {
    if (incoming && !keys) {
      incoming = ui::detail::currentSceneCallback(facadeContext(), std::move(incoming));
    }

    auto prepared = incoming ? std::make_shared<decltype(incoming)>(std::move(incoming)) : nullptr;

    // Publish callback and count before retiring captures: their destructors can reenter onKey.
    input_.swap(prepared);
    keyHandlers_ = keyHandlers;
    keys_.swap(keys);
    if (keyRevision_) {
      ++keyRevision_;
    }
    keyProjection_.reset();

    if (keys) {
      resetKeys(*keys->pending);
    }
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, root_, TreeError::NO_MEMORY));
  }

  return {};
}

std::expected<void, TreeError> ManagedScene::prepare(Application &app, CellSize) {
  ui::detail::SetupFailurePause setup(facadeContext());
  ui::detail::CurrentSceneScope mask;
  ui::detail::CurrentSceneScope current(facadeContext());
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(TreeError::WRONG_THREAD);
  }

  if (reporting_ || recoveringObservers_ || observerCallbacks_ || observerMountPending()) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  auto allowed = boundary(ManagedPhase::PREPARE, root_);

  if (!allowed) {
    remember(allowed.error());

    return std::unexpected(bridge(allowed.error()));
  }

  if (&app != &app_) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  auto actions = ui::detail::ActionAccess::prepare(*this);
  if (!actions) {
    return actions;
  }

  auto selector = prepareThemeSelector();
  if (!selector) {
    return std::unexpected(bridge(selector.error()));
  }

  auto recovered = recoverObservers(0, nullptr, nullptr, true);
  if (!recovered) {
    return recovered;
  }

  try {
    // The preparing guard rejects registry changes; prune only after every active borrow ends.
    struct Prune {
      ManagedScene &scene;

      ~Prune() {
        const bool previous = scene.preparing_;
        scene.preparing_ = true;
        std::erase_if(scene.entries_, [&](const auto &entry) {
          if (entry->removed) {
            scene.retireClicks(*entry);
            scene.retireEditing(*entry);
            scene.retireChanges(*entry);
            scene.retireLayouts(*entry);
          }

          return entry->removed;
        });
        scene.pruneScroll();
        scene.preparing_ = previous;
      }
    } prune{*this};

    const auto recoverFocus = [&] {
      FocusReport report(*this);
      auto focused = app_.router().focused();

      if (!focused && !report.matches(focused.error())) {
        remember(error(ManagedPhase::PREPARE, root_, focused.error()));
      }

      return focused;
    };
    auto focused = recoverFocus();

    if (!focused) {
      return std::unexpected(focused.error());
    }

    auto keyScopes = observeKeyScopes();
    if (!keyScopes) {
      return std::unexpected(bridge(keyScopes.error()));
    }

    auto scrollFocus = sampleScrollFocus();
    if (!scrollFocus) {
      return scrollFocus;
    }

    const bool hasAdapters = !liveText_.empty() || std::ranges::any_of(models_, [](const auto &source) {
      return source->bound();
    });
    std::optional<std::vector<Node<Widget>>> customDone;
    if (hasAdapters) {
      customDone.emplace(0);
      customDone->reserve(entries_.size());
    }
    const auto syncEntries = [&]() -> std::expected<void, TreeError> {
      struct Preparing {
        bool &flag;
        ~Preparing() {
          flag = false;
        }
      } guard{preparing_};
      preparing_ = true;
      std::optional<std::vector<std::shared_ptr<Entry>>> owners;
      if (hasAdapters) {
        owners.emplace(entries_);
      }
      const std::span<const std::shared_ptr<Entry>> entries = owners ? std::span(*owners) : std::span(entries_);
      for (const auto &entry : entries) {
        auto live = app_.tree().read(entry->node, [](const Widget &) {
        });

        if (!live && retired(live.error())) {
          stylesDirty_ = true;
          entry->removed = true;

          continue;
        }

        std::expected<void, TreeError> status = live;

        if (status && entry->caption) {
          auto synced = syncCaption(entry->caption->node, *entry->caption, ManagedPhase::PREPARE);

          if (!synced) {
            remember(synced.error());

            return std::unexpected(bridge(synced.error()));
          }
        }

        if (status) {
          if (auto *edit = std::get_if<LineEditBinding>(&entry->binding)) {
            status = edit->sync(*focused);
          } else if (auto *password = std::get_if<PasswordBinding>(&entry->binding)) {
            auto synced = password->sync(*focused);

            if (!synced) {
              auto failure = detail::passwordError(ManagedPhase::PREPARE, entry->node.id(), std::move(synced.error()));

              remember(failure);

              return std::unexpected(bridge(failure));
            }
          } else if (auto *check = std::get_if<CheckBoxBinding>(&entry->binding)) {
            status = check->sync();
          } else if (auto *typedCheck = std::get_if<CheckStateBinding>(&entry->binding)) {
            status = typedCheck->sync();
          }
        }

        if (!status) {
          auto failure = error(ManagedPhase::PREPARE, entry->node, status.error());
          for (const auto &capture : liveText_) {
            if ((status.error() == TreeError::INVALID_ARGUMENT || status.error() == TreeError::NODE_LIMIT) &&
                capture->origin->node == entry->node && capture->validateModel) {
              try {
                auto validated = capture->validateModel();
                if (!validated && validated.error().text && bridge(validated.error()) == status.error()) {
                  failure.text = validated.error().text;
                  failure.operation = capture->operation;
                  failure.registrationOrigin = capture->registered;
                  break;
                }
              } catch (...) {
                // Diagnostic enrichment is best effort and cannot replace the original sync failure.
              }
            }
          }
          remember(failure);
          return status;
        }

        if (status && entry->sync) {
          status = entry->sync(*focused);
        }

        if (status && entry->customSync && (!customDone || !std::ranges::contains(*customDone, entry->node))) {
          if (customDone) {
            customDone->push_back(entry->node);
          }
          status = entry->customSync(*focused);
        }

        if (!status) {
          // Retirement cannot erase an explicit error from an admitted callback.
          remember(error(ManagedPhase::PREPARE, entry->node, status.error()));

          return status;
        }
      }

      return {};
    };
    auto synced = syncEntries();
    if (!synced) {
      return synced;
    }
    // Each post-sync round contains exactly one observer scan; arbitrary custom sync is never repeated.
    if (hasAdapters) {
      bool stable{};
      for (std::size_t pass = 0; pass < options_.maxObserverRecoveryPasses; ++pass) {
        if (pass) {
          focused = recoverFocus();
          if (!focused) {
            return std::unexpected(focused.error());
          }
          synced = syncEntries();
          if (!synced) {
            return synced;
          }
        }
        auto captured = captureLiveText();
        if (!captured) {
          return std::unexpected(captured.error());
        }
        bool invoked{}, pending{};
        auto scanned = recoverObservers(1, &invoked, &pending);
        if (!scanned) {
          return scanned;
        }
        bool applied{};

        struct Applying {
          bool &flag;

          ~Applying() {
            flag = false;
          }
        } applying{preparing_};

        preparing_ = true;
        const auto models = models_;
        for (const auto &source : models) {
          auto result = source->synchronize();
          if (!result) {
            remember(error(ManagedPhase::PREPARE, root_, result.error()));
            return std::unexpected(result.error());
          }
          applied = applied || *result;
        }
        if (!*captured && !invoked && !pending && !applied) {
          stable = true;
          break;
        }
      }
      if (!stable) {
        remember(error(ManagedPhase::PREPARE, root_, TreeError::WOULD_BLOCK));
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
    }

    struct Pinning {
      bool &flag;

      ~Pinning() {
        flag = false;
      }
    };

    {
      Pinning pinning{preparing_};

      preparing_ = true;

      for (const auto &source : models_) {
        auto pinned = source->pin();

        if (!pinned) {
          remember(error(ManagedPhase::PREPARE, root_, pinned.error()));

          return pinned;
        }
      }
    }

    // These bounded adapters observe the just-pinned model revision without repinning or scanning data.
    auto activities = visitActivities(true);

    if (!activities || closed_) {
      return activities ? std::unexpected(TreeError::CLOSED) : activities;
    }

    Pinning styling{preparing_};

    preparing_ = true;

    auto styled = flushStyles();

    if (!styled) {
      remember(styled.error());

      return std::unexpected(bridge(styled.error()));
    }

    return {};
  } catch (const std::bad_alloc &) {
    remember(error(ManagedPhase::PREPARE, root_, TreeError::NO_MEMORY));

    return std::unexpected(TreeError::NO_MEMORY);
  } catch (...) {
    remember(error(ManagedPhase::PREPARE, root_, TreeError::CALLBACK_FAILED));

    return std::unexpected(TreeError::CALLBACK_FAILED);
  }
}

bool ManagedScene::usesPreparedLayout() const noexcept {
  return usesGeometrySnapshot() || !spinners_.empty() || !dataStates_.empty() || static_cast<bool>(dialogs_) ||
         static_cast<bool>(notifications_) || std::ranges::any_of(entries_, [](const auto &entry) {
           return static_cast<bool>(entry->viewPrepare);
         });
}

std::expected<void, TreeError> ManagedScene::prepareViews(Application &app, const LayoutSnapshot &snapshot) {
  ui::detail::SetupFailurePause setup(facadeContext());
  ui::detail::CurrentSceneScope mask;
  ui::detail::CurrentSceneScope current(facadeContext());
  auto allowed = boundary(ManagedPhase::PREPARE, root_);

  if (!allowed) {
    return std::unexpected(bridge(allowed.error()));
  }

  if (&app != &app_) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  struct Preparing {
    bool &flag;

    ~Preparing() {
      flag = false;
    }
  } guard{preparing_};

  preparing_ = true;

  auto dialogs = prepareDialogs();
  if (!dialogs) {
    remember(error(ManagedPhase::PREPARE, root_, dialogs.error()));
    return dialogs;
  }
  auto notifications = prepareNotifications();
  if (!notifications) {
    return notifications;
  }

  for (const auto &entry : entries_) {
    if (!entry->viewPrepare) {
      continue;
    }

    // Low-level removal from an earlier callback can retire a later sibling.
    // Skip only retired identities; never erase an admitted callback's explicit error.
    auto live = app_.tree().read(entry->node, [](const Widget &) {
    });

    if (!live && retired(live.error())) {
      continue;
    }

    std::expected<void, TreeError> result = live;

    try {
      if (result) {
        result = entry->viewPrepare(snapshot);
      }
    } catch (const std::bad_alloc &) {
      result = std::unexpected(TreeError::NO_MEMORY);
    } catch (...) {
      result = std::unexpected(TreeError::CALLBACK_FAILED);
    }

    if (!result) {
      remember(error(ManagedPhase::PREPARE, entry->node, result.error()));

      return result;
    }
  }

  auto scrollPrepared = prepareScrollFrame(snapshot);
  if (!scrollPrepared) {
    remember(error(ManagedPhase::PREPARE, root_, scrollPrepared.error()));

    return scrollPrepared;
  }

  auto indicators = prepareSpinners();
  return indicators ? prepareDataStates(snapshot) : indicators;
}

std::expected<bool, TreeError> ManagedScene::input(Application &app, const InputEvent &event) {
  ui::detail::SetupFailurePause setup(facadeContext());
  ui::detail::CurrentSceneScope mask;
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(TreeError::WRONG_THREAD);
  }

  if (reporting_) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  auto allowed = boundary(ManagedPhase::INPUT, root_);

  if (!allowed) {
    remember(allowed.error());

    return std::unexpected(bridge(allowed.error()));
  }

  if (&app != &app_) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  try {
    // Pin the callback even if it replaces the interceptor or closes its subtree.
    if (modalDialogInput()) {
      if (keys_) {
        resetKeys(*keys_->pending);
      }
      return false;
    }
    auto callback = input_;

    if (!callback) {
      return false;
    }

    auto result = (*callback)(event);

    if (!result) {
      remember(result.error(), result.error().registrationOrigin.value_or(std::source_location::current()));

      return std::unexpected(bridge(result.error()));
    }

    return *result;
  } catch (const std::bad_alloc &) {
    remember(error(ManagedPhase::INPUT, root_, TreeError::NO_MEMORY));

    return std::unexpected(TreeError::NO_MEMORY);
  } catch (...) {
    remember(error(ManagedPhase::INPUT, root_, TreeError::CALLBACK_FAILED));

    return std::unexpected(TreeError::CALLBACK_FAILED);
  }
}

std::expected<void, ManagedError> ManagedScene::close() {
  ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(error(ManagedPhase::CLEANUP, root_, TreeError::WRONG_THREAD));
  }

  if (preparing_ || reporting_ || styling_ || closing_ || dialogOperation_ || collectingDialogs_ ||
      recoveringObservers_ || pruningObservers_ || observerCallbacks_ || (sessionActive_ && !stoppingSession_)) {
    return std::unexpected(error(ManagedPhase::CLEANUP, root_, TreeError::WOULD_BLOCK));
  }

  if (closed_) {
    return {};
  }

  struct Closing {
    bool &flag;

    ~Closing() {
      flag = false;
    }
  } guard{closing_};

  closing_ = true;

  // Consume before entry: failed removal or callback reentry must never stop external work twice.
  const auto beforeClose = std::exchange(beforeClose_, nullptr);
  auto stopped = beforeClose ? beforeClose(closeContext_) : std::expected<void, ManagedError>{};

  auto removed = app_.tree().remove(root_);

  if (!removed && !retired(removed.error())) {
    if (!stopped) {
      stopped.error().cleanup = removed.error();
      closeFailure_ = stopped.error();

      return stopped;
    }

    return std::unexpected(error(ManagedPhase::CLEANUP, root_, removed.error()));
  }

  // Mark closed before retiring captures: their destructors may reenter application code.
  closed_ = true;

  // Retire in place: even moving an empty vector may allocate an MSVC Debug iterator proxy.
  // Closed admission prevents additions; pop before cancellation so recursive close cannot revisit this record.
  while (!asyncActivities_.empty()) {
    auto activity = asyncActivities_.back().lock();

    asyncActivities_.pop_back();

    if (activity) {
      activity->cancel();
    }
  }

  ui::detail::ActionAccess::retire(*this);
  retireThemeSelector();
  themeSelection_.reset();
  retireMessages();
  stopSpinners();
  pruneDataStates();
  if (dataStateOwner_) {
    dataStateOwner_->scene = nullptr;
  }
  dataStateOwner_.reset();
  retireDialogs();
  retireNotifications();

  for (const auto &record : liveText_) {
    record->origin->closed = true;
  }

  for (const auto &record : observers_) {
    record->retired = true;
    record->subscription = {};
  }
  observers_.clear();
  pruneLiveText();

  for (auto &layer : styles_.layerState_.layers) {
    layer.document.reset();
  }

  for (auto &entry : entries_) {
    retireClicks(*entry);
    retireEditing(*entry);
    retireChanges(*entry);
    retireLayouts(*entry);
  }

  entries_.clear();
  if (scroll_) {
    scroll_->pending.reset();
  }
  scroll_.reset();
  models_.clear();
  if (keys_) {
    resetKeys(*keys_->pending);
  }

  keys_.reset();
  input_ = {};
  errorObserver_ = {};

  if (!stopped) {
    closeFailure_ = stopped.error();
  }

  return stopped;
}

void ManagedScene::stop(Application &app) {
  if (&app != &app_) {
    cleanup_ = TreeError::INVALID_ARGUMENT;

    return;
  }

  stoppingSession_ = true;

  auto result = close();

  stoppingSession_ = false;

  if (!result) {
    cleanup_ = bridge(result.error());
  }
}

void ManagedScene::retainCloseFailure() {
  if (!closeFailure_ || closeFailureRetained_) {
    return;
  }

  closeFailureRetained_ = true;
  (void)recordError(*closeFailure_, {}, closeFailure_->operationOrigin.value_or(std::source_location::current()));
}

std::expected<ApplicationStats, ManagedError>
ManagedScene::finish(std::expected<ApplicationStats, ApplicationError> result) {
  runFailureNotified_ = false;

  // Keep exact shutdown details even when an earlier input/runner error owns the primary envelope.
  if (failure_ || !result) {
    retainCloseFailure();
  }

  if (failure_) {
    auto failure = *failure_;

    if (!result) {
      failure.application = result.error();
    }

    failure.cleanup = cleanup_;

    if (failureDiagnostic_) {
      errors_[*failureDiagnostic_].error = failure;
    } else if (!dialogPrimaryRecorded_) {
      bool invoked{};

      (void)recordError(failure, {}, std::source_location::current(), &invoked, true);
      failureNotified_ = failureNotified_ || invoked;
    }

    runFailureNotified_ = failureNotified_;

    return std::unexpected(failure);
  }

  if (!result) {
    ManagedError failure{ManagedPhase::RUN, root_.id(), result.error(), result.error(), cleanup_};

    (void)recordError(failure, {}, std::source_location::current(), &runFailureNotified_, true);

    return std::unexpected(failure);
  }

  if (closeFailure_) {
    closeFailureRetained_ = true;
    (void)recordError(*closeFailure_, {}, closeFailure_->operationOrigin.value_or(std::source_location::current()),
                      &runFailureNotified_, true);

    return std::unexpected(*closeFailure_);
  }

  if (cleanup_) {
    ManagedError failure{ManagedPhase::CLEANUP, root_.id(), *cleanup_, {}, cleanup_};

    (void)recordError(failure, {}, std::source_location::current(), &runFailureNotified_, true);

    return std::unexpected(failure);
  }

  return *result;
}

std::expected<ApplicationStats, ManagedError> ManagedScene::run(std::stop_token stop) {
  if (owner_ == std::this_thread::get_id() && (sessionActive_ || sessionOperating_)) {
    return std::unexpected(error(ManagedPhase::RUN, root_, TreeError::WOULD_BLOCK));
  }

  auto allowed = boundary(ManagedPhase::RUN, root_);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  if (closing_) {
    return std::unexpected(error(ManagedPhase::RUN, root_, TreeError::WOULD_BLOCK));
  }

  sessionOperating_ = true;

  auto result = finish(app_.run(*this, stop));

  sessionOperating_ = false;

  return result;
}

std::expected<ApplicationStats, ManagedError> ManagedScene::run(ApplicationTerminal &terminal, std::stop_token stop) {
  if (owner_ == std::this_thread::get_id() && (sessionActive_ || sessionOperating_)) {
    return std::unexpected(error(ManagedPhase::RUN, root_, TreeError::WOULD_BLOCK));
  }

  auto allowed = boundary(ManagedPhase::RUN, root_);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  if (closing_) {
    return std::unexpected(error(ManagedPhase::RUN, root_, TreeError::WOULD_BLOCK));
  }

  sessionOperating_ = true;

  auto result = finish(app_.run(*this, terminal, stop));

  sessionOperating_ = false;

  return result;
}

std::expected<ApplicationStep, ManagedError>
ManagedScene::finishStep(std::expected<ApplicationStep, ApplicationError> result) {
  if (result && !result->finished) {
    return *result;
  }

  sessionActive_ = false;

  auto finished =
    finish(result ? std::expected<ApplicationStats, ApplicationError>(result->stats) : std::unexpected(result.error()));

  if (finished) {
    sessionResult_.emplace(ApplicationStep{true, *finished});
  } else {
    sessionResult_.emplace(std::unexpected(finished.error()));
  }

  return *sessionResult_;
}

std::expected<void, ManagedError> ManagedScene::start(ApplicationReadyTerminal &terminal, std::stop_token stop) {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(error(ManagedPhase::RUN, root_, TreeError::WRONG_THREAD));
  }

  if (sessionActive_ || sessionOperating_) {
    return std::unexpected(error(ManagedPhase::RUN, root_, TreeError::WOULD_BLOCK));
  }

  auto allowed = boundary(ManagedPhase::RUN, root_);

  if (!allowed) {
    return allowed;
  }

  if (closing_) {
    return std::unexpected(error(ManagedPhase::RUN, root_, TreeError::WOULD_BLOCK));
  }

  sessionActive_ = sessionOperating_ = true;

  auto result = app_.start(*this, terminal, stop);

  // Application may finish synchronously for cancellation or a start failure.
  if (!result || closed_) {
    auto finished =
      finishStep(result ? std::expected<ApplicationStep, ApplicationError>(ApplicationStep{true, app_.stats()})
                        : std::unexpected(result.error()));

    sessionOperating_ = false;

    if (!finished) {
      return std::unexpected(finished.error());
    }
  } else {
    sessionOperating_ = false;
  }

  return {};
}

std::expected<ApplicationStep, ManagedError> ManagedScene::step() {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(error(ManagedPhase::RUN, root_, TreeError::WRONG_THREAD));
  }

  if (sessionOperating_ || preparing_ || reporting_ || styling_ || closing_) {
    return std::unexpected(error(ManagedPhase::RUN, root_, TreeError::WOULD_BLOCK));
  }

  if (sessionResult_) {
    return *sessionResult_;
  }

  if (!sessionActive_) {
    return std::unexpected(error(ManagedPhase::RUN, root_, closed_ ? TreeError::CLOSED : TreeError::INVALID_ARGUMENT));
  }

  sessionOperating_ = true;

  auto result = finishStep(app_.step());

  sessionOperating_ = false;

  return result;
}
} // namespace portals
