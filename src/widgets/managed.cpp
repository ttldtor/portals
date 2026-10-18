// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/detail/pss_parser.hpp>
#include <portals/detail/setup_failure.hpp>
#include <portals/managed.hpp>

#include <algorithm>
#include <exception>
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
namespace detail {
// Mutable runtime documents are admitted in two passes: shape before any linked index, then values.
std::expected<void, PssError> validateStyleDocument(const PssDocument &document) {
  const auto invalid = [](PssLocation location, std::string_view message) {
    return std::unexpected(detail::pssError(PssErrorCode::INVALID_VALUE, location, message));
  };

  if (document.originKind != PssOriginKind::PSS_BYTES && document.originKind != PssOriginKind::SYNTHETIC_TYPED) {
    return invalid({}, "Invalid PSS document origin kind");
  }

  if (document.sourceName.size() > MAX_PSS_SOURCE_NAME_BYTES) {
    return std::unexpected(detail::pssError(PssErrorCode::SOURCE_NAME_LIMIT, {}, "PSS source identity exceeds budget"));
  }

  auto utf8 = detail::validatePssUtf8(document.sourceName);

  if (!utf8) {
    return utf8;
  }

  if (document.sheet.rules.size() > MAX_STYLE_RULES) {
    return std::unexpected(detail::pssError(PssErrorCode::RULE_LIMIT, {}, "Too many PSS document rules"));
  }

  if (document.origins.size() != document.sheet.rules.size()) {
    return invalid({}, "PSS document requires one origin per rule");
  }

  std::size_t ownedBytes{};
  const auto charge = [&](std::size_t bytes) {
    if (bytes > MAX_PSS_DOCUMENT_BYTES - ownedBytes) {
      return false;
    }

    ownedBytes += bytes;

    return true;
  };
  const auto chargeRecords = [&](std::size_t count, std::size_t size) {
    return count <= (MAX_PSS_DOCUMENT_BYTES - ownedBytes) / size && charge(count * size);
  };

  if (!charge(document.sourceName.size()) || !chargeRecords(document.sheet.rules.size(), sizeof(StyleRule)) ||
      !chargeRecords(document.origins.size(), sizeof(PssRuleOrigin)) ||
      !chargeRecords(document.declarations.size(), sizeof(PssDeclarationOrigin))) {
    return std::unexpected(detail::pssError(PssErrorCode::DOCUMENT_LIMIT, {}, "PSS document exceeds payload budget"));
  }

  const auto byteLocation = [](PssLocation location) {
    return location.offset <= MAX_PSS_BYTES && location.line > 0 && location.column > 0 &&
           location.line <= location.offset + 1 && location.column <= location.offset + 1;
  };
  const auto ordered = [](PssLocation previous, PssLocation next) {
    return previous.offset <= next.offset && previous.line <= next.line &&
           (previous.line != next.line || previous.column <= next.column);
  };
  std::size_t consumed{};
  PssLocation previous;

  for (std::size_t ruleIndex = 0; ruleIndex < document.origins.size(); ++ruleIndex) {
    const auto &origin = document.origins[ruleIndex];
    const auto &rule = document.sheet.rules[ruleIndex];
    const bool synthetic = document.originKind == PssOriginKind::SYNTHETIC_TYPED;

    if ((synthetic && origin.selector != PssLocation{ruleIndex, ruleIndex + 1, 1}) ||
        (!synthetic && (!byteLocation(origin.selector) || (ruleIndex && !ordered(previous, origin.selector))))) {
      return invalid(origin.selector, "Invalid PSS selector coordinates");
    }

    if (origin.declarationBegin != consumed || origin.declarationCount > document.declarations.size() - consumed) {
      return invalid(origin.selector, "Invalid PSS contiguous occurrence range");
    }

    const auto end = consumed + origin.declarationCount;

    for (const auto index : origin.effectiveDeclaration) {
      if (index != NO_PSS_DECLARATION && (index < consumed || index >= end)) {
        return invalid(origin.selector, "PSS effective declaration escapes its rule");
      }
    }

    if (rule.selector.state < StyleState::ANY || rule.selector.state > StyleState::CARET) {
      return invalid(origin.selector, "Invalid PSS selector state");
    }

    if (rule.declarations.minWidth && (*rule.declarations.minWidth < 0 || rule.selector.state != StyleState::ANY)) {
      return std::unexpected(
        detail::pssError(PssErrorCode::INVALID_WIDTH, origin.selector, "Invalid PSS document minimum width"));
    }

    if (!charge(rule.selector.type.size()) || !charge(rule.selector.id.size()) ||
        !chargeRecords(rule.selector.classes.size(), sizeof(std::string))) {
      return std::unexpected(
        detail::pssError(PssErrorCode::DOCUMENT_LIMIT, origin.selector, "PSS selectors exceed document budget"));
    }

    for (const auto &name : rule.selector.classes) {
      if (!charge(name.size())) {
        return std::unexpected(
          detail::pssError(PssErrorCode::DOCUMENT_LIMIT, origin.selector, "PSS classes exceed document budget"));
      }
    }

    previous = origin.selector;

    for (std::size_t index = consumed; index < end; ++index) {
      const auto &declaration = document.declarations[index];

      if (declaration.ruleIndex != ruleIndex || declaration.property < PssProperty::FOREGROUND ||
          declaration.property > PssProperty::MIN_WIDTH) {
        return invalid(declaration.location, "Invalid PSS declaration rule or property index");
      }

      const auto property = static_cast<std::size_t>(declaration.property);

      if ((synthetic && declaration.location != PssLocation{property, ruleIndex + 1, property + 1}) ||
          (!synthetic && (!byteLocation(declaration.location) || !ordered(previous, declaration.location)))) {
        return invalid(declaration.location, "Invalid PSS declaration coordinates");
      }

      previous = declaration.location;
    }

    consumed = end;
  }

  if (consumed != document.declarations.size()) {
    return invalid({}, "PSS document contains unclaimed declarations");
  }

  // Every enum, containment range and effective link is now safe to inspect.
  for (std::size_t ruleIndex = 0; ruleIndex < document.origins.size(); ++ruleIndex) {
    const auto &origin = document.origins[ruleIndex];
    const auto &rule = document.sheet.rules[ruleIndex];
    std::array<std::size_t, PSS_PROPERTY_COUNT> last;
    StyleDeclarations effective;

    last.fill(NO_PSS_DECLARATION);

    for (std::size_t index = origin.declarationBegin; index < origin.declarationBegin + origin.declarationCount;
         ++index) {
      const auto &declaration = document.declarations[index];
      const auto property = static_cast<std::size_t>(declaration.property);
      const auto &value = declaration.value;
      const bool color =
        declaration.property == PssProperty::FOREGROUND || declaration.property == PssProperty::BACKGROUND;
      const bool width = declaration.property == PssProperty::MIN_WIDTH;

      if ((!color && value.color != RgbColor{}) || ((color || width) && value.boolean) ||
          (!width && value.width != 0)) {
        return invalid(declaration.location, "Noncanonical PSS scalar fields");
      }

      if (width && (value.width < 0 || rule.selector.state != StyleState::ANY)) {
        return std::unexpected(
          detail::pssError(PssErrorCode::INVALID_WIDTH, declaration.location, "Invalid PSS declaration minimum width"));
      }

      last[property] = index;
      detail::assignPssDeclaration(declaration.property, value, effective);
    }

    if (last != origin.effectiveDeclaration || effective != rule.declarations) {
      return invalid(origin.selector, "PSS last-wins provenance disagrees with final declarations");
    }
  }

  return {};
}

} // namespace detail

namespace {
struct StyleMutationGuard {
  bool &active;

  explicit StyleMutationGuard(bool &value) : active(value) {
    active = true;
  }

  ~StyleMutationGuard() {
    active = false;
  }
};

bool retired(TreeError error) {
  return error == TreeError::STALE_NODE || error == TreeError::CLOSED;
}

TreeError bridge(const ManagedError &error) {
  if (const auto *tree = std::get_if<TreeError>(&error.cause)) {
    return *tree;
  }

  if (const auto *style = std::get_if<StyleError>(&error.cause)) {
    return style->cause;
  }

  return TreeError::CALLBACK_FAILED;
}

void retainStyleSource(ManagedError &error, const PssDocument &document) {
  // The style boundary emits TreeError or StyleError; never allocate fallback identity after NO_MEMORY.
  if (bridge(error) == TreeError::NO_MEMORY || document.sourceName.size() > MAX_PSS_SOURCE_NAME_BYTES) {
    return;
  }

  try {
    error.styleSource = std::make_shared<const std::string>(document.sourceName);
  } catch (const std::bad_alloc &) {
    // Retain the original cause even when the optional owning label cannot be allocated.
  }
}

void increment(std::size_t &count) noexcept {
  if (count != std::numeric_limits<std::size_t>::max()) {
    ++count;
  }
}

template <class T> std::size_t maxElementCount() noexcept {
  const auto differenceLimit = static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max()) / sizeof(T);
  const auto allocatorLimit = std::allocator_traits<std::allocator<T>>::max_size(std::allocator<T>{});

  return std::min(differenceLimit, allocatorLimit);
}

void copyLabel(std::string &target, const std::string &source, std::size_t &remaining, bool &truncated) {
  const auto count = std::min(source.size(), remaining);

  target.assign(source, 0, count);
  remaining -= count;
  truncated = truncated || count != source.size();
}

void copyContexts(ManagedDiagnostic &diagnostic, std::span<const ManagedDiagnosticContext> contexts,
                  ManagedDiagnosticOptions options) {
  const auto count = std::min(contexts.size(), options.maxContexts);
  auto remaining = options.maxContextBytes;

  diagnostic.contextsTruncated = count != contexts.size();
  diagnostic.contexts.reserve(count);

  for (const auto &source : contexts.first(count)) {
    auto &target = diagnostic.contexts.emplace_back();

    target.origin = source.origin;
    copyLabel(target.operation, source.operation, remaining, diagnostic.contextsTruncated);
    copyLabel(target.name, source.name, remaining, diagnostic.contextsTruncated);
    copyLabel(target.scope, source.scope, remaining, diagnostic.contextsTruncated);
    copyLabel(target.expectedType, source.expectedType, remaining, diagnostic.contextsTruncated);
  }
}
} // namespace

struct ManagedScene::KeyRecord {
  std::optional<KeyEvent> legacy;
  std::optional<Shortcut> shortcut;
  detail::ShortcutActions actions{detail::ShortcutActions::PRESS};
  std::function<EditorCommandResult(const KeyEvent &)> action;
  std::optional<std::source_location> origin;
};

struct ManagedScene::KeyPending {
  struct Candidate {
    std::size_t record{};
    std::size_t alternative{};
  };

  // Count construction keeps MSVC Debug iterator-proxy failures inside registration catches.
  std::vector<Candidate> candidates = std::vector<Candidate>(0);
  std::vector<Candidate> scratch = std::vector<Candidate>(0);
  Subscription timer;
  std::size_t depth{};
  std::uint64_t generation{1};
  std::uint64_t focusGeneration{};
  std::uint64_t inputGeneration{};
  SchedulerTime deadline{};
};

struct ManagedScene::KeySnapshot {
  std::vector<std::shared_ptr<KeyRecord>> records = std::vector<std::shared_ptr<KeyRecord>>(0);
  std::shared_ptr<std::function<std::expected<bool, ManagedError>(const InputEvent &)>> fallback;
  std::shared_ptr<KeyPending> pending;
};

ManagedScene::ManagedScene(Application &app, ManagedOptions options)
    : app_(app), owner_(std::this_thread::get_id()), options_(options), lifetime_(std::make_shared<const bool>(true)),
      styles_(app.tree(), options.maxNodes), entries_(0), properties_(0), models_(0),
      errors_(options.diagnostics.maxErrors) {
}

ManagedError ManagedScene::error(ManagedPhase phase, Node<Widget> node, TreeError cause) {
  return {phase, node.id(), cause, {}, {}};
}

std::expected<std::unique_ptr<ManagedScene>, ManagedError> ManagedScene::create(Application &app,
                                                                                ManagedOptions options) {
  auto allowed = app.tree().withScope(app.root(), [](const UiScope &) {
  });

  if (!allowed) {
    return std::unexpected(error(ManagedPhase::MOUNT, app.root(), allowed.error()));
  }

  const auto diagnostics = options.diagnostics;

  if (!options.maxNodes || !options.maxStates || !options.maxModels || !options.maxKeyHandlers ||
      !options.maxClickHandlers || !options.maxClickHandlersPerNode || !options.keySequenceTimeout ||
      !options.maxLayoutHandlers || !options.maxLayoutPublicationRetries || !options.maxObservers ||
      !options.maxObserversPerNode || !options.maxObserverRecoveryPasses || !diagnostics.maxErrors ||
      !diagnostics.maxContexts || !diagnostics.maxContextBytes ||
      diagnostics.maxErrors > maxElementCount<ManagedDiagnostic>() ||
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
      (recoveringObservers_ || observerCallbacks_ || observerMountPending())) {
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
  std::erase_if(entries_, [&](const auto &entry) {
    auto live = app_.tree().withScope(entry->node, [](const UiScope &) {
    });

    if (!live && retired(live.error())) {
      retireClicks(*entry);
      retireLayouts(*entry);

      return true;
    }

    return false;
  });
  pruneObservers();
  stylesDirty_ = true;

  return {};
}

void ManagedScene::pruneObservers() noexcept {
  struct Pruning {
    bool &flag;

    ~Pruning() {
      flag = false;
    }
  } guard{pruningObservers_};

  pruningObservers_ = true;
  std::erase_if(observers_, [&](const auto &record) {
    auto live = app_.tree().withScope(record->node, [](const UiScope &) {
    });
    if (!live && retired(live.error())) {
      record->retired = true;
      record->subscription = {};
    }
    return record->retired;
  });
}

std::expected<void, ManagedError> ManagedScene::observerAdmission(Node<Widget> node) {
  auto allowed = boundary(ManagedPhase::BIND, node);
  if (!allowed) {
    return allowed;
  }
  if (recoveringObservers_ || closing_) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::WOULD_BLOCK));
  }
  if (node != root_ && !find(node)) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::INVALID_ARGUMENT));
  }
  pruneObservers();
  const auto nodeCount = std::ranges::count(observers_, node, [](const auto &record) {
    return record->node;
  });
  if (observers_.size() >= options_.maxObservers ||
      static_cast<std::size_t>(nodeCount) >= options_.maxObserversPerNode) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NODE_LIMIT));
  }
  return {};
}

std::expected<void, TreeError> ManagedScene::invokeObserver(const std::shared_ptr<ObserverRecord> &record,
                                                            ManagedPhase phase) {
  if (closed_ || record->retired || record->running || observerMountPending() ||
      (phase == ManagedPhase::INPUT && recoveringObservers_)) {
    return {};
  }
  auto live = app_.tree().withScope(record->node, [](const UiScope &) {
  });
  if (!live && retired(live.error())) {
    record->retired = true;
    record->subscription = {};
    return {};
  }

  struct Invoking {
    ObserverRecord &record;
    std::size_t &count;

    ~Invoking() {
      record.running = false;
      --count;
    }
  } guard{*record, observerCallbacks_};

  record->running = true;
  ++observerCallbacks_;
  record->shortFailure.reset();
  record->shortAttempt = nullptr;
  record->promotesFailure = false;

  const auto report = [&](TreeError cause) -> std::expected<void, TreeError> {
    remember(error(phase, record->node, cause));
    return std::unexpected(cause);
  };
  const auto reportShort = [&]() -> std::expected<void, TreeError> {
    // Same-scene short collectors promoted their exact history receipt at synchronous admission.
    // A foreign collector has its own history; retain the unchanged failure in this runner too.
    if (!failure_) {
      remember(*record->shortFailure);
    }
    app_.requestStop();
    return std::unexpected(bridge(*record->shortFailure));
  };
  try {
    if (!live) {
      return report(live.error());
    }
    auto latest = record->revision();
    if (!latest) {
      return report(latest.error());
    }
    if (!record->initialPending && record->successful == *latest) {
      return {};
    }

    auto delivered = record->deliver(record);
    if (record->shortFailure) {
      return reportShort();
    }
    if (!delivered) {
      return report(delivered.error());
    }
    record->successful = *delivered;
    record->initialPending = false;
    return {};
  } catch (const std::bad_alloc &) {
    return record->shortFailure ? reportShort() : report(TreeError::NO_MEMORY);
  } catch (...) {
    return record->shortFailure ? reportShort() : report(TreeError::CALLBACK_FAILED);
  }
}

std::expected<void, TreeError> ManagedScene::recoverObservers() {
  if (observers_.empty()) {
    return {};
  }

  struct Recovering {
    bool &flag;

    ~Recovering() {
      flag = false;
    }
  } guard{recoveringObservers_};

  recoveringObservers_ = true;

  try {
    pruneObservers();
    if (observers_.empty()) {
      return {};
    }
    // Immutable owners survive immediate registry erase and deferred capture retirement.
    const auto snapshot = observers_;
    for (std::size_t pass = 0; pass < options_.maxObserverRecoveryPasses; ++pass) {
      for (const auto &record : snapshot) {
        auto result = invokeObserver(record, ManagedPhase::PREPARE);
        if (!result) {
          return result;
        }
      }

      bool pending{};
      for (const auto &record : snapshot) {
        if (record->retired) {
          continue;
        }
        auto current = record->revision();
        if (!current) {
          remember(error(ManagedPhase::PREPARE, record->node, current.error()));
          return std::unexpected(current.error());
        }
        pending = pending || record->initialPending || record->successful != *current;
      }
      if (!pending) {
        return {};
      }
    }
    remember(error(ManagedPhase::PREPARE, root_, TreeError::WOULD_BLOCK));
    return std::unexpected(TreeError::WOULD_BLOCK);
  } catch (const std::bad_alloc &) {
    remember(error(ManagedPhase::PREPARE, root_, TreeError::NO_MEMORY));
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::optional<std::size_t> ManagedScene::recordError(ManagedError failure,
                                                     std::span<const ManagedDiagnosticContext> contexts,
                                                     std::source_location origin, bool *observerInvoked,
                                                     bool runFailure, const ui::detail::SetupFailureTicket *shortTicket,
                                                     bool contextUnavailable) noexcept {
  if (observerInvoked) {
    *observerInvoked = false;
  }

  if (contextUnavailable) {
    increment(contextFailures_);
  }

  if (errorCount_ == errors_.size()) {
    increment(droppedErrors_);

    if (reporting_) {
      increment(suppressedNotifications_);

      return {};
    }

    // Reuse storage created with the scene; nested overflow must not overwrite this active borrow.
    overflowDiagnostic_.error = std::move(failure);
    overflowDiagnostic_.origin = origin;
    overflowDiagnostic_.storageOverflow = true;
    overflowDiagnostic_.contextsTruncated = contextUnavailable || !contexts.empty();
    overflowDiagnostic_.contextUnavailable = contextUnavailable;
    overflowDiagnostic_.notificationSuppressed = false;
    overflowDiagnostic_.observerFailed = false;
    const bool invoked = notifyError(overflowDiagnostic_, runFailure, shortTicket);

    if (observerInvoked) {
      *observerInvoked = invoked;
    }

    return {};
  }

  const auto index = errorCount_++;

  auto &diagnostic = errors_[index];

  // Entries and their empty context containers were constructed before mounting the scene.
  diagnostic.error = std::move(failure);
  diagnostic.origin = origin;
  diagnostic.contextUnavailable = contextUnavailable;

  try {
    if (contextUnavailable) {
      diagnostic.contextsTruncated = true;
    } else {
      copyContexts(diagnostic, contexts, options_.diagnostics);
    }
  } catch (const std::bad_alloc &) {
    diagnostic.contexts.clear();
    diagnostic.contextUnavailable = true;
    diagnostic.contextsTruncated = !contexts.empty();
    increment(contextFailures_);
  }

  const bool invoked = notifyError(diagnostic, runFailure, shortTicket);

  if (observerInvoked) {
    *observerInvoked = invoked;
  }

  return index;
}

bool ManagedScene::notifyError(ManagedDiagnostic &diagnostic, bool runFailure,
                               const ui::detail::SetupFailureTicket *shortTicket) noexcept {
  ui::detail::CurrentSceneScope mask;
  if (!errorObserver_) {
    return false;
  }

  if (reporting_) {
    diagnostic.notificationSuppressed = true;
    increment(suppressedNotifications_);

    return false;
  }

  struct Reporting {
    bool &flag;
    bool &runFlag;
    bool previousRunFlag;

    ~Reporting() {
      flag = false;
      runFlag = previousRunFlag;
    }
  } guard{reporting_, notifyingRunFailure_, notifyingRunFailure_};

  auto callback = errorObserver_;

  reporting_ = true;
  notifyingRunFailure_ = runFailure;

  if (shortTicket) {
    shortTicket->notifying();
  }

  try {
    (*callback)(diagnostic);
  } catch (...) {
    diagnostic.observerFailed = true;
    increment(observerFailures_);
  }

  return true;
}

std::expected<void, TreeError> ManagedScene::reportError(ManagedError failure,
                                                         std::span<const ManagedDiagnosticContext> contexts,
                                                         std::source_location origin) noexcept {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(TreeError::WRONG_THREAD);
  }

  if (closed_) {
    return std::unexpected(TreeError::CLOSED);
  }

  (void)recordError(std::move(failure), contexts, origin);

  return {};
}

void ManagedScene::reportShortError(ManagedError &failure, const char *operation, std::string_view name,
                                    std::string_view expectedType, std::source_location origin) noexcept {
  failure.operation = operation;
  failure.operationOrigin = origin;

  if (owner_ != std::this_thread::get_id() || closed_) {
    const auto ticket = ui::detail::SetupFailureScope::latch(failure);

    return;
  }

  // Admission commits to collector entry. No allocation or user callback precedes this receipt and latch.
  const std::byte attempt{};
  failure.reportReceipt.collector_ = lifetime_;
  const auto ticket = ui::detail::SetupFailureScope::latch(failure, &attempt);

  try {
    const ManagedDiagnosticContext context{
      std::string(operation), std::string(name), {}, std::string(expectedType), origin};

    (void)recordError(failure, std::span(&context, 1), origin, nullptr, false, &ticket);
  } catch (const std::bad_alloc &) {
    (void)recordError(failure, {}, origin, nullptr, false, &ticket, true);
  } catch (const std::length_error &) {
    (void)recordError(failure, {}, origin, nullptr, false, &ticket, true);
  }
}

std::expected<void, ManagedError> ManagedScene::onError(ManagedErrorHandler callback) {
  ui::detail::SetupFailureScope mask({}, nullptr, nullptr);
  ManagedErrorHandler incoming;

  incoming.swap(callback);

  auto allowed = boundary(ManagedPhase::BIND, root_);

  if (!allowed) {
    return allowed;
  }

  try {
    auto replacement =
      incoming
        ? std::make_shared<ManagedErrorHandler>(ui::detail::currentSceneCallback(facadeContext(), std::move(incoming)))
        : nullptr;

    errorObserver_ = std::move(replacement);
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, root_, TreeError::NO_MEMORY));
  }

  return {};
}

std::expected<ManagedDiagnosticBatch, TreeError> ManagedScene::diagnostics() const {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(TreeError::WRONG_THREAD);
  }

  try {
    ManagedDiagnosticBatch batch{std::vector<ManagedDiagnostic>(errors_.begin(), errors_.begin() + errorCount_),
                                 droppedErrors_,
                                 contextFailures_,
                                 observerFailures_,
                                 suppressedNotifications_,
                                 closed_};

    // Copy into expected directly: MSVC Debug's noexcept vector move may allocate a proxy.
    return std::expected<ManagedDiagnosticBatch, TreeError>(std::in_place, batch);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<void, TreeError> ManagedScene::clearErrors() noexcept {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(TreeError::WRONG_THREAD);
  }

  if (reporting_) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  for (auto &diagnostic : std::span(errors_.data(), errorCount_)) {
    diagnostic.contexts.clear();
    diagnostic.contextsTruncated = false;
    diagnostic.contextUnavailable = false;
    diagnostic.storageOverflow = false;
    diagnostic.notificationSuppressed = false;
    diagnostic.observerFailed = false;
  }

  errorCount_ = 0;
  droppedErrors_ = 0;
  contextFailures_ = 0;
  observerFailures_ = 0;
  suppressedNotifications_ = 0;
  failureDiagnostic_.reset();

  return {};
}

std::expected<ManagedDiagnosticBatch, TreeError> ManagedScene::drainErrors() {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(TreeError::WRONG_THREAD);
  }

  if (reporting_) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  try {
    ManagedDiagnosticBatch batch{std::vector<ManagedDiagnostic>(errors_.begin(), errors_.begin() + errorCount_),
                                 droppedErrors_,
                                 contextFailures_,
                                 observerFailures_,
                                 suppressedNotifications_,
                                 closed_};

    struct ClearOnSuccess {
      ManagedScene &scene;
      int exceptions{std::uncaught_exceptions()};

      ~ClearOnSuccess() {
        if (std::uncaught_exceptions() == exceptions) {
          (void)scene.clearErrors();
        }
      }
    } guard{*this};

    return std::expected<ManagedDiagnosticBatch, TreeError>(std::in_place, batch);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

void ManagedScene::remember(ManagedError failure, std::source_location origin,
                            std::span<const ManagedDiagnosticContext> contexts) {
  const bool first = !failure_;

  if (first) {
    failure_ = failure;
  }

  bool invoked{};
  const auto diagnostic = recordError(std::move(failure), contexts, origin, &invoked, first);

  if (first) {
    failureDiagnostic_ = diagnostic;
    failureNotified_ = invoked;
  }

  app_.requestStop();
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

std::expected<void, TreeError> ManagedScene::afterDispatch(Application &app) {
  if (&app != &app_ || owner_ != std::this_thread::get_id()) {
    return std::unexpected(owner_ != std::this_thread::get_id() ? TreeError::WRONG_THREAD
                                                                : TreeError::INVALID_ARGUMENT);
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
  return {};
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
    return {};
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
    auto bound = bindCheckbox(
      app_.tree(), app_.router(), node, *property,
      [this, node, changed = ui::detail::currentSceneCallback(facadeContext(), std::move(incomingChanged))](
        CheckBoxResult receipt) {
        try {
          if (!receipt) {
            remember(error(ManagedPhase::INPUT, node, receipt.error()));
          }

          if (changed) {
            auto result = changed(std::move(receipt));

            if (!result) {
              remember(error(ManagedPhase::INPUT, node, result.error()));
            }
          }
        } catch (const std::bad_alloc &) {
          remember(error(ManagedPhase::INPUT, node, TreeError::NO_MEMORY));
        } catch (...) {
          remember(error(ManagedPhase::INPUT, node, TreeError::CALLBACK_FAILED));
        }
      });

    if (!bound) {
      return std::unexpected(error(ManagedPhase::BIND, node, bound.error()));
    }

    (*entry)->binding.emplace<CheckBoxBinding>(std::move(*bound));

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::CALLBACK_FAILED));
  }
}

std::expected<void, ManagedError> ManagedScene::syncCaption(Node<Control> node, CaptionBinding &binding,
                                                            ManagedPhase phase) try {
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

  try {
    incomingModel = std::make_unique<ManagedState<std::string>>(model);
    feedback = std::make_shared<detail::EditorCommandFeedback>();
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
  }

  try {
    auto bound = bindLineEdit(
      app_.tree(), app_.router(), node, *property,
      [this, node, lifetime = std::weak_ptr(lifetime_), feedback,
       result = ui::detail::currentSceneCallback(facadeContext(), std::move(incomingResult))](LineEditResult receipt) {
        const auto report = [this, node, &lifetime, feedback, trigger = receipt.trigger](TreeError cause,
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
          failure.operationOrigin = origin;
          failure.reportReceipt.collector_ = lifetime_;

          auto *destination = FocusReport::destination(app_);

          if (commandFailure && trigger == LineEditTrigger::BLUR && destination && !*destination) {
            // Only this Application's failed loss receipt returns through the active focus call.
            // Independent result failures and other Applications never populate its destination.
            destination->emplace(failure);
          }

          try {
            const ManagedDiagnosticContext context{std::string(operation), std::string(0, '\0'), std::string(0, '\0'),
                                                   std::string("LineEdit"), origin};

            remember(failure, origin, std::span(&context, 1));
          } catch (const std::bad_alloc &) {
            remember(failure, origin);
          } catch (const std::length_error &) {
            remember(failure, origin);
          }
        };

        try {
          if (receipt.status == LineEditStatus::FAILED && receipt.error) {
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
                       : std::function<bool(std::string_view)>{});

    if (!bound) {
      return std::unexpected(error(ManagedPhase::BIND, node, bound.error()));
    }

    bound->commandFeedback(std::move(feedback));
    (*entry)->binding.emplace<LineEditBinding>(std::move(*bound));
    (*entry)->textModel = std::move(incomingModel);

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(error(ManagedPhase::BIND, node, TreeError::CALLBACK_FAILED));
  }
}

std::expected<void, ManagedError> ManagedScene::style(Node<Widget> node, StyleTarget target) {
  auto allowed = boundary(ManagedPhase::STYLE, node);

  if (!allowed) {
    return allowed;
  }

  auto entry = find(node);

  if (!entry) {
    return std::unexpected(error(ManagedPhase::STYLE, node, TreeError::INVALID_ARGUMENT));
  }

  try {
    auto prepared = std::make_shared<StyleTarget>(target);
    auto dirty = app_.tree().invalidate(node, Invalidation::PAINT);

    if (!dirty) {
      return std::unexpected(error(ManagedPhase::STYLE, node, dirty.error()));
    }

    // Publish only the prepared owner: checked string/container moves can allocate on MSVC.
    entry->target = std::move(prepared);
    entry->stylePending = true;
    stylesDirty_ = true;

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::STYLE, node, TreeError::NO_MEMORY));
  }
}

std::expected<void, ManagedError> ManagedScene::id(Node<Widget> node, std::string_view value) {
  auto allowed = boundary(ManagedPhase::STYLE, node);

  if (!allowed) {
    return allowed;
  }

  auto entry = find(node);

  if (!entry) {
    return std::unexpected(error(ManagedPhase::STYLE, node, TreeError::INVALID_ARGUMENT));
  }

  try {
    auto target = entry->target ? *entry->target : StyleTarget{};

    target.id = value;

    // Unlike style(), an explicit empty id must clear the previous identity.
    auto prepared = std::make_shared<StyleTarget>(target);
    auto dirty = app_.tree().invalidate(node, Invalidation::PAINT);

    if (!dirty) {
      return std::unexpected(error(ManagedPhase::STYLE, node, dirty.error()));
    }

    entry->target = std::move(prepared);
    entry->stylePending = true;
    stylesDirty_ = true;

    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::STYLE, node, TreeError::NO_MEMORY));
  }
}

std::expected<void, ManagedError>
ManagedScene::pendingStyles(std::optional<std::vector<StyleRegistration>> &destination) {
  try {
    if (!stylesDirty_) {
      return {};
    }
    destination.emplace(0);
    auto &registrations = *destination;
    registrations.reserve(entries_.size());
    for (const auto &entry : entries_) {
      auto live = app_.tree().read(entry->node, [](const Widget &) {
      });
      if (!live) {
        if (retired(live.error())) {
          continue;
        }
        return std::unexpected(error(ManagedPhase::STYLE, entry->node, live.error()));
      }
      if (entry->target && entry->stylePending) {
        registrations.push_back({entry->node, *entry->target});
      }
    }
    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::STYLE, root_, TreeError::NO_MEMORY));
  }
}

void ManagedScene::publishStyleMetadata() noexcept {
  for (const auto &entry : entries_) {
    entry->stylePending = false;
  }
  stylesDirty_ = false;
}

std::expected<void, ManagedError> ManagedScene::flushStyles() {
  if (!stylesDirty_) {
    return {};
  }
  std::optional<std::vector<StyleRegistration>> registrations;
  auto staged = pendingStyles(registrations);
  if (!staged) {
    return std::unexpected(staged.error());
  }
  auto installed = styles_.bindBatch(*registrations);
  if (!installed) {
    return std::unexpected(ManagedError{ManagedPhase::STYLE, root_.id(), installed.error(), {}, {}});
  }
  publishStyleMetadata();
  return {};
}

std::expected<Invalidation, ManagedError>
ManagedScene::mutateStyles(StyleEngine::Mutation mutation, StyleLayerOptions options, StyleTier tier,
                           std::string_view name, const StyleSheet *sheet, std::span<const StyleLayerUpdate> updates,
                           StyleLayerHandle *added, const detail::StyleThemeLease *lease, const PssDocument *document,
                           std::optional<StyleLayerOptions> admissionOptions) {
  auto allowed = boundary(ManagedPhase::STYLE, root_);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  StyleMutationGuard guard(styling_);

  if (document) {
    auto valid = detail::validateStyleDocument(*document);

    if (!valid) {
      const auto &pss = valid.error();
      ManagedError failure{ManagedPhase::STYLE,
                           root_.id(),
                           StyleError{pss.cause, pss.location.line, pss.location.column, pss.message},
                           {},
                           {}};

      failure.pss = pss;
      retainStyleSource(failure, *document);

      return std::unexpected(failure);
    }
  }

  // Pending registrations and layer declarations share the engine's sole publication boundary.
  std::optional<std::vector<StyleRegistration>> registrations;
  auto staged = pendingStyles(registrations);

  if (!staged) {
    return std::unexpected(staged.error());
  }

  // A scoped sheet can style an otherwise unregistered layout/custom widget. Stage its
  // metadata together with the candidate layer, and publish ownership only after commit.
  std::shared_ptr<Entry> scopedEntry;
  std::shared_ptr<StyleTarget> scopedTarget;

  if (sheet && sheet->scope) {
    const auto found = std::ranges::find_if(entries_, [&](const auto &entry) {
      return entry->node.id() == *sheet->scope && !entry->removed;
    });

    if (found == entries_.end()) {
      const auto cause = sheet->scope->session != root_.id().session ? TreeError::WRONG_SESSION : TreeError::STALE_NODE;

      return std::unexpected(ManagedError{ManagedPhase::STYLE, *sheet->scope, cause, {}, {}});
    }

    auto live = app_.tree().withScope((*found)->node, [](const UiScope &) {
    });

    if (!live) {
      return std::unexpected(error(ManagedPhase::STYLE, (*found)->node, live.error()));
    }

    if (!(*found)->target) {
      try {
        scopedEntry = *found;
        scopedTarget = std::make_shared<StyleTarget>();

        if (!registrations) {
          registrations.emplace(0);
        }

        registrations->push_back({scopedEntry->node, *scopedTarget});
      } catch (const std::bad_alloc &) {
        return std::unexpected(error(ManagedPhase::STYLE, scopedEntry->node, TreeError::NO_MEMORY));
      }
    }
  }

  auto installed = styles_.mutate(mutation, options, tier, name, sheet, updates,
                                  registrations ? std::span<const StyleRegistration>(*registrations)
                                                : std::span<const StyleRegistration>{},
                                  added, lease, document, admissionOptions);

  if (!installed) {
    ManagedError failure{ManagedPhase::STYLE, root_.id(), installed.error(), {}, {}};

    if (document) {
      if (installed.error().cause == TreeError::NO_MEMORY) {
        auto pss = detail::pssError(PssErrorCode::ALLOCATION, {}, "Style document allocation failed");

        pss.cause = TreeError::NO_MEMORY;
        failure.pss = pss;
      } else {
        retainStyleSource(failure, *document);
      }
    }

    return std::unexpected(failure);
  }

  if (scopedEntry) {
    scopedEntry->target = std::move(scopedTarget);
  }

  publishStyleMetadata();

  return *installed;
}

std::expected<StyleLayerHandle, ManagedError> ManagedScene::styleDocument(StyleTier tier, std::string_view name,
                                                                          const PssDocument &document,
                                                                          std::optional<StyleLayerOptions> options) {
  StyleLayerHandle admitted;
  auto installed = mutateStyles(StyleEngine::Mutation::ADMIT, {}, tier, name, &document.sheet, {}, &admitted, nullptr,
                                &document, options);

  if (!installed) {
    return std::unexpected(installed.error());
  }

  return admitted;
}

std::expected<std::vector<StyleLayerSource>, ManagedError> ManagedScene::styleSources() const {
  auto allowed = boundary(ManagedPhase::STYLE, root_);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  StyleMutationGuard guard(styling_);

  try {
    std::vector<StyleLayerSource> sources(0);

    sources.reserve(styles_.layerState_.layers.size());

    for (const auto &layer : styles_.layerState_.layers) {
      sources.emplace_back();

      auto &source = sources.back();

      source.handle = styles_.handle(layer.id, styles_.layerState_.epoch);
      source.tier = layer.tier;
      source.name.assign(layer.name);
      source.document = layer.document;
    }

    // MSVC Debug vector moves can allocate inside noexcept: copy directly into the checked result.
    return std::expected<std::vector<StyleLayerSource>, ManagedError>(std::in_place, sources);
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::STYLE, root_, TreeError::NO_MEMORY));
  }
}

std::expected<void, ManagedError> ManagedScene::validateStyleLayer(StyleLayerHandle handle) const {
  auto allowed = boundary(ManagedPhase::STYLE, root_);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  auto valid = styles_.validateHandle(handle);

  if (!valid) {
    return std::unexpected(ManagedError{ManagedPhase::STYLE, root_.id(), valid.error(), {}, {}});
  }

  return {};
}

std::expected<void, ManagedError> ManagedScene::enableStyleLayers(StyleLayerOptions options) {
  auto installed = mutateStyles(StyleEngine::Mutation::ENABLE, options, {}, {}, nullptr, {});
  if (!installed) {
    return std::unexpected(installed.error());
  }
  return {};
}

std::expected<StyleLayerHandle, ManagedError> ManagedScene::addLayer(StyleTier tier, std::string_view name,
                                                                     const StyleSheet &sheet) {
  StyleLayerHandle added;
  auto installed = mutateStyles(StyleEngine::Mutation::ADD, {}, tier, name, &sheet, {}, &added);
  if (!installed) {
    return std::unexpected(installed.error());
  }
  return added;
}

std::expected<Invalidation, ManagedError> ManagedScene::replaceLayer(StyleLayerHandle value, const StyleSheet &sheet) {
  return mutateStyles(StyleEngine::Mutation::UPDATE, {}, {}, {}, &sheet, {}, &value);
}

std::expected<Invalidation, ManagedError> ManagedScene::removeLayer(StyleLayerHandle value) {
  const StyleLayerUpdate update{value, std::nullopt};
  return updateLayers(std::span(&update, 1));
}

std::expected<Invalidation, ManagedError> ManagedScene::updateLayers(std::span<const StyleLayerUpdate> updates) {
  return mutateStyles(StyleEngine::Mutation::UPDATE, {}, {}, {}, nullptr, updates);
}

std::expected<StyleLayerHandle, ManagedError> ManagedScene::baseLayer(StyleTier tier) const {
  auto allowed = boundary(ManagedPhase::STYLE, root_);
  if (!allowed) {
    return std::unexpected(allowed.error());
  }
  auto found = styles_.baseLayer(tier);
  if (!found) {
    return std::unexpected(ManagedError{ManagedPhase::STYLE, root_.id(), found.error(), {}, {}});
  }
  return *found;
}

std::expected<Invalidation, ManagedError> ManagedScene::styleDefaults(const StyleSheet &sheet) {
  return mutateStyles(StyleEngine::Mutation::DEFAULTS, {}, {}, {}, &sheet, {});
}

detail::StyleThemeLease ManagedScene::captureThemeLease() const noexcept {
  return styles_.themeLease();
}

std::expected<void, ManagedError> ManagedScene::checkThemeLease(detail::StyleThemeLease lease) const {
  auto allowed = boundary(ManagedPhase::STYLE, root_);
  if (!allowed) {
    return allowed;
  }
  auto valid = styles_.validateLease(lease);
  if (!valid) {
    return std::unexpected(ManagedError{ManagedPhase::STYLE, root_.id(), valid.error(), {}, {}});
  }
  return {};
}

std::expected<detail::StyleThemeLease, ManagedError> ManagedScene::attachTheme(const StyleSheet &sheet,
                                                                               detail::StyleThemeLease lease) {
  auto allowed = checkThemeLease(lease);
  if (!allowed) {
    return std::unexpected(allowed.error());
  }
  if (lease.layered) {
    auto installed = mutateStyles(StyleEngine::Mutation::THEME, {}, {}, {}, &sheet, {}, nullptr, &lease);
    if (!installed) {
      return std::unexpected(installed.error());
    }
  } else {
    auto installed = applyLegacyTheme(sheet, lease);
    if (!installed) {
      return std::unexpected(installed.error());
    }
  }
  return captureThemeLease();
}

std::expected<void, ManagedError> ManagedScene::applyLegacyTheme(const StyleSheet &sheet,
                                                                 detail::StyleThemeLease lease) {
  auto allowed = checkThemeLease(lease);
  if (!allowed) {
    return allowed;
  }

  struct Guard {
    bool &active;

    explicit Guard(bool &value) : active(value) {
      active = true;
    }

    ~Guard() {
      active = false;
    }
  } guard(styling_);

  auto flushed = flushStyles();
  if (!flushed) {
    return flushed;
  }
  auto installed = styles_.replaceOwnedTheme(sheet, lease);
  if (!installed) {
    return std::unexpected(ManagedError{ManagedPhase::STYLE, root_.id(), installed.error(), {}, {}});
  }
  return {};
}

std::expected<void, ManagedError> ManagedScene::applyTheme(const StyleSheet &sheet, detail::StyleThemeLease lease) {
  auto allowed = checkThemeLease(lease);
  if (!allowed) {
    return allowed;
  }
  if (!lease.layered) {
    return applyLegacyTheme(sheet, lease);
  }
  auto installed = mutateStyles(StyleEngine::Mutation::OWNED_THEME, {}, {}, {}, &sheet, {}, nullptr, &lease);
  if (!installed) {
    return std::unexpected(installed.error());
  }
  return {};
}

std::expected<void, ManagedError> ManagedScene::theme(StyleSheet sheet) {
  auto allowed = boundary(ManagedPhase::STYLE, root_);

  if (!allowed) {
    return allowed;
  }

  if (styles_.layerState_.enabled) {
    auto installed = mutateStyles(StyleEngine::Mutation::THEME, {}, {}, {}, &sheet, {});
    if (!installed) {
      return std::unexpected(installed.error());
    }
    return {};
  }

  StyleMutationGuard guard(styling_);

  auto flushed = flushStyles();

  if (!flushed) {
    return flushed;
  }

  auto installed = styles_.replace(std::move(sheet));

  if (!installed) {
    return std::unexpected(ManagedError{ManagedPhase::STYLE, root_.id(), installed.error(), {}, {}});
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
    const auto command = record->action(event);

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

  auto recovered = recoverObservers();
  if (!recovered) {
    return recovered;
  }

  struct Preparing {
    bool &flag;

    ~Preparing() {
      flag = false;
    }
  } guard{preparing_};

  preparing_ = true;

  try {
    // The preparing guard rejects registry changes; prune only after every active borrow ends.
    struct Prune {
      ManagedScene &scene;

      ~Prune() {
        std::erase_if(scene.entries_, [&](const auto &entry) {
          if (entry->removed) {
            scene.retireClicks(*entry);
            scene.retireLayouts(*entry);
          }

          return entry->removed;
        });
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

    for (const auto &entry : entries_) {
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
        } else if (auto *check = std::get_if<CheckBoxBinding>(&entry->binding)) {
          status = check->sync();
        }
      }

      if (status && entry->sync) {
        status = entry->sync(*focused);
      }

      if (status && entry->customSync) {
        status = entry->customSync(*focused);
      }

      if (!status) {
        // Retirement cannot erase an explicit error from an admitted callback.
        remember(error(ManagedPhase::PREPARE, entry->node, status.error()));

        return status;
      }
    }

    for (const auto &source : models_) {
      auto pinned = source->pin();

      if (!pinned) {
        remember(error(ManagedPhase::PREPARE, root_, pinned.error()));

        return pinned;
      }
    }

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
  return std::ranges::any_of(entries_, [](const auto &entry) {
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

  return {};
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

  if (preparing_ || reporting_ || styling_ || closing_ || recoveringObservers_ || pruningObservers_ ||
      observerCallbacks_ || (sessionActive_ && !stoppingSession_)) {
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

  for (const auto &record : observers_) {
    record->retired = true;
    record->subscription = {};
  }
  observers_.clear();

  for (auto &layer : styles_.layerState_.layers) {
    layer.document.reset();
  }

  for (auto &entry : entries_) {
    retireClicks(*entry);
    retireLayouts(*entry);
  }

  entries_.clear();
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
    } else {
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
