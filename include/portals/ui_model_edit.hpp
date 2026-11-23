// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/model_edit.hpp>
#include <portals/ui_form_field.hpp>
#include <portals/ui_model.hpp>

namespace portals::ui {
template <class Row> class EditSession;
template <class Row, class Value> class EditField;

/** @brief Owning inspection of one exact retained target, independent of navigation. */
template <class Value> struct EditFieldSnapshot {
  EditTarget target; ///< Exact resource, source incarnation, edit session, record and field qualification.
  std::shared_ptr<const Value>
    accepted; ///< Owning last admitted accepted value; a newer read may exceed retention credit.
  std::shared_ptr<const Value> draft;   ///< Nullable owning local draft for this exact target.
  EditStatus status{EditStatus::CLEAN}; ///< Current retained draft, submission or acceptance state.
};

} // namespace portals::ui

#include <portals/detail/ui_model_edit_core.hpp>

namespace portals::ui {

/** @brief Owning UI-thread-affine current-record editing descriptor; no runtime is created before mount.
 * Field/session operations, including retained inspection and reply admission, require the creating UI thread.
 * Workers receive owning requests and deliver receipts through the existing UI delivery path.
 */
template <class Row> class EditSession {
public:
  EditSession() = default;

  /** @brief Typed field with a process-local member shorthand.
   * @param member Record member selected by this field; its pointer is not a persistent schema key.
   * @return Shared field descriptor retaining the selected record's draft and history.
   */
  template <class Value> EditField<Row, Value> field(Value Row::*member) const {
    return makeField({}, member, {});
  }

  /** @brief Typed field with an explicit persistent schema key.
   * @param key Nonempty schema identity copied into owning storage.
   * @param member Record member selected by this field.
   * @param endpoint Optional source adapter; omitted for the built-in model field capability.
   * @return Shared field descriptor, or an empty descriptor for an invalid or conflicting key.
   */
  template <class Value>
  EditField<Row, Value> field(std::string_view key, Value Row::*member,
                              std::type_identity_t<std::shared_ptr<EditEndpoint<Value>>> endpoint = {}) const {
    return makeField(key, member, std::move(endpoint), true);
  }

  /** @param origin Calling location used by managed error reporting.
   * @return Checked command admission; pending remains visible in lastResult(), never claimed committed.
   */
  EditorCommandResult commit(std::source_location origin = std::source_location::current()) const {
    const auto state = state_;
    const auto navigation = state ? state->managed : ManagedNavigation{};
    auto result = commitChecked();
    return detail::EditAccess::command(navigation, result, origin);
  }

  /** @param origin Calling location used by managed error reporting.
   * @return Cancel only displayed/current drafts, adopting admitted accepted values.
   * Already admitted backend requests remain in flight; cancellation does not undo backend work.
   */
  EditorCommandResult cancel(std::source_location origin = std::source_location::current()) const {
    const auto state = state_;
    const auto navigation = state ? state->managed : ManagedNavigation{};
    auto result = cancelChecked();
    return detail::EditAccess::command(navigation, result, origin);
  }

  /** @return Owning explicit per-field result; non-atomic partial acceptance remains visible. */
  std::shared_ptr<const EditCommitResult> lastResult() const {
    return state_ && state_->ownerThread == std::this_thread::get_id() ? state_->last : nullptr;
  }

  /** @param receipt Original owning backend reply. @return True for an admitted final reply, false when stale. */
  std::expected<bool, TreeError> applyReply(const EditReceipt &receipt) const {
    const auto state = state_;
    if (!state) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    if (state->ownerThread != std::this_thread::get_id()) {
      return std::unexpected(TreeError::WRONG_THREAD);
    }
    if (state->accounting) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    if (state->endpointClosed) {
      return false;
    }
    for (const auto &entry : state->entries) {
      if (entry->target.resource == receipt.target.resource && entry->target.session == receipt.target.session &&
          entry->target.row == receipt.target.row && entry->target.field == receipt.target.field &&
          entry->target.sourceSession == receipt.target.sourceSession) {
        auto retained = entry;
        return retained->reply(receipt);
      }
    }
    return false;
  }

private:
  template <class> friend class EditSession;
  template <class, class> friend class EditField;
  template <class T> friend EditSession<T> edit(Navigation<T>, EditOptions);
  std::expected<void, TreeError> commitChecked() const;
  std::expected<void, TreeError> cancelChecked() const;
  std::shared_ptr<detail::EditState<Row>> state_;
  template <class Value>
  EditField<Row, Value> makeField(std::string_view, Value Row::*, std::shared_ptr<EditEndpoint<Value>>,
                                  bool persistent = false) const;
};

/** @brief UI-thread-affine typed field capability with exact-target retained inspection and discard. */
template <class Row, class Value> class EditField {
public:
  /** @return Current field snapshot; owning values do not borrow a provider page. */
  std::expected<EditFieldSnapshot<Value>, TreeError> snapshot() const {
    const auto state = state_;
    const auto definition = definition_;
    try {
      if (!state || !definition) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
      if (state->ownerThread != std::this_thread::get_id()) {
        return std::unexpected(TreeError::WRONG_THREAD);
      }
      auto position = state->current();
      if (!position) {
        return std::unexpected(position.error());
      }
      auto entry = state->entry(definition, *position);
      return entry ? inspect(**entry) : std::unexpected(entry.error());

    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::NO_MEMORY);
    } catch (const std::length_error &) {
      return std::unexpected(TreeError::NO_MEMORY);
    } catch (...) {
      return std::unexpected(TreeError::CALLBACK_FAILED);
    }
  }

  /** @param target Original exact destination. @return Retained data, including an orphan after source close. */
  std::expected<EditFieldSnapshot<Value>, TreeError> snapshot(const EditTarget &target) const {
    try {
      auto entry = retained(target);
      return entry ? inspect(**entry) : std::unexpected(entry.error());

    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::NO_MEMORY);
    } catch (const std::length_error &) {
      return std::unexpected(TreeError::NO_MEMORY);
    } catch (...) {
      return std::unexpected(TreeError::CALLBACK_FAILED);
    }
  }

  /** @param value Owning typed candidate. @return Validation/submission status; unpublished input stays owned. */
  std::expected<EditStatus, TreeError> set(const Value &value) const {
    const auto state = state_;
    const auto definition = definition_;
    try {
      if (!state || !definition) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
      if (state->ownerThread != std::this_thread::get_id()) {
        return std::unexpected(TreeError::WRONG_THREAD);
      }
      if (state->accounting) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
      auto position = state->current();
      if (!position) {
        return std::unexpected(position.error());
      }
      auto entry = state->entry(definition, *position);
      if (!entry) {
        return std::unexpected(entry.error());
      }
      if (!(*entry)->endpoint->capabilities().writable) {
        return EditStatus::READ_ONLY;
      }
      if constexpr (std::same_as<Value, std::string>) {
        for (const auto &weak : state->bindings) {
          auto binding = weak.lock();
          if (binding && !binding->closed && binding->displayedEntry == entry->get() &&
              detail::preferredEditBinding(state->bindings, binding)) {
            auto result = binding->setText(value);
            if (!result) {
              return std::unexpected(result.error());
            }
            return result->status == LineEditStatus::COMMITTED           ? EditStatus::ACCEPTED
                   : result->status == LineEditStatus::PENDING           ? EditStatus::PENDING
                   : result->status == LineEditStatus::VALIDATION_FAILED ? EditStatus::INVALID
                   : result->status == LineEditStatus::CONFLICT          ? EditStatus::CONFLICT
                                                                         : EditStatus::DIRTY;
          }
        }
      }
      if constexpr (std::same_as<Value, std::string>) {
        if ((*entry)->text) {
          auto updated = (*entry)->setRetainedText(value);
          if (!updated) {
            return std::unexpected(updated.error());
          }
          if (state->options.mode == EditMode::IMMEDIATE) {
            auto result = (*entry)->commit();
            return result ? std::expected<EditStatus, TreeError>(result->status) : std::unexpected(result.error());
          }
          return EditStatus::DIRTY;
        }
      }
      if (!(*entry)->endpoint->capabilities().writable) {
        return EditStatus::READ_ONLY;
      }
      auto charge = (*entry)->retainedCharge(&value, (*entry)->textCharge, true);
      if (!charge) {
        return std::unexpected(charge.error());
      }
      auto checked = state->checkCredit(**entry, *charge);
      if (!checked) {
        return std::unexpected(checked.error());
      }
      auto candidate = std::make_shared<const Value>(value);
      charge = (*entry)->retainedCharge(candidate.get(), (*entry)->textCharge);
      if (!charge) {
        return std::unexpected(charge.error());
      }
      if ((*entry)->discarded || state->endpointClosed) {
        return std::unexpected(TreeError::CLOSED);
      }
      auto admitted = state->admit(**entry, *charge);
      if (!admitted) {
        return std::unexpected(admitted.error());
      }
      const auto previousDraft = std::exchange((*entry)->draft, std::move(candidate));
      ++(*entry)->generation;
      (*entry)->status = EditStatus::DIRTY;
      if (state->options.mode == EditMode::IMMEDIATE) {
        auto result = (*entry)->commit();
        return result ? std::expected<EditStatus, TreeError>(result->status) : std::unexpected(result.error());
      }
      return EditStatus::DIRTY;

    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::NO_MEMORY);
    } catch (const std::length_error &) {
      return std::unexpected(TreeError::NO_MEMORY);
    } catch (...) {
      return std::unexpected(TreeError::CALLBACK_FAILED);
    }
  }

  /** @param target Frozen destination, never resolved through nearest/current.
   * @return Checked removal of this retained draft and its history; other targets are unchanged.
   * A live admitted write returns WOULD_BLOCK until its final receipt, preserving serialization.
   */
  std::expected<void, TreeError> discard(const EditTarget &target) const {
    try {
      auto entry = retained(target);
      return entry ? (*entry)->cancel(true) : std::unexpected(entry.error());

    } catch (const std::bad_alloc &) {
      return std::unexpected(TreeError::NO_MEMORY);
    } catch (const std::length_error &) {
      return std::unexpected(TreeError::NO_MEMORY);
    } catch (...) {
      return std::unexpected(TreeError::CALLBACK_FAILED);
    }
  }

  /** @param rule Owning typed validation rule; borrowed values expire on return.
   * @return This shared field definition with its explicit synchronous rule.
   */
  EditField validate(std::function<bool(const Value &)> rule) const {
    if (!state_ || !definition_ || state_->ownerThread != std::this_thread::get_id()) {
      return *this;
    }
    definition_->validate = std::move(rule);
    return *this;
  }

  /// @cond INTERNAL
  std::shared_ptr<detail::EditState<Row>> state_;
  std::shared_ptr<detail::EditDefinition<Row, Value>> definition_;
  /// @endcond

private:
  friend class EditSession<Row>;

  std::expected<std::shared_ptr<detail::EditEntry<Row, Value>>, TreeError> retained(const EditTarget &target) const {
    if (state_ && state_->ownerThread != std::this_thread::get_id()) {
      return std::unexpected(TreeError::WRONG_THREAD);
    }
    if (!state_ || !definition_ || target.field != definition_->key) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    for (const auto &base : state_->entries) {
      if (base->target.resource == target.resource && base->target.session == target.session &&
          base->target.row == target.row && base->target.field == target.field &&
          base->target.sourceSession == target.sourceSession && !base->discarded) {
        auto value = std::dynamic_pointer_cast<detail::EditEntry<Row, Value>>(base);
        return value ? std::expected<std::shared_ptr<detail::EditEntry<Row, Value>>, TreeError>(std::move(value))
                     : std::unexpected(TreeError::INVALID_ARGUMENT);
      }
    }
    return std::unexpected(TreeError::STALE_NODE);
  }

  static std::expected<EditFieldSnapshot<Value>, TreeError> inspect(detail::EditEntry<Row, Value> &entry) {
    auto draft = entry.draft;
    if constexpr (std::same_as<Value, std::string>) {
      if (entry.text && entry.text->dirty) {
        draft = std::make_shared<const Value>(entry.text->text.bytes());
      }
    }
    return EditFieldSnapshot<Value>{
      entry.target, entry.accepted, std::move(draft),
      entry.text && entry.text->dirty && entry.status == EditStatus::CLEAN ? EditStatus::DIRTY : entry.status};
  }
};

template <class Row>
template <class Value>
EditField<Row, Value> EditSession<Row>::makeField(std::string_view key, Value Row::*member,
                                                  std::shared_ptr<EditEndpoint<Value>> endpoint,
                                                  bool persistent) const {
  static_assert(std::copy_constructible<Value> && std::copy_constructible<Row>,
                "Edit fields require owning copyable values");
  static_assert(!std::is_pointer_v<Value> && !portals::detail::IS_BORROWED_DERIVED_VALUE<Value> &&
                  !std::ranges::view<Value>,
                "Edit fields cannot retain borrowed values");
  if (!state_ || !member || state_->ownerThread != std::this_thread::get_id() || (persistent && key.empty())) {
    return {};
  }
  for (const auto &base : state_->definitions) {
    auto definition = std::dynamic_pointer_cast<detail::EditDefinition<Row, Value>>(base);
    if (persistent && base->key.name() == key && (!definition || definition->member != member)) {
      return {};
    }
    if (definition && definition->member == member &&
        (key.empty() ? !definition->key.persistent() : definition->key.name() == key)) {
      EditField<Row, Value> field;
      field.state_ = state_;
      field.definition_ = std::move(definition);
      return field;
    }
  }
  auto definition = std::make_shared<detail::EditDefinition<Row, Value>>();
  definition->member = member;
  definition->endpoint = std::move(endpoint);
  definition->prepare =
    [weakState = std::weak_ptr(state_), weakField = std::weak_ptr(definition)](
      const NavigationPosition &position,
      const std::shared_ptr<portals::detail::ManagedModelSource> &source) -> std::expected<void, TreeError> {
    auto state = weakState.lock();
    auto field = weakField.lock();
    if (field && !field->active) {
      return {};
    }
    if (field && ((field->endpoint && !field->endpoint->capabilities().writable) ||
                  (!field->endpoint && !detail::EditAccess::sequence(detail::EditAccess::model<Row>(source))))) {
      return {};
    }
    if (!state || !field) {
      return std::unexpected(TreeError::CLOSED);
    }
    auto ready = state->entry(field, position, source);
    return ready ? std::expected<void, TreeError>{} : std::unexpected(ready.error());
  };
  if (!key.empty()) {
    auto id = EditFieldKey::create(key);
    if (!id) {
      return {};
    }
    definition->key = std::move(*id);
  } else {
    definition->key = EditFieldKey::local(state_->nextLocal++);
  }
  state_->definitions.push_back(definition);
  EditField<Row, Value> field;
  field.state_ = state_;
  field.definition_ = std::move(definition);
  return field;
}

/** @param navigation Existing explicit current-record context. @param options Existing modes and retained budgets.
 * @return Cold owning session; readonly sources remain navigable and readable.
 */
template <class Row> EditSession<Row> edit(Navigation<Row> navigation, EditOptions options = {}) {
  EditSession<Row> result;
  result.state_ = std::make_shared<detail::EditState<Row>>(std::move(navigation), options);
  return result;
}

namespace detail {
/// @cond INTERNAL
template <class Row>
std::vector<std::shared_ptr<EditEntryBase>> displayedEntries(const std::shared_ptr<EditState<Row>> &state) {
  std::vector<std::shared_ptr<EditEntryBase>> result(0);
  bool displayed = false;
  for (const auto &weak : state->bindings) {
    auto binding = weak.lock();
    if (!binding || binding->closed) {
      continue;
    }
    displayed = true;
    if (!binding->displayedEntry) {
      continue;
    }
    for (const auto &entry : state->entries) {
      if (entry.get() == binding->displayedEntry && std::ranges::find(result, entry) == result.end()) {
        result.push_back(entry);
      }
    }
  }
  if (displayed) {
    for (const auto &entry : state->entries) {
      const bool sameRecord = std::ranges::any_of(result, [&](const auto &visible) {
        return visible->target.resource == entry->target.resource && visible->target.session == entry->target.session &&
               visible->target.row == entry->target.row && visible->target.sourceSession == entry->target.sourceSession;
      });
      if (sameRecord && !entry->discarded && std::ranges::find(result, entry) == result.end()) {
        result.push_back(entry);
      }
    }
    return result;
  }
  auto position = state->current();
  if (!position) {
    return result;
  }
  for (const auto &entry : state->entries) {
    if (entry->target.resource == position->resource && position->row == entry->target.row && !entry->discarded) {
      result.push_back(entry);
    }
  }
  return result;
}

/// @endcond
} // namespace detail

namespace detail {
/// @cond INTERNAL
struct EditCommandField {
  std::shared_ptr<EditEntryBase> entry;
  std::shared_ptr<portals::detail::EditTextBinding> binding;
};

template <class Row> std::vector<EditCommandField> editCommandPlan(const std::shared_ptr<EditState<Row>> &state) {
  const auto entries = displayedEntries(state);
  std::vector<EditCommandField> plan(0);
  plan.reserve(entries.size());
  for (const auto &entry : entries) {
    auto chosen = selectEditBinding(state->bindings, entry.get());
    plan.push_back({entry, std::move(chosen)});
  }
  return plan;
}

inline std::expected<EditReceipt, TreeError> commitEditField(const EditCommandField &field) {
  if (!field.binding) {
    return field.entry->commit();
  }
  if (field.binding->closed || field.binding->displayedEntry != field.entry.get()) {
    return EditReceipt{field.entry->target, EditStatus::CONFLICT};
  }
  auto completed = field.binding->complete(false);
  if (!completed) {
    return std::unexpected(completed.error());
  }
  if (completed->status == LineEditStatus::COMMITTED) {
    return field.entry->lastReceipt;
  }
  if (completed->status == LineEditStatus::PENDING) {
    if (field.entry->lastReceipt.status == EditStatus::PENDING && field.entry->lastReceipt.target.sequence) {
      return field.entry->lastReceipt;
    }
    return EditReceipt{field.entry->target, EditStatus::PENDING};
  }
  if (completed->status == LineEditStatus::VALIDATION_FAILED) {
    field.entry->status = EditStatus::INVALID;
    return EditReceipt{field.entry->target, EditStatus::INVALID};
  }
  if (completed->status == LineEditStatus::CONFLICT) {
    field.entry->status = EditStatus::CONFLICT;
    return EditReceipt{field.entry->target, EditStatus::CONFLICT};
  }
  if (completed->error) {
    return std::unexpected(*completed->error);
  }
  return EditReceipt{field.entry->target, field.entry->hasDraft() ? EditStatus::DIRTY : EditStatus::CLEAN};
}

/// @endcond
} // namespace detail

template <class Row> std::expected<void, TreeError> EditSession<Row>::commitChecked() const {
  const auto state = state_;
  if (!state) {
    return std::unexpected(TreeError::CLOSED);
  }
  if (state->ownerThread != std::this_thread::get_id()) {
    return std::unexpected(TreeError::WRONG_THREAD);
  }
  if (state->endpointClosed) {
    return std::unexpected(TreeError::CLOSED);
  }
  if (state->accounting) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }
  try {
    auto alive = state->current();
    if (!alive) {
      return std::unexpected(alive.error());
    }
    const auto plan = detail::editCommandPlan(state);
    auto result = std::make_shared<EditCommitResult>();
    result->fields.reserve(plan.size());
    state->last = result;
    bool rejected = false;
    for (const auto &field : plan) {
      const auto previousSequence = field.entry->lastReceipt.target.sequence;
      std::expected<EditReceipt, TreeError> receipt;
      try {
        receipt = detail::commitEditField(field);
      } catch (const std::bad_alloc &) {
        receipt = std::unexpected(TreeError::NO_MEMORY);
      } catch (...) {
        receipt = std::unexpected(TreeError::CALLBACK_FAILED);
      }
      if (!receipt) {
        auto partial = field.entry->lastReceipt.target.sequence != previousSequence
                         ? field.entry->lastReceipt
                         : EditReceipt{field.entry->target, EditStatus::REJECTED};
        partial.error = receipt.error();
        result->fields.push_back(std::move(partial));
        return std::unexpected(receipt.error());
      }
      rejected = rejected || (receipt->status != EditStatus::CLEAN && receipt->status != EditStatus::ACCEPTED &&
                              receipt->status != EditStatus::PENDING);
      result->fields.push_back(std::move(*receipt));
    }
    return rejected ? std::unexpected(TreeError::WOULD_BLOCK) : std::expected<void, TreeError>{};
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(TreeError::CALLBACK_FAILED);
  }
}

template <class Row> std::expected<void, TreeError> EditSession<Row>::cancelChecked() const {
  const auto state = state_;
  if (!state) {
    return std::unexpected(TreeError::CLOSED);
  }
  if (state->ownerThread != std::this_thread::get_id()) {
    return std::unexpected(TreeError::WRONG_THREAD);
  }
  if (state->endpointClosed) {
    return std::unexpected(TreeError::CLOSED);
  }
  if (state->accounting) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }
  try {
    auto alive = state->current();
    if (!alive) {
      return std::unexpected(alive.error());
    }
    const auto plan = detail::editCommandPlan(state);
    for (const auto &field : plan) {
      if (field.binding && (field.binding->closed || field.binding->displayedEntry != field.entry.get())) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
      auto prepared = field.entry->prepareCancel();
      if (!prepared) {
        return prepared;
      }
    }

    // Completion uses the admitted value/version pairs; no backend read follows clearing input.
    /** @brief Release the prepared cancellation snapshot pins on every return path. */
    struct ReadPin {
      const std::vector<detail::EditCommandField> &plan;

      ~ReadPin() {
        for (const auto &field : plan) {
          field.entry->cancelReadPinned = false;
        }
      }
    } pin{plan};

    for (const auto &field : plan) {
      field.entry->cancelReadPinned = true;
    }
    for (const auto &field : plan) {
      if (field.binding) {
        if (field.binding->closed || field.binding->displayedEntry != field.entry.get()) {
          return std::unexpected(TreeError::WOULD_BLOCK);
        }
        auto canceled = field.binding->complete(true);
        if (!canceled) {
          return std::unexpected(canceled.error());
        }
        if (canceled->status != LineEditStatus::CANCELLED) {
          return std::unexpected(canceled->error.value_or(TreeError::WOULD_BLOCK));
        }
      }
      auto canceled = field.entry->cancel(false);
      if (!canceled) {
        return canceled;
      }
    }
    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(TreeError::CALLBACK_FAILED);
  }
}

namespace detail {
/// @cond INTERNAL
template <class Row> struct EditTextMount {
  EditField<Row, std::string> field;
  portals::detail::EditTextBinding *binding{};
  LineEditOptions options;
  std::unique_ptr<portals::detail::EditTextDraft> empty;
  std::shared_ptr<const PropertyChange<std::string>> emptySnapshot;
  std::shared_ptr<const FieldValidation> unchecked;
  EditEntry<Row, std::string> *creditedEntry{};
  std::size_t preparedCharge{};
  std::size_t preparedTextCharge{};
  bool preparedRetention{};

  std::expected<void, TreeError> initialize(const LineEditOptions &value) {
    options = value;
    auto text = GraphemeText::from("", options.widthPolicy, options.limits);
    if (!text) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    empty = std::make_unique<portals::detail::EditTextDraft>(std::move(*text), options);
    empty->initialized = true;
    emptySnapshot = std::make_shared<const PropertyChange<std::string>>();
    unchecked = std::make_shared<const FieldValidation>();
    binding->displayed = empty.get();
    binding->prepared = empty.get();
    auto ready = prepare();
    if (!ready) {
      return ready;
    }
    binding->displayed = binding->prepared;
    binding->displayedEntry = binding->preparedEntry;
    return {};
  }

  std::expected<void, TreeError> prepareText(const std::shared_ptr<EditEntry<Row, std::string>> &entry) {
    if (!entry->text) {
      if (field.state_->nextGeneration == std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
      }
      auto text =
        GraphemeText::from(entry->draft ? *entry->draft : *entry->accepted, options.widthPolicy, options.limits);
      if (!text) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
      auto draft = std::make_unique<portals::detail::EditTextDraft>(std::move(*text), options);
      draft->cursor = draft->text.bytes().size();
      draft->anchor = draft->cursor;
      draft->revision = entry->target.baseVersion;
      draft->generation = field.state_->nextGeneration++;
      draft->initialized = true;
      draft->dirty = static_cast<bool>(entry->draft);
      auto prepared =
        draft->history->prepareResetBaseline({std::string(draft->text.bytes()), draft->cursor, draft->anchor});
      if (!prepared) {
        return std::unexpected(TreeError::NO_MEMORY);
      }
      auto committed = draft->history->commit(std::move(*prepared));
      if (!committed) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
      if (options.displayMode == LineEditDisplayMode::PASSWORD) {
        auto masked =
          GraphemeText::from(std::string(draft->text.graphemes().size(), '*'), options.widthPolicy, options.limits);
        if (!masked) {
          return std::unexpected(TreeError::INVALID_ARGUMENT);
        }
        draft->mask = std::make_unique<GraphemeText>(std::move(*masked));
      }
      auto textCharge =
        portals::detail::EditStorageAccess::draftBytes(draft->text, draft->mask.get(), draft->history->retainedBytes());
      if (!textCharge) {
        return std::unexpected(textCharge.error());
      }
      auto charge = entry->retainedCharge(entry->draft.get(), *textCharge);
      if (!charge) {
        return std::unexpected(charge.error());
      }
      auto admitted = field.state_->admit(*entry, *charge, false);
      if (!admitted) {
        return admitted;
      }
      entry->textCharge = *textCharge;
      entry->text = std::move(draft);
    }
    return {};
  }

  std::expected<void, TreeError> preflight(const NavigationPosition &position,
                                           const std::shared_ptr<portals::detail::ManagedModelSource> &source) {
    const auto &definition = field.definition_;
    if ((definition->endpoint && !definition->endpoint->capabilities().writable) ||
        (!definition->endpoint && !EditAccess::sequence(EditAccess::model<Row>(source)))) {
      return {};
    }
    auto entry = field.state_->entry(definition, position, source);
    return entry ? prepareText(*entry) : std::unexpected(entry.error());
  }

  std::expected<void, TreeError> prepare() {
    auto position = field.state_->current();
    if (!position) {
      if (position.error() == TreeError::CLOSED) {
        binding->writable = false;
        return {};
      }
      return std::unexpected(position.error());
    }
    if (!position->row || position->resolution != PositionResolution::EXACT) {
      binding->prepared = empty.get();
      binding->preparedEntry = nullptr;
      return {};
    }
    auto entry = field.state_->entry(field.definition_, *position);
    if (!entry) {
      if (entry.error() == TreeError::WOULD_BLOCK) {
        binding->prepared = empty.get();
        binding->preparedEntry = nullptr;
        return {};
      }
      return std::unexpected(entry.error());
    }
    auto ready = prepareText(*entry);
    if (!ready) {
      return ready;
    }
    if (binding->displayedEntry != entry->get() && binding->validation && binding->displayValidation) {
      if (binding->displayedEntry && binding->displayedEntry == binding->preparedEntry) {
        auto retained = preserveValidation();
        if (!retained) {
          return retained;
        }
      }
      auto shown = binding->displayValidation((*entry)->validation ? (*entry)->validation : unchecked);
      if (!shown) {
        return shown;
      }
    }
    binding->writable = (*entry)->endpoint->capabilities().writable;
    binding->prepared = (*entry)->text.get();
    binding->preparedEntry = entry->get();
    return {};
  }

  std::expected<std::shared_ptr<const PropertyChange<std::string>>, TreeError> snapshot() {
    auto *entry = static_cast<EditEntry<Row, std::string> *>(binding->displayedEntry);
    if (!entry) {
      return emptySnapshot;
    }
    auto ready = entry->refresh();
    const bool retainedRefusal =
      !ready && ready.error() == TreeError::NODE_LIMIT && entry->admissionRefused && entry->textSnapshot;
    if (!ready && !retainedRefusal && ready.error() != TreeError::WOULD_BLOCK &&
        ready.error() != TreeError::STALE_NODE && ready.error() != TreeError::CLOSED) {
      return std::unexpected(ready.error());
    }
    return entry->textSnapshot;
  }

  std::expected<std::shared_ptr<const PropertyChange<std::string>>, TreeError> cancelSnapshot() {
    binding->retentionRefused = false;
    auto *entry = static_cast<EditEntry<Row, std::string> *>(binding->displayedEntry);
    if (!entry) {
      return emptySnapshot;
    }
    // Session Cancel already preflighted and pinned every selected field. Routed Cancel
    // must admit its own fresh baseline; the display fallback cannot authorize data loss.
    if (!entry->cancelReadPinned) {
      auto ready = entry->prepareCancel();
      if (!ready) {
        binding->retentionRefused = ready.error() == TreeError::NODE_LIMIT && entry->admissionRefused;
        return std::unexpected(ready.error());
      }
    }
    // The owning snapshot pins the admitted value/version through history preparation and adopt.
    return entry->textSnapshot;
  }

  std::expected<std::optional<PropertyUpdate<std::string>>, TreeError> publish(const std::string &candidate) {
    auto *entry = static_cast<EditEntry<Row, std::string> *>(binding->displayedEntry);
    if (!entry || field.state_->endpointClosed) {
      return std::unexpected(TreeError::CLOSED);
    }
    binding->publicationStatus = LineEditStatus::FAILED;
    binding->retentionRefused = false;
    auto charge = entry->retainedCharge(&candidate, entry->textCharge, true);
    if (!charge) {
      binding->retentionRefused = charge.error() == TreeError::NODE_LIMIT;
      return std::unexpected(charge.error());
    }
    auto checked = field.state_->checkCredit(*entry, *charge);
    if (!checked) {
      binding->retentionRefused = checked.error() == TreeError::NODE_LIMIT;
      return std::unexpected(checked.error());
    }
    auto retained = std::make_shared<const std::string>(candidate);
    auto snapshot = std::make_shared<PropertyChange<std::string>>(PropertyChange<std::string>{candidate, 0});
    const auto snapshotBytes = sizeof(PropertyChange<std::string>) + snapshot->value.capacity() + 1;
    charge = entry->retainedCharge(retained.get(), entry->textCharge, false, {}, nullptr,
                                   std::max(entry->snapshotCharge, snapshotBytes));
    if (!charge) {
      binding->retentionRefused = charge.error() == TreeError::NODE_LIMIT;
      return std::unexpected(charge.error());
    }
    auto admitted = field.state_->admit(*entry, *charge);
    if (!admitted) {
      binding->retentionRefused = admitted.error() == TreeError::NODE_LIMIT;
      return std::unexpected(admitted.error());
    }
    entry->draft = std::move(retained);
    entry->status = EditStatus::DIRTY;
    binding->publicationStatus = LineEditStatus::FAILED;
    if (binding->validatedByEditor) {
      entry->validatedGeneration = entry->text->generation;
    }
    auto receipt = entry->commit();
    if (!receipt) {
      binding->retentionRefused = entry->admissionRefused;
      return std::unexpected(receipt.error());
    }
    if (receipt->status == EditStatus::PENDING) {
      return std::optional<PropertyUpdate<std::string>>{};
    }
    if (receipt->status != EditStatus::ACCEPTED && receipt->status != EditStatus::CLEAN) {
      binding->publicationStatus =
        receipt->status == EditStatus::INVALID ? LineEditStatus::VALIDATION_FAILED : LineEditStatus::CONFLICT;
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    snapshot->revision = receipt->acceptedVersion;
    if (!entry->discarded) {
      entry->textSnapshot = snapshot;
      entry->snapshotCharge = snapshotBytes;
    }
    return std::optional(
      PropertyUpdate<std::string>{std::move(snapshot), receipt->acceptedVersion != receipt->target.baseVersion});
  }

  std::expected<void, TreeError> preserveValidation() {
    auto *entry = static_cast<EditEntry<Row, std::string> *>(binding->displayedEntry);
    if (!entry || !binding->validation) {
      return {};
    }
    auto presentation = binding->validation();
    if (!presentation) {
      return std::unexpected(presentation.error());
    }
    if ((*presentation)->status == FieldStatus::UNCHECKED) {
      auto checked = field.state_->admit(*entry, entry->charge - entry->validationCharge, false);
      if (!checked) {
        return checked;
      }
      entry->validation.reset();
      entry->validationCharge = 0;
      return {};
    }
    const auto extra = sizeof(FieldValidation) + (*presentation)->message.capacity() + 1;
    const auto limit = field.state_->options.maxRetainedBytes;
    const auto oldCharge = entry->charge - entry->validationCharge;
    if (oldCharge > limit || extra > limit - oldCharge) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }
    auto credit = field.state_->admit(*entry, oldCharge + extra);
    if (!credit) {
      return credit;
    }
    entry->validation = std::move(*presentation);
    entry->validationCharge = extra;
    return {};
  }

  std::expected<void, TreeError> admit(std::size_t textBytes, bool retain) {
    auto *entry = static_cast<EditEntry<Row, std::string> *>(binding->displayedEntry);
    if (!entry) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    auto charge = entry->retainedCharge(entry->draft.get(), textBytes);
    if (!charge) {
      return std::unexpected(charge.error());
    }
    auto checked = field.state_->checkCredit(*entry, *charge, retain);
    if (!checked) {
      return checked;
    }
    preparedRetention = retain;
    creditedEntry = entry;
    preparedCharge = *charge;
    preparedTextCharge = textBytes;
    return {};
  }

  void accepted() noexcept {
    if (!creditedEntry) {
      return;
    }
    field.state_->bytes = field.state_->bytes - creditedEntry->charge + preparedCharge;
    creditedEntry->charge = preparedCharge;
    creditedEntry->textCharge = preparedTextCharge;
    if (preparedRetention) {
      creditedEntry->status = EditStatus::DIRTY;
    }
    creditedEntry = nullptr;
  }
};

template <class Row>
std::shared_ptr<portals::detail::EditTextBinding> editTextBinding(EditField<Row, std::string> field) {
  auto binding = std::make_shared<portals::detail::EditTextBinding>();
  auto mount = std::make_shared<EditTextMount<Row>>();
  mount->field = field;
  mount->binding = binding.get();
  binding->initialize = [mount](const LineEditOptions &options) {
    return mount->initialize(options);
  };
  binding->prepare = [mount] {
    return mount->prepare();
  };
  binding->preflight = [mount](const NavigationPosition &position,
                               const std::shared_ptr<portals::detail::ManagedModelSource> &source) {
    return mount->preflight(position, source);
  };
  binding->snapshot = [mount] {
    return mount->snapshot();
  };
  binding->cancelSnapshot = [mount] {
    return mount->cancelSnapshot();
  };
  binding->publish = [mount](const std::string &candidate) {
    return mount->publish(candidate);
  };
  binding->preserveValidation = [mount] {
    return mount->preserveValidation();
  };
  binding->admit = [mount](std::size_t bytes, bool retain) {
    return mount->admit(bytes, retain);
  };
  binding->accepted = [mount] {
    mount->accepted();
  };
  binding->observe = [state = field.state_](Node<LineEdit> receiver,
                                            std::function<void()> callback) -> std::expected<Subscription, TreeError> {
    auto channel = state->managed.current();
    if (!channel) {
      return std::unexpected(channel.error());
    }
    return Access::observeEditNavigation(state->managed, receiver, std::move(callback));
  };
  std::erase_if(field.state_->bindings, [](const auto &weak) {
    auto value = weak.lock();
    return !value;
  });
  field.state_->bindings.push_back(binding);
  return binding;
}

/// @endcond
} // namespace detail

namespace detail {
/// @cond INTERNAL
template <class Row>
Description<LineEdit> editLineEditRecipe(EditField<Row, std::string> field,
                                         std::shared_ptr<portals::detail::EditTextBinding> source,
                                         LineEditConfig config) {
  if (!field.state_ || !field.definition_) {
    return detail::Access::cold<LineEdit>(
      [](ManagedScene &, Node<Widget> parent, detail::MountContext &) -> std::expected<Node<LineEdit>, ManagedError> {
        return std::unexpected(detail::error(parent, TreeError::INVALID_ARGUMENT));
      });
  }
  auto description =
    detail::Access::cold<LineEdit>([field, source, options = config.options, policy = config.policy](
                                     ManagedScene &scene, Node<Widget> parent,
                                     detail::MountContext &context) -> std::expected<Node<LineEdit>, ManagedError> {
      auto resolved = field.state_->resolve(scene, context);
      if (!resolved) {
        return std::unexpected(detail::error(parent, resolved.error()));
      }
      const ControlSettings empty;
      const auto &settings = context.controlSettings ? *context.controlSettings : empty;
      auto effective = detail::controlLineEditOptions(settings, options, scene.widthPolicy(), {}, parent);
      if (!effective) {
        return std::unexpected(effective.error());
      }
      effective->editMode = field.state_->options.mode;
      auto placeholder = detail::lineEditPlaceholder(*effective, parent);
      if (!placeholder) {
        return std::unexpected(placeholder.error());
      }
      return scene.emplace<LineEdit>(parent, *effective, policy, std::move(*placeholder));
    });
  config.result = [source,
                   result = std::move(config.result)](LineEditResult receipt) -> std::expected<void, TreeError> {
    auto *entry = static_cast<EditEntry<Row, std::string> *>(source->displayedEntry);
    if (receipt.status == LineEditStatus::CANCELLED && entry && !entry->discarded && !entry->cancelReadPinned) {
      // The existing editor already restored its owning baseline and reset history.
      // Keep admitted backend work; canceling input never withdraws a submitted request.
      const auto previousDraft = std::exchange(entry->draft, {});
      entry->target.baseVersion = entry->text->revision;
      entry->status = entry->pending.empty() ? EditStatus::CLEAN : EditStatus::PENDING;
    }
    return result ? result(std::move(receipt)) : std::expected<void, TreeError>{};
  };
  return detail::Access::controlSetup(
    detail::Access::configuredControl(std::move(description), "LineEdit", true),
    [source, config = std::move(config)](ManagedScene &scene, Node<LineEdit> node, const ControlSettings &) mutable {
      return detail::Access::bindEditText(scene, node, source, std::move(config));
    });
}

/// @endcond
} // namespace detail

/** @param field Current-record string field. @param config Ordinary editor rules and appearance.
 * @return Existing LineEdit runtime with retained target-local draft and history.
 */
template <class Row> Description<LineEdit> lineEdit(EditField<Row, std::string> field, LineEditConfig config = {}) {
  if (!field.state_ || !field.definition_) {
    return detail::editLineEditRecipe(std::move(field), {}, std::move(config));
  }
  auto source = detail::editTextBinding(field);
  return detail::editLineEditRecipe(std::move(field), std::move(source), std::move(config));
}

/** @param reference Ordinary weak editor observation. @param field Exact typed field descriptor.
 * @param config Ordinary editor configuration. @return Retained field editor.
 */
template <class Row>
Description<LineEdit> lineEdit(Ref<LineEdit> reference, EditField<Row, std::string> field, LineEditConfig config = {}) {
  return lineEdit(std::move(field), std::move(config)).reference(std::move(reference));
}

/** @param caption Standard FormField caption. @param field Current-record string field.
 * @param config Existing required/sync/async validation and editor settings.
 * @return Existing FormField composition over the real session-bound LineEdit.
 */
template <class Row>
Description<FormField> formField(std::string_view caption, EditField<Row, std::string> field,
                                 FormFieldConfig config = {}) {
  if (!field.state_ || !field.definition_) {
    return detail::Access::formField(std::string(caption), detail::Access::erase(lineEdit(field)), {});
  }
  auto source = detail::editTextBinding(field);
  field.definition_->asynchronous = static_cast<bool>(config.validateAsync);
  source->validatedByEditor = true;
  const auto required = config.required;
  const auto requiredMessage = config.requiredMessage;
  auto semantic = std::make_shared<std::function<FieldValidation(std::string_view)>>(
    [required, requiredMessage, rule = std::move(config.validate),
     legacy = std::move(config.editor.validate)](std::string_view candidate) {
      if (required && candidate.empty()) {
        return FieldValidation::invalid(requiredMessage);
      }
      auto result = rule ? rule(candidate) : FieldValidation::valid();
      if (result.status == FieldStatus::VALID && legacy && !legacy(candidate)) {
        return FieldValidation::invalid();
      }
      return result;
    });
  config.validate = [semantic](std::string_view candidate) {
    return (*semantic)(candidate);
  };
  config.editor.validate = {};
  field.definition_->validate = [semantic](const std::string &candidate) {
    return (*semantic)(candidate).status == FieldStatus::VALID;
  };
  auto canceledPresentation = std::make_shared<const FieldValidation>();
  config.editor.result = [source, canceledPresentation, result = std::move(config.editor.result)](
                           LineEditResult receipt) -> std::expected<void, TreeError> {
    if (receipt.status == LineEditStatus::CANCELLED && source->displayValidation) {
      auto cleared = source->displayValidation(canceledPresentation);
      if (!cleared) {
        return cleared;
      }
    }
    auto preserved = source->preserveValidation();
    if (!preserved) {
      return preserved;
    }
    return result ? result(std::move(receipt)) : std::expected<void, TreeError>{};
  };
  return detail::Access::formField(
           std::string(caption),
           [field, source](LineEditConfig editor, ControlSettings settings) {
             return detail::Access::erase(
               detail::editLineEditRecipe(field, source, std::move(editor)).settings(std::move(settings)));
           },
           std::move(config))
    .setup([source](ManagedScene &scene, Node<FormField> node) {
      return detail::Access::connectEditValidation(scene, node, source);
    });
}
} // namespace portals::ui
