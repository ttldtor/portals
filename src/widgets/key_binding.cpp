// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "key_binding_state.hpp"
#include <algorithm>
#include <limits>

namespace portals {
namespace {
ManagedError keyError(TreeError cause, NodeId node = {}) {
  return {ManagedPhase::BIND, node, cause, {}, {}};
}

bool connected(const std::weak_ptr<detail::ActionCancellation> &weak) {
  auto state = weak.lock();
  return state && state->connected.load();
}

bool hasPhase(detail::ShortcutActions actions, KeyAction phase) {
  const auto mask = phase == KeyAction::PRESS ? 1u : phase == KeyAction::REPEAT ? 2u : 4u;
  return (static_cast<unsigned>(actions) & mask) != 0;
}

KeySupport support(KeyCapabilities value, KeyAction phase) {
  return phase == KeyAction::PRESS ? value.press : phase == KeyAction::REPEAT ? value.repeat : value.release;
}

bool sameShortcutValue(const Shortcut &a, const Shortcut &b) {
  const auto count = a.alternatives.size() + a.sequences.size();
  if (count != b.alternatives.size() + b.sequences.size()) {
    return false;
  }
  for (std::size_t i{}; i < count; ++i) {
    auto first = detail::shortcutAlternative(a, i);
    auto second = detail::shortcutAlternative(b, i);
    if (first.size() != second.size()) {
      return false;
    }
    for (std::size_t j{}; j < first.size(); ++j) {
      const KeyEvent event{second[j].key, second[j].scalar, second[j].modifiers, KeyAction::PRESS};
      if (!detail::shortcutChordMatches(first[j], event)) {
        return false;
      }
    }
  }
  return true;
}

bool sameShortcut(const std::optional<Shortcut> &a, const std::optional<Shortcut> &b) {
  return a.has_value() == b.has_value() && (!a || sameShortcutValue(*a, *b));
}
} // namespace

std::expected<ManagedScene *, ManagedError> KeyBinding::scene() const {
  if (owner_ != std::thread::id{} && owner_ != std::this_thread::get_id()) {
    return std::unexpected(keyError(TreeError::WRONG_THREAD));
  }
  if (lifetime_.expired()) {
    return std::unexpected(keyError(TreeError::CLOSED));
  }
  if (scene_->closed_) {
    return std::unexpected(keyError(TreeError::CLOSED));
  }
  return scene_;
}

std::expected<void, ManagedError> KeyBinding::setEnabled(bool value) const {
  auto owner = scene();
  return owner ? (*owner)->changeKey(id_, ManagedScene::KeyChange::ENABLED, value) : std::unexpected(owner.error());
}

std::expected<void, ManagedError> KeyBinding::setVisible(bool value) const {
  auto owner = scene();
  return owner ? (*owner)->changeKey(id_, ManagedScene::KeyChange::VISIBLE, value) : std::unexpected(owner.error());
}

std::expected<void, ManagedError> KeyBinding::setText(TextValue label, TextValue description,
                                                      KeyTextResolver resolver) const {
  auto owner = scene();
  if (!owner) {
    return std::unexpected(owner.error());
  }
  try {
    KeyInfo info(std::move(label));
    info.description = std::move(description);
    info.resolver = std::move(resolver);
    return (*owner)->changeKey(id_, ManagedScene::KeyChange::TEXT, false, nullptr, &info);
  } catch (const std::bad_alloc &) {
    return std::unexpected(keyError(TreeError::NO_MEMORY));
  }
}

std::expected<void, ManagedError> KeyBinding::rebind(const KeySelection &selection) const {
  auto owner = scene();
  return owner ? (*owner)->changeKey(id_, ManagedScene::KeyChange::REBIND, false, &selection)
               : std::unexpected(owner.error());
}

std::expected<void, ManagedError> KeyBinding::unbind() const {
  auto owner = scene();
  return owner ? (*owner)->changeKey(id_, ManagedScene::KeyChange::UNBIND) : std::unexpected(owner.error());
}

std::expected<void, ManagedError> KeyBinding::remove() const {
  auto owner = scene();
  return owner ? (*owner)->changeKey(id_, ManagedScene::KeyChange::REMOVE) : std::unexpected(owner.error());
}

std::expected<GraphemeText, ManagedError> ManagedScene::keyText(const TextValue &value, const KeyTextResolver &resolver,
                                                                std::size_t limit, bool label) {
  if (value.bytes_.size() > limit) {
    return std::unexpected(keyError(TreeError::NODE_LIMIT));
  }
  TextLimits limits{limit, limit, limit, limit};
  auto text = resolver ? value.resolve(widthPolicy(), resolver, limits) : value.resolve(widthPolicy(), limits);
  if (!text) {
    auto failure = keyError(TreeError::INVALID_ARGUMENT);
    if (auto error = std::get_if<TextError>(&text.error())) {
      failure.text = *error;
      if (error->kind == TextErrorKind::INPUT_LIMIT || error->kind == TextErrorKind::CLUSTER_LIMIT ||
          error->kind == TextErrorKind::GRAPHEME_LIMIT || error->kind == TextErrorKind::CELL_LIMIT) {
        failure.cause = TreeError::NODE_LIMIT;
      }
    } else {
      failure.resource = std::get<TextResourceError>(text.error());
    }
    return std::unexpected(failure);
  }
  if (label && (text->bytes().empty() || text->bytes().find_first_of("\r\n") != std::string_view::npos)) {
    return std::unexpected(keyError(TreeError::INVALID_ARGUMENT));
  }
  return std::move(*text);
}

std::expected<KeyBinding, ManagedError>
ManagedScene::registerAnnotatedKey(const KeySelection &selection, KeyInfo info,
                                   std::function<EditorCommandResult(const KeyEvent &)> action,
                                   std::source_location origin, KeyTransaction &transaction) {
  const auto &shortcut = detail::KeySelectionAccess::shortcut(selection);
  if (!shortcut || !*shortcut) {
    auto failure = keyError(TreeError::INVALID_ARGUMENT);
    failure.shortcut = shortcut ? ShortcutError{ShortcutErrorKind::INVALID_CHORD, 0} : shortcut.error();
    failure.registrationOrigin = origin;
    return std::unexpected(failure);
  }
  if (!action || keyHandlers_ >= options_.maxKeyHandlers) {
    return std::unexpected(keyError(!action ? TreeError::INVALID_ARGUMENT : TreeError::NODE_LIMIT));
  }
  if (keySerial_ == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(keyError(TreeError::IDENTITY_EXHAUSTED));
  }
  if (info.publication_ && (info.publication_->owner != owner_ || info.publication_->binding)) {
    return std::unexpected(keyError(TreeError::INVALID_ARGUMENT));
  }
  const auto owner = info.owner == Node<Widget>{} ? root_ : info.owner;
  if (!app_.tree().inputParent(owner) || !app_.router().within(root_, owner)) {
    return std::unexpected(keyError(TreeError::STALE_NODE, owner.id()));
  }
  std::weak_ptr<detail::ActionCancellation> cancellation;
  if (info.scope != Node<Widget>{}) {
    const auto *scope = app_.router().scope(info.scope);
    if (!scope || !app_.router().within(root_, info.scope) || !scope->cancellation->connected.load() ||
        info.scopeIdentity_.owner_before(scope->cancellation) ||
        scope->cancellation.owner_before(info.scopeIdentity_)) {
      return std::unexpected(keyError(TreeError::INVALID_ARGUMENT, info.scope.id()));
    }
    cancellation = scope->cancellation;
  }
  auto label = keyText(info.label, info.resolver, options_.maxKeyLabelBytes, true);
  if (!label) {
    return std::unexpected(label.error());
  }
  auto description = keyText(info.description, info.resolver, options_.maxKeyDescriptionBytes, false);
  if (!description) {
    return std::unexpected(description.error());
  }
  auto metadata = std::make_shared<KeyRecord::Metadata>(std::move(*label), std::move(*description));
  metadata->id.scene_ = root_.id();
  metadata->id.serial_ = keySerial_ + 1;
  metadata->owner = owner;
  metadata->scope = info.scope;
  metadata->cancellation = cancellation;
  if (info.ownerLifetime_) {
    metadata->ownerLifetime = info.ownerLifetime_;
  }
  metadata->enabled = info.enabled;
  metadata->visible = info.visible;
  auto record = std::make_shared<KeyRecord>();
  record->shortcut = **shortcut;
  record->actions = detail::KeySelectionAccess::actions(selection);
  record->metadata = metadata;
  record->origin = origin;
  record->annotatedAction = std::make_shared<const decltype(action)>(std::move(action));
  auto snapshot = std::make_shared<KeySnapshot>();
  if (keys_) {
    snapshot->records = keys_->records;
    snapshot->fallback = keys_->fallback;
  } else {
    snapshot->fallback = input_;
  }
  snapshot->records.push_back(record);
  KeyBinding result;
  result.scene_ = this;
  result.lifetime_ = lifetime_;
  result.owner_ = owner_;
  result.id_ = metadata->id;
  auto published = publishKeyCandidate(snapshot, false, transaction, &result, info.publication_);
  if (!published) {
    return std::unexpected(published.error());
  }
  return result;
}

std::expected<void, ManagedError>
ManagedScene::publishKeyCandidate(std::shared_ptr<KeySnapshot> snapshot, bool preserve, KeyTransaction &transaction,
                                  KeyBinding *result, const std::shared_ptr<detail::KeyBindingSlot> &slot) {
  if (!keyRevision_ || keyRevision_ == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(keyError(TreeError::IDENTITY_EXHAUSTED));
  }
  snapshot->annotated = std::ranges::any_of(snapshot->records, [](const auto &record) {
    return bool(record->metadata);
  });
  snapshot->scopeConnected.emplace(0);
  snapshot->scopeConnected->reserve(snapshot->records.size());
  for (const auto &record : snapshot->records) {
    unsigned char observed = record->metadata ? record->metadata->observation() : 0;
    if (record->metadata && transaction.previous && transaction.previous->annotated) {
      for (std::size_t i{}; i < transaction.previous->records.size(); ++i) {
        const auto &previous = transaction.previous->records[i]->metadata;
        if (previous && previous->id == record->metadata->id) {
          observed = (*transaction.previous->scopeConnected)[i];
          break;
        }
      }
    }
    if (result && record->metadata && record->metadata->id == result->id_ &&
        record->metadata->scope != Node<Widget>{}) {
      observed |= KeyRecord::Metadata::SCOPE_CONNECTED;
    }
    snapshot->scopeConnected->push_back(observed);
  }
  if (preserve) {
    snapshot->pending = transaction.previous->pending;
  } else {
    snapshot->pending = std::make_shared<KeyPending>();
    std::size_t capacity{};
    for (const auto &record : snapshot->records) {
      const auto count = record->shortcut ? record->shortcut->sequences.size() : 0;
      if (count > std::numeric_limits<std::size_t>::max() - capacity) {
        return std::unexpected(keyError(TreeError::NODE_LIMIT));
      }
      capacity += count;
    }
    snapshot->pending->candidates.reserve(capacity);
    snapshot->pending->scratch.reserve(capacity);
  }
  using Input = std::function<std::expected<bool, ManagedError>(const InputEvent &)>;
  auto prepared = std::make_shared<Input>([this, snapshot](const InputEvent &event) {
    return dispatchKeys(snapshot, event);
  });
  // Last validation is callback-free and follows every staging allocation, including input storage.
  if (closed_) {
    return std::unexpected(keyError(TreeError::CLOSED));
  }
  if (transaction.owner != Node<Widget>{} && !app_.tree().inputParent(transaction.owner)) {
    return std::unexpected(keyError(TreeError::STALE_NODE, transaction.owner.id()));
  }
  if (keys_ != transaction.previous || input_ != transaction.input || keyRevision_ != transaction.revision ||
      app_.tree().inputGeneration() != transaction.tree || app_.router().focusGeneration() != transaction.focus) {
    return std::unexpected(keyError(TreeError::WOULD_BLOCK));
  }
  if (slot && (slot->owner != owner_ || slot->binding)) {
    return std::unexpected(keyError(TreeError::INVALID_ARGUMENT));
  }
  for (std::size_t i{}; i < snapshot->records.size(); ++i) {
    const auto &metadata = snapshot->records[i]->metadata;
    if (!metadata) {
      continue;
    }
    if (metadata->scope != Node<Widget>{} &&
        connected(metadata->cancellation) !=
          bool((*snapshot->scopeConnected)[i] & KeyRecord::Metadata::SCOPE_CONNECTED)) {
      return std::unexpected(keyError(TreeError::INVALID_ARGUMENT, metadata->scope.id()));
    }
    if (metadata->ownerLifetime && metadata->observation() != (*snapshot->scopeConnected)[i]) {
      return std::unexpected(keyError(TreeError::INVALID_ARGUMENT, metadata->owner.id()));
    }
    if (result && metadata->id == result->id_ && !app_.tree().inputParent(metadata->owner)) {
      return std::unexpected(keyError(TreeError::STALE_NODE, metadata->owner.id()));
    }
  }
  input_.swap(prepared);
  keys_.swap(snapshot);
  keyHandlers_ = keys_->records.size();
  ++keyRevision_;
  keyProjection_.reset();
  if (!preserve && snapshot) {
    resetKeys(*snapshot->pending);
  }
  if (result) {
    keySerial_ = result->id_.serial_;
  }
  if (slot) {
    slot->binding = *result;
  }
  transaction.release();
  (void)app_.tree().invalidate(root_, Invalidation::PAINT);
  return {};
}

std::expected<void, ManagedError> ManagedScene::changeKey(KeyBindingId id, KeyChange change, bool value,
                                                          const KeySelection *selection, const KeyInfo *text) {
  auto allowed = keyBoundary();
  if (!allowed) {
    return allowed;
  }
  auto observed = observeKeyScopes();
  if (!observed) {
    return observed;
  }
  if (keyStaging_) {
    return std::unexpected(keyError(TreeError::WOULD_BLOCK));
  }
  if (!keys_ || id.scene_ != root_.id()) {
    return std::unexpected(keyError(TreeError::STALE_NODE));
  }
  const auto found = std::ranges::find_if(keys_->records, [&](const auto &record) {
    return record->metadata && record->metadata->id == id;
  });
  if (found == keys_->records.end() || !app_.tree().inputParent((*found)->metadata->owner)) {
    return std::unexpected(keyError(TreeError::STALE_NODE));
  }
  if (change == KeyChange::REBIND) {
    const auto &shortcut = detail::KeySelectionAccess::shortcut(*selection);
    if (!shortcut || !*shortcut) {
      auto failure =
        keyError(!shortcut && shortcut.error().cause == ShortcutErrorKind::NO_MEMORY ? TreeError::NO_MEMORY
                                                                                     : TreeError::INVALID_ARGUMENT);
      failure.shortcut = shortcut ? ShortcutError{ShortcutErrorKind::INVALID_CHORD, 0} : shortcut.error();
      return std::unexpected(failure);
    }
    if ((*found)->actions == detail::KeySelectionAccess::actions(*selection) && (*found)->shortcut &&
        sameShortcutValue(*(*found)->shortcut, **shortcut)) {
      return {};
    }
  }
  KeyTransaction transaction(*this);
  transaction.owner = (*found)->metadata->owner;
  try {
    const auto original = *found;
    const auto index = static_cast<std::size_t>(found - keys_->records.begin());
    if ((change == KeyChange::ENABLED && original->metadata->enabled == value) ||
        (change == KeyChange::VISIBLE && original->metadata->visible == value) ||
        (change == KeyChange::UNBIND && !original->shortcut)) {
      return {};
    }
    auto replacement = std::make_shared<KeyRecord>(*original);
    auto metadata = std::make_shared<KeyRecord::Metadata>(*original->metadata);
    replacement->metadata = metadata;
    bool preserve{};
    if (change == KeyChange::ENABLED || change == KeyChange::VISIBLE) {
      bool &state = change == KeyChange::ENABLED ? metadata->enabled : metadata->visible;
      if (state == value) {
        return {};
      }
      state = value;
    } else if (change == KeyChange::TEXT) {
      auto label = keyText(text->label, text->resolver, options_.maxKeyLabelBytes, true);
      if (!label) {
        return std::unexpected(label.error());
      }
      auto description = keyText(text->description, text->resolver, options_.maxKeyDescriptionBytes, false);
      if (!description) {
        return std::unexpected(description.error());
      }
      if (*label == metadata->label && *description == metadata->description) {
        return {};
      }
      metadata->label = std::move(*label);
      metadata->description = std::move(*description);
      preserve = true;
    } else if (change == KeyChange::REBIND) {
      const auto &shortcut = detail::KeySelectionAccess::shortcut(*selection);
      if (!shortcut || !*shortcut) {
        auto failure = keyError(TreeError::INVALID_ARGUMENT);
        failure.shortcut = shortcut ? ShortcutError{ShortcutErrorKind::INVALID_CHORD, 0} : shortcut.error();
        return std::unexpected(failure);
      }
      replacement->shortcut = **shortcut;
      replacement->actions = detail::KeySelectionAccess::actions(*selection);
      if (sameShortcut(original->shortcut, replacement->shortcut) && original->actions == replacement->actions) {
        return {};
      }
    } else if (change == KeyChange::UNBIND) {
      if (!replacement->shortcut) {
        return {};
      }
      replacement->shortcut.reset();
    }
    auto snapshot = std::make_shared<KeySnapshot>();
    snapshot->records = keys_->records;
    snapshot->fallback = keys_->fallback;
    if (change == KeyChange::REMOVE) {
      snapshot->records.erase(snapshot->records.begin() + index);
    } else {
      snapshot->records[index] = replacement;
    }
    return publishKeyCandidate(snapshot, preserve, transaction);
  } catch (const std::bad_alloc &) {
    return std::unexpected(keyError(TreeError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(keyError(TreeError::CALLBACK_FAILED));
  }
}

std::expected<void, ManagedError> ManagedScene::keyBoundary() const {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(keyError(TreeError::WRONG_THREAD));
  }
  if (keyStaging_) {
    return std::unexpected(keyError(TreeError::WOULD_BLOCK));
  }
  return boundary(ManagedPhase::BIND, root_);
}

std::expected<void, ManagedError> ManagedScene::keyPreflight(KeyInfo &info) {
  if (keyHandlers_ >= options_.maxKeyHandlers) {
    return std::unexpected(keyError(TreeError::NODE_LIMIT));
  }
  if (keySerial_ == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(keyError(TreeError::IDENTITY_EXHAUSTED));
  }
  if (info.publication_ && (info.publication_->owner != owner_ || info.publication_->binding)) {
    return std::unexpected(keyError(TreeError::INVALID_ARGUMENT));
  }
  if (info.owner == Node<Widget>{}) {
    info.owner = root_;
  }
  if (!app_.tree().inputParent(info.owner) || !app_.router().within(root_, info.owner)) {
    return std::unexpected(keyError(TreeError::STALE_NODE, info.owner.id()));
  }
  if (info.scope != Node<Widget>{}) {
    const auto *scope = app_.router().scope(info.scope);
    if (!scope || !scope->cancellation->connected.load() || !app_.router().within(root_, info.scope)) {
      return std::unexpected(keyError(TreeError::INVALID_ARGUMENT, info.scope.id()));
    }
    info.scopeIdentity_ = scope->cancellation;
  }
  return {};
}

KeyEligibility ManagedScene::keyEligibility(const KeyRecord &record, Node<Widget> focus) const {
  if (record.metadata) {
    if (!record.metadata->ownerConnected() || !app_.tree().inputParent(record.metadata->owner)) {
      return KeyEligibility::OWNER_RETIRED;
    }
    if (!record.metadata->visible) {
      return KeyEligibility::HIDDEN;
    }
  }
  if (const auto host = app_.router().state_->dialogs.lock()) {
    for (const auto &layer : host->layers) {
      if (!layer->retired && layer->open && layer->options.mode == DialogMode::MODAL) {
        auto live = app_.tree().inputEligible(layer->root);
        if (live && *live) {
          return KeyEligibility::SCOPE_INACTIVE;
        }
      }
    }
  }
  if (!record.metadata) {
    return KeyEligibility::ELIGIBLE;
  }
  const auto &metadata = *record.metadata;
  if (!metadata.enabled) {
    return KeyEligibility::DISABLED;
  }
  if (!record.shortcut) {
    return KeyEligibility::UNBOUND;
  }
  auto eligible = app_.tree().inputEligible(metadata.owner);
  if (!eligible || !*eligible) {
    return KeyEligibility::OWNER_INELIGIBLE;
  }
  if (metadata.scope != Node<Widget>{}) {
    auto scopeEligible = app_.tree().inputEligible(metadata.scope);
    if (!connected(metadata.cancellation) || !scopeEligible || !*scopeEligible || focus == Node<Widget>{} ||
        !app_.router().within(metadata.scope, focus)) {
      return KeyEligibility::SCOPE_INACTIVE;
    }
  }
  for (auto phase : {KeyAction::PRESS, KeyAction::REPEAT, KeyAction::RELEASE}) {
    if (hasPhase(record.actions, phase) && support(options_.keyCapabilities, phase) != KeySupport::UNSUPPORTED) {
      return KeyEligibility::ELIGIBLE;
    }
  }
  return KeyEligibility::TERMINAL_UNSUPPORTED;
}

std::expected<void, ManagedError> ManagedScene::observeKeyScopes() {
  retireThemeSelector();
  if (!keys_ || !keys_->annotated) {
    return {};
  }
  if (!keyScopeGeneration_) {
    return std::unexpected(keyError(TreeError::IDENTITY_EXHAUSTED));
  }
  bool changed{};
  for (std::size_t i{}; i < keys_->records.size(); ++i) {
    const auto &metadata = keys_->records[i]->metadata;
    const unsigned char current = metadata ? metadata->observation() : 0;
    if (current != (*keys_->scopeConnected)[i]) {
      changed = true;
      (*keys_->scopeConnected)[i] = current;
    }
  }
  if (changed) {
    resetKeys(*keys_->pending);
    keyProjection_.reset();
    if (keyScopeGeneration_ == std::numeric_limits<std::uint64_t>::max()) {
      keyScopeGeneration_ = 0;
      return std::unexpected(keyError(TreeError::IDENTITY_EXHAUSTED));
    }
    ++keyScopeGeneration_;
    (void)app_.tree().invalidate(root_, Invalidation::PAINT);
  }
  return {};
}

std::expected<void, ManagedError> ManagedScene::setKeyCapabilities(KeyCapabilities value) {
  auto allowed = keyBoundary();
  if (!allowed) {
    return allowed;
  }
  for (const auto phase : {value.press, value.repeat, value.release}) {
    if (phase != KeySupport::UNKNOWN && phase != KeySupport::SUPPORTED && phase != KeySupport::UNSUPPORTED) {
      return std::unexpected(keyError(TreeError::INVALID_ARGUMENT));
    }
  }
  if (value == options_.keyCapabilities) {
    return {};
  }
  if (!keyRevision_ || keyRevision_ == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(keyError(TreeError::IDENTITY_EXHAUSTED));
  }
  options_.keyCapabilities = value;
  ++keyRevision_;
  keyProjection_.reset();
  if (keys_) {
    resetKeys(*keys_->pending);
  }
  (void)app_.tree().invalidate(root_, Invalidation::PAINT);
  return {};
}

std::expected<std::shared_ptr<const KeyBindingsSnapshot>, ManagedError> ManagedScene::readKeyProjection(bool recover) {
  if (recover) {
    auto allowed = keyBoundary();
    if (!allowed) {
      return std::unexpected(allowed.error());
    }
    failedKeyProjection_ = {};
  } else if (owner_ != std::this_thread::get_id() || closed_) {
    return std::unexpected(keyError(closed_ ? TreeError::CLOSED : TreeError::WRONG_THREAD));
  }
  if (keyStaging_) {
    return std::unexpected(keyError(TreeError::WOULD_BLOCK));
  }
  if (!keyRevision_ || !keyScopeGeneration_) {
    return std::unexpected(keyError(TreeError::IDENTITY_EXHAUSTED));
  }
  Node<Widget> focus = app_.router().state_->focus;
  if (recover) {
    auto recovered = app_.router().focused();
    if (!recovered) {
      return std::unexpected(keyError(recovered.error()));
    }
    focus = *recovered;
  }
  auto scopes = observeKeyScopes();
  if (!scopes) {
    return std::unexpected(scopes.error());
  }
  const auto focusGeneration = app_.router().focusGeneration();
  const auto inputGeneration = app_.tree().inputGeneration();
  if (keyProjection_ && keyProjection_->revision == keyRevision_ &&
      keyProjection_->focusGeneration == focusGeneration && keyProjection_->inputGeneration == inputGeneration &&
      keyProjection_->scopeGeneration == keyScopeGeneration_) {
    return keyProjection_;
  }
  KeyTransaction transaction(*this);
  try {
    const auto canonical = keys_;
    auto projection = std::make_shared<KeyBindingsSnapshot>();
    projection->revision = keyRevision_;
    projection->focusGeneration = focusGeneration;
    projection->inputGeneration = inputGeneration;
    projection->scopeGeneration = keyScopeGeneration_;
    projection->capabilities = options_.keyCapabilities;
    std::size_t used{};
    const auto charge = [&](std::size_t bytes) {
      if (bytes > options_.maxKeyProjectionBytes - used) {
        return false;
      }
      used += bytes;
      return true;
    };
    if (canonical) {
      projection->bindings.reserve(canonical->records.size());
      for (std::size_t ordinal{}; ordinal < canonical->records.size(); ++ordinal) {
        const auto &record = canonical->records[ordinal];
        const auto eligibility = keyEligibility(*record, focus);
        if (eligibility == KeyEligibility::OWNER_RETIRED || eligibility == KeyEligibility::HIDDEN) {
          continue;
        }
        auto empty = GraphemeText::from("", widthPolicy());
        if (!empty) {
          auto failure = keyError(TreeError::INVALID_ARGUMENT);
          failure.text = empty.error();
          return std::unexpected(failure);
        }
        KeyBindingView view(record->metadata ? record->metadata->label : *empty,
                            record->metadata ? record->metadata->description : *empty);
        view.ordinal = ordinal;
        view.legacy = record->legacy;
        view.shortcut = record->shortcut;
        view.eligibility = eligibility;
        if (record->metadata) {
          view.id = record->metadata->id;
          view.owner = record->metadata->owner.id();
          view.scope = record->metadata->scope.id();
        }
        for (auto phase : {KeyAction::PRESS, KeyAction::REPEAT, KeyAction::RELEASE}) {
          if (record->legacy ? phase != KeyAction::RELEASE : hasPhase(record->actions, phase)) {
            view.actions.push_back(phase);
          }
        }
        if (!charge(sizeof(KeyBindingView)) || !charge(view.label.bytes().size()) ||
            !charge(view.description.bytes().size()) || !charge(view.actions.size() * sizeof(KeyAction))) {
          return std::unexpected(keyError(TreeError::NODE_LIMIT));
        }
        const auto chargeStrokes = [&](std::size_t count) {
          return count <= (options_.maxKeyProjectionBytes - used) / sizeof(key::Chord) &&
                 charge(count * sizeof(key::Chord));
        };
        if (view.shortcut) {
          if (!chargeStrokes(view.shortcut->alternatives.size())) {
            return std::unexpected(keyError(TreeError::NODE_LIMIT));
          }
          for (const auto &sequence : view.shortcut->sequences) {
            if (!chargeStrokes(sequence.strokes.size())) {
              return std::unexpected(keyError(TreeError::NODE_LIMIT));
            }
          }
        }
        projection->bindings.push_back(view);
      }
      // Pair relations are built only for a new owning projection, never in layout or paint.
      for (auto &row : projection->bindings) {
        if (row.eligibility != KeyEligibility::ELIGIBLE) {
          continue;
        }
        for (const auto &other : projection->bindings) {
          if (row.ordinal == other.ordinal || other.eligibility != KeyEligibility::ELIGIBLE) {
            continue;
          }
          std::vector<KeyAction> overlap(0);
          for (auto phase : row.actions) {
            if (support(projection->capabilities, phase) != KeySupport::UNSUPPORTED &&
                std::ranges::find(other.actions, phase) != other.actions.end()) {
              overlap.push_back(phase);
            }
          }
          if (overlap.empty()) {
            continue;
          }
          const auto count = row.legacy     ? 1
                             : row.shortcut ? row.shortcut->alternatives.size() + row.shortcut->sequences.size()
                                            : 0;
          const auto otherCount = other.legacy ? 1
                                  : other.shortcut
                                    ? other.shortcut->alternatives.size() + other.shortcut->sequences.size()
                                    : 0;
          for (std::size_t a{}; a < count; ++a) {
            const key::Chord legacy{row.legacy ? row.legacy->key : InputKey{}, row.legacy ? row.legacy->scalar : 0,
                                    row.legacy ? row.legacy->modifiers : InputModifiers{}};
            const auto first =
              row.legacy ? std::span<const key::Chord>(&legacy, 1) : detail::shortcutAlternative(*row.shortcut, a);
            for (std::size_t b{}; b < otherCount; ++b) {
              const key::Chord otherLegacy{other.legacy ? other.legacy->key : InputKey{},
                                           other.legacy ? other.legacy->scalar : 0,
                                           other.legacy ? other.legacy->modifiers : InputModifiers{}};
              const auto second = other.legacy ? std::span<const key::Chord>(&otherLegacy, 1)
                                               : detail::shortcutAlternative(*other.shortcut, b);
              std::size_t shared{};
              while (
                shared < std::min(first.size(), second.size()) &&
                detail::shortcutChordMatches(first[shared], KeyEvent{second[shared].key, second[shared].scalar,
                                                                     second[shared].modifiers, KeyAction::PRESS})) {
                ++shared;
              }
              if (!shared) {
                continue;
              }
              if (!charge(sizeof(KeyConflict)) || !charge(overlap.size() * sizeof(KeyAction))) {
                return std::unexpected(keyError(TreeError::NODE_LIMIT));
              }
              KeyConflict relation;
              relation.alternative = a;
              relation.otherOrdinal = other.ordinal;
              relation.otherId = other.id;
              relation.otherAlternative = b;
              relation.sharedStrokes = shared;
              relation.thisIsNewer = row.ordinal > other.ordinal;
              relation.thisTerminalAtPrefix = shared == first.size();
              relation.otherTerminalAtPrefix = shared == second.size();
              relation.kind = relation.thisTerminalAtPrefix && relation.otherTerminalAtPrefix ? KeyConflictKind::EXACT
                              : relation.thisTerminalAtPrefix || relation.otherTerminalAtPrefix
                                ? KeyConflictKind::TERMINAL_PREFIX
                                : KeyConflictKind::SHARED_PREFIX;
              relation.overlapPhases = overlap;
              row.conflicts.push_back(relation);
            }
          }
        }
      }
    }
    auto budget = checkKeyProjectionBudget(nullptr, projection, used, 0);
    if (!budget) {
      return std::unexpected(budget.error());
    }
    if (closed_ || keys_ != canonical || keyRevision_ != transaction.revision ||
        app_.tree().inputGeneration() != inputGeneration || app_.router().focusGeneration() != focusGeneration) {
      return std::unexpected(keyError(closed_ ? TreeError::CLOSED : TreeError::WOULD_BLOCK));
    }
    if (canonical && canonical->annotated) {
      for (std::size_t i{}; i < canonical->records.size(); ++i) {
        const auto &metadata = canonical->records[i]->metadata;
        if ((metadata ? metadata->observation() : 0) != (*canonical->scopeConnected)[i]) {
          return std::unexpected(keyError(TreeError::WOULD_BLOCK));
        }
      }
    }
    keyProjection_ = projection;
    keyProjectionBytes_ = used;
    return keyProjection_;
  } catch (const std::bad_alloc &) {
    return std::unexpected(keyError(TreeError::NO_MEMORY));
  } catch (...) {
    return std::unexpected(keyError(TreeError::CALLBACK_FAILED));
  }
}

std::expected<KeyBindingsSnapshot, ManagedError> ManagedScene::keyBindings() {
  auto current = readKeyProjection(true);
  if (!current) {
    return std::unexpected(current.error());
  }
  try {
    return **current;
  } catch (const std::bad_alloc &) {
    return std::unexpected(keyError(TreeError::NO_MEMORY));
  }
}
} // namespace portals

namespace portals::ui {
namespace {
std::expected<void, ManagedError> keyCommand(std::expected<void, ManagedError> result, const char *operation,
                                             std::source_location origin) {
  if (!result && !result.error().operation) {
    result.error().operation = operation;
    result.error().operationOrigin = origin;
  }
  return result;
}
} // namespace

std::expected<KeyBinding, ManagedError> KeyBindingRef::binding() const {
  if (slot_ && slot_->owner != std::this_thread::get_id()) {
    return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::WRONG_THREAD, {}, {}});
  }
  if (!slot_ || !slot_->binding) {
    return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::CLOSED, {}, {}});
  }
  return *slot_->binding;
}

std::expected<KeyBindingId, ManagedError> KeyBindingRef::id() const {
  auto value = binding();
  if (!value) {
    return std::unexpected(value.error());
  }
  auto owner = value->scene();
  return owner ? std::expected<KeyBindingId, ManagedError>(value->id()) : std::unexpected(owner.error());
}

std::expected<void, ManagedError> KeyBindingRef::setEnabled(bool state, std::source_location origin) const {
  auto value = binding();
  return keyCommand(value ? value->setEnabled(state) : std::unexpected(value.error()), "KeyBinding.setEnabled", origin);
}

std::expected<void, ManagedError> KeyBindingRef::setVisible(bool state, std::source_location origin) const {
  auto value = binding();
  return keyCommand(value ? value->setVisible(state) : std::unexpected(value.error()), "KeyBinding.setVisible", origin);
}

std::expected<void, ManagedError> KeyBindingRef::setText(TextValue label, TextValue description,
                                                         KeyTextResolver resolver, std::source_location origin) const {
  auto value = binding();
  try {
    return keyCommand(value ? value->setText(std::move(label), std::move(description), std::move(resolver))
                            : std::unexpected(value.error()),
                      "KeyBinding.setText", origin);
  } catch (const std::bad_alloc &) {
    return keyCommand(std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::NO_MEMORY, {}, {}}),
                      "KeyBinding.setText", origin);
  }
}

std::expected<void, ManagedError> KeyBindingRef::rebind(const KeySelection &selection,
                                                        std::source_location origin) const {
  auto value = binding();
  return keyCommand(value ? value->rebind(selection) : std::unexpected(value.error()), "KeyBinding.rebind", origin);
}

std::expected<void, ManagedError> KeyBindingRef::unbind(std::source_location origin) const {
  auto value = binding();
  return keyCommand(value ? value->unbind() : std::unexpected(value.error()), "KeyBinding.unbind", origin);
}

std::expected<void, ManagedError> KeyBindingRef::remove(std::source_location origin) const {
  auto value = binding();
  return keyCommand(value ? value->remove() : std::unexpected(value.error()), "KeyBinding.remove", origin);
}
} // namespace portals::ui
