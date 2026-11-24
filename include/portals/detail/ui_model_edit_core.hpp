// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/model_edit.hpp>
#include <portals/ui_model.hpp>

namespace portals::ui::detail {
/// @cond INTERNAL

struct EditAccess {
  static EditorCommandResult command(ManagedNavigation, std::expected<void, TreeError>, std::source_location);

  static TreeError cause(const ManagedError &error) {
    const auto *tree = std::get_if<TreeError>(&error.cause);
    return tree ? *tree : TreeError::CALLBACK_FAILED;
  }

  template <class Row> static Model<Row> model(const Navigation<Row> &navigation) {
    return navigation.data_ ? navigation.data_->initial : Model<Row>{};
  }

  template <class Row> static Model<Row> model(const std::shared_ptr<portals::detail::ManagedModelSource> &source) {
    Model<Row> result;
    if (!std::dynamic_pointer_cast<portals::detail::ManagedModelSourceOf<Row>>(source)) {
      return result;
    }
    if (auto declaration = source->editDeclaration) {
      result.data_ = std::static_pointer_cast<ModelDeclarationBase>(declaration);
    }
    return result;
  }

  template <class Row> static std::expected<ManagedModel<Row>, TreeError> provisional(const Model<Row> &model) {
    if (!model.data_ || model.data_->kind != ModelDeclarationBase::Kind::COLLECTION) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    auto data = std::static_pointer_cast<ModelDeclaration<Row>>(model.data_);
    auto valid = portals::detail::ManagedModelAccess::validate(data->handle, data->scene);
    return valid ? std::expected<ManagedModel<Row>, TreeError>(data->handle) : std::unexpected(valid.error());
  }

  template <class Row>
  static std::expected<SourceStamp, TreeError>
  sourceSession(const std::shared_ptr<portals::detail::ManagedModelSource> &source) {
    const auto typed = std::dynamic_pointer_cast<portals::detail::ManagedModelSourceOf<Row>>(source);
    if (!typed || typed->retired || !typed->model) {
      return std::unexpected(TreeError::CLOSED);
    }
    return SourceStamp{typed->model->state_->session, 1};
  }

  template <class Row> static std::shared_ptr<OwnedSequence<Row>> sequence(const Model<Row> &model) {
    return model.data_ && model.data_->kind == ModelDeclarationBase::Kind::COLLECTION
             ? std::static_pointer_cast<ModelDeclaration<Row>>(model.data_)->sequence
             : nullptr;
  }

  template <class Row> static NodeId scene(const Model<Row> &model) {
    return model.data_ ? model.data_->scene : NodeId{};
  }

  static std::expected<std::shared_ptr<portals::detail::ManagedNavigationRecord>, TreeError>
  context(const ManagedNavigation &navigation) {
    return navigation.lock();
  }

  template <class Row>
  static std::expected<std::uint64_t, TreeError> replace(const Model<Row> &model, RowId id, std::uint64_t version,
                                                         const Row &value) {
    std::uint64_t acceptedVersion{};
    const typename Model<Row>::Replacement replacement{id, value, version, &acceptedVersion};
    auto result = model.mutate(
      {}, false,
      [&](auto &rows, auto old, std::uint64_t) {
        for (const auto &row : old) {
          rows.emplace_back(row.id, row.id == id ? value : row.value);
        }
      },
      &replacement);
    return result ? std::expected<std::uint64_t, TreeError>(acceptedVersion) : std::unexpected(cause(result.error()));
  }
};

template <class Value> std::expected<std::size_t, TreeError> editValueBytes(const Value &value) {
  if constexpr (std::same_as<Value, std::string>) {
    return sizeof(Value) + value.capacity() + 1;
  } else if constexpr (std::is_trivially_copyable_v<Value>) {
    return sizeof(Value);
  } else {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
}

template <class Value>
std::expected<Value, TreeError> parseEditText(const std::shared_ptr<const EditTextCodec<Value>> &codec,
                                              std::string_view candidate) {
  if (codec && codec->parse) {
    return codec->parse(candidate);
  }
  if constexpr (std::same_as<Value, std::string>) {
    return std::expected<Value, TreeError>(std::in_place, candidate);
  }

  return std::unexpected(TreeError::INVALID_ARGUMENT);
}

template <class Row, class Value> class OwnedFieldEndpoint final : public EditEndpoint<Value> {
public:
  OwnedFieldEndpoint(Model<Row> model, Value Row::*member, EditTarget identity, std::size_t maxBytes,
                     std::optional<std::size_t> maxTextBytes)
      : model_(std::move(model)), member_(member), resource_(identity.resource), identity_(std::move(identity)),
        maxBytes_(maxBytes), maxTextBytes_(maxTextBytes) {
  }

  EditCapabilities capabilities() const noexcept override {
    return {static_cast<bool>(EditAccess::sequence(model_)), true, false, false, true};
  }

  std::expected<EditRecord<Value>, TreeError> read(ModelResource resource, RowId row) override {
    auto managed = EditAccess::provisional(model_);
    if (!managed) {
      return std::unexpected(managed.error());
    }
    if (resource != resource_ || row != identity_.row || !member_) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    auto sequence = EditAccess::sequence(model_);
    if (!sequence || !sequence->current) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    const auto previous = sequence->current;
    const auto location = previous->locateRow(row);
    if (location.state != RowLookupState::FOUND) {
      return std::unexpected(TreeError::STALE_NODE);
    }
    if (sequence->editing) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    if (!sequence->editVersions) {
      sequence->editVersions = std::make_shared<std::vector<std::weak_ptr<OwnedEditVersion>>>(0);
    }
    std::erase_if(*sequence->editVersions, [](const auto &weak) {
      return weak.expired();
    });
    auto version = version_;
    if (!version) {
      for (const auto &weak : *sequence->editVersions) {
        auto entry = weak.lock();
        if (entry && entry->row == row) {
          version = std::move(entry);
          break;
        }
      }
    }
    if (!version) {
      version = std::make_shared<OwnedEditVersion>(OwnedEditVersion{row, previous->revision()});
      sequence->editVersions->push_back(version);
    }
    version_ = version;
    if (cachedRow_ == row && cachedVersion_ == version->version) {
      if (auto cached = cachedValue_.lock()) {
        return EditRecord<Value>{std::move(cached), cachedVersion_};
      }
    }
    if constexpr (std::same_as<Value, std::string>) {
      if (maxTextBytes_ && (previous->rows()[location.index].value.*member_).size() > *maxTextBytes_) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
    }
    auto size = editValueBytes(previous->rows()[location.index].value.*member_);
    if (!size) {
      return std::unexpected(size.error());
    }
    if (*size > maxBytes_) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }
    const auto expectedVersion = version->version;
    auto value = std::make_shared<const Value>(previous->rows()[location.index].value.*member_);
    if (sequence->current != previous || version->version != expectedVersion) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    auto live = EditAccess::provisional(model_);
    if (!live) {
      return std::unexpected(live.error());
    }
    cachedValue_ = value;
    cachedVersion_ = expectedVersion;
    cachedRow_ = row;
    return EditRecord<Value>{std::move(value), expectedVersion};
  }

  std::expected<EditReceipt, TreeError> submit(EditRequest<Value> request) override {
    if (!request.value || request.target.resource != resource_ || request.target.session != identity_.session ||
        request.target.sourceSession != identity_.sourceSession || request.target.row != identity_.row ||
        request.target.field != identity_.field || !request.target.baseVersion || !request.target.sequence) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    auto current = read(resource_, request.target.row);
    if (!current) {
      return EditReceipt{request.target,
                         current.error() == TreeError::STALE_NODE ? EditStatus::TARGET_MISSING : EditStatus::CLOSED, 0,
                         current.error()};
    }
    if (current->version != request.target.baseVersion) {
      return EditReceipt{request.target, EditStatus::CONFLICT};
    }
    auto sequence = EditAccess::sequence(model_);
    const auto previous = sequence->current;
    const auto location = previous->locateRow(request.target.row);
    Row candidate(previous->rows()[location.index].value);
    candidate.*member_ = *request.value;
    auto applied = EditAccess::replace(model_, request.target.row, request.target.baseVersion, candidate);
    if (!applied) {
      return EditReceipt{request.target,
                         applied.error() == TreeError::WOULD_BLOCK ? EditStatus::CONFLICT : EditStatus::REJECTED, 0,
                         applied.error()};
    }
    return EditReceipt{request.target, EditStatus::ACCEPTED, *applied};
  }

  std::expected<std::size_t, TreeError> ownedBytes(const Value &value) const override {
    return editValueBytes(value);
  }

private:
  Model<Row> model_;
  Value Row::*member_;
  ModelResource resource_;
  EditTarget identity_;
  std::size_t maxBytes_;
  std::optional<std::size_t> maxTextBytes_;
  std::shared_ptr<OwnedEditVersion> version_;
  RowId cachedRow_;
  std::weak_ptr<const Value> cachedValue_;
  std::uint64_t cachedVersion_{};
};

template <class Row, class Value> class ReadOnlyFieldEndpoint final : public EditEndpoint<Value> {
public:
  ReadOnlyFieldEndpoint(Model<Row> model, Value Row::*member, ModelResource resource, std::size_t maxBytes,
                        std::optional<std::size_t> maxTextBytes)
      : model_(std::move(model)), member_(member), resource_(resource), maxBytes_(maxBytes),
        maxTextBytes_(maxTextBytes) {
  }

  EditCapabilities capabilities() const noexcept override {
    return {};
  }

  std::expected<EditRecord<Value>, TreeError> read(ModelResource resource, RowId row) override {
    if (resource != resource_) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    auto handle = EditAccess::provisional(model_);
    if (!handle) {
      return std::unexpected(handle.error());
    }
    auto snapshot = portals::detail::ManagedModelAccess::snapshot(*handle, EditAccess::scene(model_));
    if (!snapshot) {
      return std::unexpected(snapshot.error());
    }
    const auto found = (*snapshot)->locateRow(row);
    if (found.state != RowLookupState::FOUND) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    auto rows = (*snapshot)->read({found.index, 1});
    if (rows.state != RangeState::READY || rows.rows.size() != 1 || rows.rows.front().id != row) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    const auto &value = rows.rows.front().value.*member_;
    if constexpr (std::same_as<Value, std::string>) {
      if (maxTextBytes_ && value.size() > *maxTextBytes_) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
    }
    auto size = editValueBytes(value);
    if (!size || *size > maxBytes_) {
      return std::unexpected(size ? TreeError::NODE_LIMIT : size.error());
    }

    return EditRecord<Value>{std::make_shared<const Value>(value), (*snapshot)->revision()};
  }

  std::expected<EditReceipt, TreeError> submit(EditRequest<Value> request) override {
    return EditReceipt{request.target, EditStatus::READ_ONLY};
  }

  std::expected<std::size_t, TreeError> ownedBytes(const Value &value) const override {
    return editValueBytes(value);
  }

private:
  Model<Row> model_;
  Value Row::*member_;
  ModelResource resource_;
  std::size_t maxBytes_;
  std::optional<std::size_t> maxTextBytes_;
};

inline std::shared_ptr<portals::detail::EditTextBinding>
selectEditBinding(const std::vector<std::weak_ptr<portals::detail::EditTextBinding>> &bindings, const void *entry) {
  std::shared_ptr<portals::detail::EditTextBinding> chosen;
  for (const auto &weak : bindings) {
    auto binding = weak.lock();
    if (!binding || binding->closed || binding->displayedEntry != entry) {
      continue;
    }
    if (!chosen || (binding->validatedByEditor && !chosen->validatedByEditor)) {
      chosen = std::move(binding);
    }
  }
  return chosen;
}

inline bool preferredEditBinding(const std::vector<std::weak_ptr<portals::detail::EditTextBinding>> &bindings,
                                 const std::shared_ptr<portals::detail::EditTextBinding> &candidate) {
  return selectEditBinding(bindings, candidate->displayedEntry) == candidate;
}

// Preserve registration order: the first eligible live FormField wins over plain editors.
// A cold recipe has no displayed draft, and a removed editor closes its binding.
inline std::shared_ptr<portals::detail::EditTextBinding>
selectEditPolicy(const std::vector<std::weak_ptr<portals::detail::EditTextBinding>> &bindings, const void *definition) {
  std::shared_ptr<portals::detail::EditTextBinding> chosen;
  for (const auto &weak : bindings) {
    auto binding = weak.lock();
    if (!binding || binding->closed || !binding->displayed || binding->definition != definition ||
        !binding->validateForCommit) {
      continue;
    }
    if (!chosen || (binding->validatedByEditor && !chosen->validatedByEditor)) {
      chosen = std::move(binding);
    }
  }

  return chosen;
}

template <class Row> struct EditState;

struct EditDefinitionBase {
  virtual ~EditDefinitionBase() = default;
  EditFieldKey key;
  std::function<std::expected<void, TreeError>(const NavigationPosition &,
                                               const std::shared_ptr<portals::detail::ManagedModelSource> &)>
    prepare;
};

template <class Row, class Value> struct EditDefinition final : EditDefinitionBase {
  Value Row::*member{};
  std::shared_ptr<EditEndpoint<Value>> endpoint;
  std::function<bool(const Value &)> validate;
  std::shared_ptr<const EditTextCodec<Value>> textCodec;
  std::optional<std::size_t> maxTextBytes;
  bool active{};
};

struct EditEntryBase {
  virtual ~EditEntryBase() = default;
  EditTarget target;
  EditStatus status{EditStatus::CLEAN};
  EditReceipt lastReceipt;
  std::shared_ptr<const FieldValidation> validation;
  std::size_t validationCharge{};
  std::size_t charge{};
  bool discarded{};
  bool admissionRefused{};
  bool cancelReadPinned{};
  virtual bool hasDraft() const noexcept = 0;
  virtual bool pristine() const noexcept = 0;
  virtual std::expected<EditReceipt, TreeError> commit() = 0;
  virtual std::expected<void, TreeError> prepareCancel() = 0;
  virtual std::expected<void, TreeError> cancel(bool discard) = 0;
  virtual std::expected<bool, TreeError> reply(const EditReceipt &) = 0;
  virtual void rebase(std::uint64_t oldVersion, std::uint64_t newVersion) = 0;
};

template <class Row, class Value> struct EditEntry final : EditEntryBase {
  struct Pending {
    EditTarget target;
    std::shared_ptr<const Value> value;
    std::uint64_t generation;
  };

  std::weak_ptr<EditState<Row>> owner;
  std::shared_ptr<EditDefinition<Row, Value>> definition;
  std::shared_ptr<EditEndpoint<Value>> endpoint;
  std::shared_ptr<const Value> accepted;
  std::shared_ptr<const Value> draft;
  std::unique_ptr<portals::detail::EditTextDraft> text;
  std::shared_ptr<const PropertyChange<std::string>> textSnapshot;
  std::vector<Pending> pending = std::vector<Pending>(0);
  std::uint64_t generation{};
  std::size_t textCharge{};
  std::size_t snapshotCharge{};
  std::uint64_t acceptedSequence{};
  std::uint64_t observedVersion{};  // Version paired with the admitted accepted value.
  std::uint64_t witnessedVersion{}; // Read/receipt watermark, including values refused by the budget.
  std::uint64_t validatedGeneration{std::numeric_limits<std::uint64_t>::max()};
  bool requiresAsyncValidation{};

  std::expected<std::string, TreeError> format(const Value &value) const {
    if (definition->textCodec && definition->textCodec->format) {
      return definition->textCodec->format(value);
    }
    if constexpr (std::same_as<Value, std::string>) {
      return std::expected<std::string, TreeError>(std::in_place, value);
    }
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  std::expected<Value, TreeError> parse(std::string_view value) const {
    return parseEditText(definition->textCodec, value);
  }

  std::expected<std::size_t, TreeError> retainedCharge(const Value *candidate, std::size_t nextTextCharge,
                                                       bool copiesCandidate = false,
                                                       std::optional<std::size_t> pendingCapacity = {},
                                                       const Value *nextAccepted = nullptr,
                                                       std::optional<std::size_t> nextSnapshotCharge = {}) const {
    auto state = owner.lock();
    if (!state) {
      return std::unexpected(TreeError::CLOSED);
    }
    const auto endpointOwner = endpoint;
    if (discarded || !endpointOwner) {
      return std::unexpected(TreeError::STALE_NODE);
    }
    if (state->accounting) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }

    struct Accounting {
      bool &active;

      explicit Accounting(bool &value) : active(value) {
        active = true;
      }

      ~Accounting() {
        active = false;
      }
    } accounting(state->accounting);

    const auto acceptedOwner = accepted;
    const auto draftOwner = draft;
    const auto originalTarget = target;
    const auto originalGeneration = text ? text->generation : generation;
    const auto originalPendingCount = pending.size();
    auto unchanged = [&]() -> std::expected<void, TreeError> {
      if (discarded || endpoint != endpointOwner || state->endpointClosed) {
        return std::unexpected(TreeError::CLOSED);
      }
      if (target != originalTarget || accepted != acceptedOwner || draft != draftOwner ||
          (text ? text->generation : generation) != originalGeneration || pending.size() != originalPendingCount) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
      return {};
    };
    const auto limit = state->options.maxRetainedBytes;
    const auto capacity = pendingCapacity.value_or(pending.capacity());
    std::size_t total = sizeof(*this);
    auto add = [&](std::size_t count) -> bool {
      if (total > limit || count > limit - total) {
        return false;
      }
      total += count;
      return true;
    };
    if (!add(nextTextCharge) || !add(validationCharge) || !add(nextSnapshotCharge.value_or(snapshotCharge)) ||
        capacity > limit / sizeof(Pending) || !add(capacity * sizeof(Pending))) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }
    auto addValue = [&](const Value *value) -> std::expected<void, TreeError> {
      if (!value) {
        return {};
      }
      auto size = endpointOwner->ownedBytes(*value);
      auto live = unchanged();
      if (!live) {
        return live;
      }
      if (!size) {
        return std::unexpected(size.error());
      }
      return add(*size) ? std::expected<void, TreeError>{} : std::unexpected(TreeError::NODE_LIMIT);
    };
    const auto *acceptedValue = nextAccepted ? nextAccepted : acceptedOwner.get();
    auto base = addValue(acceptedValue);
    if (!base) {
      return std::unexpected(base.error());
    }
    if (candidate && (copiesCandidate || candidate != acceptedValue)) {
      auto added = addValue(candidate);
      if (!added) {
        return std::unexpected(added.error());
      }
    }
    for (std::size_t index = 0; index < pending.size(); ++index) {
      const auto pendingOwner = pending[index].value;
      const auto *value = pendingOwner.get();
      if (value == acceptedValue || (!copiesCandidate && value == candidate)) {
        continue;
      }
      bool counted = false;
      for (std::size_t previous = 0; previous < index; ++previous) {
        counted = counted || pending[previous].value.get() == value;
      }
      if (!counted) {
        auto added = addValue(value);
        if (!added) {
          return std::unexpected(added.error());
        }
      }
    }
    return total;
  }

  std::expected<void, TreeError> refresh() {
    admissionRefused = false;
    if (discarded || !endpoint) {
      return std::unexpected(TreeError::STALE_NODE);
    }
    if (cancelReadPinned) {
      return {};
    }
    const auto state = owner.lock();
    if (!state) {
      return std::unexpected(TreeError::CLOSED);
    }
    if (state->accounting) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    const auto endpointOwner = endpoint;
    const auto retainedDraft = draft;
    auto value = endpointOwner->read(target.resource, target.row);
    if (discarded || endpoint != endpointOwner) {
      return std::unexpected(TreeError::STALE_NODE);
    }
    if (!value) {
      if (value.error() == TreeError::STALE_NODE) {
        status = EditStatus::TARGET_MISSING;
      }
      return std::unexpected(value.error());
    }
    if (!value->value || !value->version) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }
    if (value->version < std::max(observedVersion, witnessedVersion)) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    // A valid read witnesses the version even when retaining its payload is refused.
    witnessedVersion = value->version;
    auto localFailure = [&](TreeError error) -> std::expected<void, TreeError> {
      admissionRefused = error == TreeError::NODE_LIMIT;
      return std::unexpected(error);
    };
    auto nextSnapshot = textSnapshot;
    auto nextSnapshotBytes = snapshotCharge;
    if constexpr (std::same_as<Value, std::string>) {
      if (!definition->textCodec && (!nextSnapshot || nextSnapshot->revision != value->version)) {
        if (value->value->capacity() >
            std::numeric_limits<std::size_t>::max() - sizeof(PropertyChange<std::string>) - 1) {
          return localFailure(TreeError::NODE_LIMIT);
        }
        nextSnapshotBytes = sizeof(PropertyChange<std::string>) + value->value->capacity() + 1;
      }
    }
    // Refuse known oversized input before copying the UI's owning string snapshot.
    if (charge) {
      auto estimated =
        retainedCharge(retainedDraft.get(), textCharge, false, {}, value->value.get(), nextSnapshotBytes);
      if (!estimated) {
        return localFailure(estimated.error());
      }
      auto checked = state->checkCredit(*this, *estimated, false);
      if (!checked) {
        return localFailure(checked.error());
      }
    }
    if constexpr (std::same_as<Value, std::string>) {
      if (!definition->textCodec && (!nextSnapshot || nextSnapshot->revision != value->version)) {
        nextSnapshot = std::make_shared<const PropertyChange<std::string>>(
          PropertyChange<std::string>{*value->value, value->version});
        nextSnapshotBytes = sizeof(PropertyChange<std::string>) + nextSnapshot->value.capacity() + 1;
      }
    }
    if (definition->textCodec && (!nextSnapshot || nextSnapshot->revision != value->version)) {
      auto formatted = format(*value->value);
      if (!formatted) {
        return localFailure(formatted.error());
      }
      nextSnapshot =
        std::make_shared<const PropertyChange<std::string>>(PropertyChange<std::string>{*formatted, value->version});
      nextSnapshotBytes = sizeof(PropertyChange<std::string>) + nextSnapshot->value.capacity() + 1;
    }
    if (charge) {
      auto nextCharge =
        retainedCharge(retainedDraft.get(), textCharge, false, {}, value->value.get(), nextSnapshotBytes);
      if (!nextCharge) {
        return localFailure(nextCharge.error());
      }
      if (discarded || endpoint != endpointOwner || state->endpointClosed) {
        return std::unexpected(TreeError::CLOSED);
      }
      if (value->version < witnessedVersion) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
      auto admitted = state->admit(*this, *nextCharge, false);
      if (!admitted) {
        return localFailure(admitted.error());
      }
    }
    const auto previousAccepted = std::exchange(accepted, value->value);
    if (!draft && (!text || !text->dirty) && pending.empty()) {
      target.baseVersion = value->version;
    }
    observedVersion = value->version;
    textSnapshot = std::move(nextSnapshot);
    snapshotCharge = nextSnapshotBytes;
    return {};
  }

  std::expected<void, TreeError> prepareCancel() override {
    if (cancelReadPinned) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    return refresh();
  }

  bool hasDraft() const noexcept override {
    return draft || (text && text->dirty);
  }

  bool pristine() const noexcept override {
    return !discarded && !hasDraft() && pending.empty() && status != EditStatus::DIRTY &&
           (!text || !text->history->operationCount()) && (!validation || validation->status == FieldStatus::UNCHECKED);
  }

  std::expected<void, TreeError> setRetainedText(const Value &typedValue, const std::string &value) {
    const auto state = owner.lock();
    if (!state || state->endpointClosed || discarded || !text) {
      return std::unexpected(TreeError::CLOSED);
    }
    if (text->generation == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
    }
    auto precharge = retainedCharge(&typedValue, textCharge, true);
    if (!precharge) {
      return std::unexpected(precharge.error());
    }
    auto checked = state->checkCredit(*this, *precharge);
    if (!checked) {
      return checked;
    }
    const auto beforeGeneration = text->generation;
    auto prepared = text->history->prepareEdit({std::string(text->text.bytes()), text->cursor, text->anchor},
                                               {value, value.size(), value.size()}, TextHistoryKind::PROGRAMMATIC);
    if (!prepared) {
      return std::unexpected(prepared.error() == TextHistoryError::NO_MEMORY ? TreeError::NO_MEMORY
                                                                             : TreeError::INVALID_ARGUMENT);
    }
    std::unique_ptr<GraphemeText> mask;
    if (text->mask) {
      auto masked =
        GraphemeText::from(std::string(prepared->text()->graphemes().size(), '*'), text->text.policy(), text->limits);
      if (!masked) {
        return std::unexpected(TreeError::INVALID_ARGUMENT);
      }
      mask = std::make_unique<GraphemeText>(std::move(*masked));
    }
    auto candidate = std::make_shared<const Value>(typedValue);
    auto nextTextCharge =
      portals::detail::EditStorageAccess::draftBytes(*prepared->text(), mask.get(), prepared->retainedBytes());
    if (!nextTextCharge) {
      return std::unexpected(nextTextCharge.error());
    }
    auto finalCharge = retainedCharge(candidate.get(), *nextTextCharge);
    if (!finalCharge) {
      return std::unexpected(finalCharge.error());
    }
    auto ready = text->history->checkPrepared(*prepared);
    if (!ready || beforeGeneration != text->generation) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    if (discarded || state->endpointClosed) {
      return std::unexpected(TreeError::CLOSED);
    }
    auto admitted = state->admit(*this, *finalCharge);
    if (!admitted) {
      return admitted;
    }
    const auto cursor = prepared->cursor();
    const auto anchor = prepared->anchor();
    prepared->takeText(text->text);
    auto committed = text->history->commit(std::move(*prepared));
    if (!committed) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    text->mask = std::move(mask);
    text->cursor = cursor;
    text->anchor = anchor;
    text->dirty = true;
    ++text->generation;
    textCharge = *nextTextCharge;
    draft = std::move(candidate);
    status = EditStatus::DIRTY;
    return {};
  }

  std::expected<EditReceipt, TreeError> commit() override;
  std::expected<void, TreeError> cancel(bool discard) override;
  std::expected<bool, TreeError> reply(const EditReceipt &receipt) override;

  void rebase(std::uint64_t oldVersion, std::uint64_t newVersion) override {
    if (discarded || !endpoint || !endpoint->capabilities().recordVersion || target.baseVersion != oldVersion ||
        status == EditStatus::CONFLICT || !pending.empty() || newVersion < witnessedVersion) {
      return;
    }
    target.baseVersion = newVersion;
    observedVersion = std::max(observedVersion, newVersion);
    witnessedVersion = std::max(witnessedVersion, newVersion);
    if (text) {
      text->revision = newVersion;
    }
  }
};

template <class Row>
struct EditState final : portals::detail::EditNavigationParticipant, std::enable_shared_from_this<EditState<Row>> {
  explicit EditState(Navigation<Row> value, EditOptions config) : navigation(std::move(value)), options(config) {
  }

  Navigation<Row> navigation;
  EditOptions options;
  ManagedNavigation managed;
  std::vector<std::shared_ptr<EditDefinitionBase>> definitions = std::vector<std::shared_ptr<EditDefinitionBase>>(0);
  std::vector<std::shared_ptr<EditEntryBase>> entries = std::vector<std::shared_ptr<EditEntryBase>>(0);
  std::vector<std::weak_ptr<portals::detail::EditTextBinding>> bindings =
    std::vector<std::weak_ptr<portals::detail::EditTextBinding>>(0);
  std::shared_ptr<EditCommitResult> last;
  std::uint64_t sequence{};
  SourceStamp writeSession;
  std::uint64_t nextLocal{1};
  std::uint64_t nextGeneration{1};
  std::size_t bytes{};
  const std::thread::id ownerThread{std::this_thread::get_id()};
  bool preparingTargets{};
  bool accounting{};
  bool registered{};
  bool endpointClosed{};

  std::expected<void, TreeError> resolve(ManagedScene &scene, MountContext &context) {
    auto nav = ModelAccess::resolve(scene, navigation, context);
    if (!nav) {
      return std::unexpected(EditAccess::cause(nav.error()));
    }
    auto previous = EditAccess::context(managed);
    auto next = EditAccess::context(*nav);
    if (!next) {
      return std::unexpected(next.error());
    }
    if (!previous || previous->get() != next->get()) {
      registered = false;
      endpointClosed = false;
    }
    managed = *nav;
    const auto existingEntries = entries.size();
    const auto wasRegistered = registered;
    auto admitted = registerParticipant();
    if (!admitted) {
      return admitted;
    }
    if (!wasRegistered) {
      context.rollback.push_back([weak = std::weak_ptr(this->shared_from_this()), existingEntries] {
        if (auto state = weak.lock()) {
          state->registered = false;
          while (state->entries.size() > existingEntries) {
            auto retired = std::move(state->entries.back());
            state->entries.pop_back();
            state->bytes -= retired->charge;
          }
        }
      });
    }
    return {};
  }

  std::expected<void, TreeError> registerParticipant() {
    if (registered) {
      return {};
    }
    auto nav = navigation.managed();
    if (nav) {
      managed = *nav;
    }
    auto record = EditAccess::context(managed);
    if (!record) {
      return std::unexpected(record.error());
    }
    if (!writeSession.instance) {
      auto stamp = portals::detail::windowSourceStamp();
      if (!stamp) {
        return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
      }
      writeSession = *stamp;
    }
    if ((*record)->admittingEdit) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    if (!(*record)->editors) {
      (*record)->editors = std::make_unique<std::vector<std::weak_ptr<portals::detail::EditNavigationParticipant>>>(0);
    }
    std::erase_if(*(*record)->editors, [](const auto &weak) {
      return weak.expired();
    });
    (*record)->editors->push_back(this->shared_from_this());
    registered = true;
    return {};
  }

  std::expected<NavigationPosition, TreeError> current() {
    if (ownerThread != std::this_thread::get_id()) {
      return std::unexpected(TreeError::WRONG_THREAD);
    }
    if (endpointClosed) {
      return std::unexpected(TreeError::CLOSED);
    }
    auto admitted = registerParticipant();
    if (!admitted) {
      return std::unexpected(admitted.error());
    }
    auto channel = managed.current();
    if (!channel) {
      return std::unexpected(channel.error());
    }
    auto snapshot = channel->snapshot();

    if (!snapshot) {
      return std::unexpected(snapshot.error());
    }

    auto position = (*snapshot)->value;

    // NEAREST describes how navigation selected this already-resolved current row. A new editor
    // addresses that concrete RowId exactly; it never resolves or rewrites an older retained target.
    // Keep the navigation channel/bookmark diagnostic unchanged and retain exact entry/endpoint checks.
    if (position.kind == PositionKind::ITEM && position.row && position.resolution == PositionResolution::NEAREST) {
      position.resolution = PositionResolution::EXACT;
    }

    return position;
  }

  void prunePristine(const NavigationPosition &target, SourceStamp sourceIdentity) {
    std::vector<std::shared_ptr<EditEntryBase>> retired(0);
    retired.reserve(entries.size());
    for (auto &entry : entries) {
      if ((!entry->pristine() && !entry->discarded) ||
          (entry->target.resource == target.resource && target.row == entry->target.row &&
           entry->target.sourceSession == sourceIdentity && !entry->discarded)) {
        continue;
      }
      const auto visible = std::ranges::any_of(bindings, [&](const auto &weak) {
        auto binding = weak.lock();
        return binding && (binding->displayedEntry == entry.get() || binding->preparedEntry == entry.get());
      });
      if (!visible) {
        bytes -= entry->charge;
        retired.push_back(std::move(entry));
      }
    }
    std::erase(entries, std::shared_ptr<EditEntryBase>{});
  }

  std::expected<void, TreeError> checkCredit(const EditEntryBase &entry, std::size_t charge, bool retain = true) const {
    if (retain && entry.pristine()) {
      const auto retained = std::ranges::count_if(entries, [](const auto &value) {
        return !value->discarded && !value->pristine();
      });
      if (static_cast<std::size_t>(retained) >= options.maxRetainedFields) {
        return std::unexpected(TreeError::NODE_LIMIT);
      }
    }
    if (entry.charge > bytes || charge > options.maxRetainedBytes ||
        bytes - entry.charge > options.maxRetainedBytes - charge) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }
    return {};
  }

  std::expected<void, TreeError> admit(EditEntryBase &entry, std::size_t charge, bool retain = true) {
    auto checked = checkCredit(entry, charge, retain);
    if (!checked) {
      return checked;
    }
    bytes = bytes - entry.charge + charge;
    entry.charge = charge;
    return {};
  }

  template <class Value>
  std::expected<std::shared_ptr<EditEntry<Row, Value>>, TreeError>
  entry(const std::shared_ptr<EditDefinition<Row, Value>> &definition, const NavigationPosition &position,
        const std::shared_ptr<portals::detail::ManagedModelSource> &nextSource = {}) {
    definition->active = true;
    if (!position.row || position.kind != PositionKind::ITEM || position.resolution != PositionResolution::EXACT) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    auto record = EditAccess::context(managed);
    if (!record) {
      return std::unexpected(record.error());
    }
    const auto actualSource = nextSource ? nextSource : (*record)->model.lock();
    auto incarnation = EditAccess::sourceSession<Row>(actualSource);
    if (!incarnation) {
      return std::unexpected(incarnation.error());
    }
    for (const auto &base : entries) {
      if (base->target.resource == position.resource && base->target.row == *position.row &&
          base->target.field == definition->key && base->target.sourceSession == *incarnation && !base->discarded) {
        auto value = std::dynamic_pointer_cast<EditEntry<Row, Value>>(base);
        return value ? std::expected<std::shared_ptr<EditEntry<Row, Value>>, TreeError>(std::move(value))
                     : std::unexpected(TreeError::INVALID_ARGUMENT);
      }
    }
    if (accounting) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    if (preparingTargets && !nextSource) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    if (!preparingTargets) {
      prunePristine(position, *incarnation);
    }
    auto value = std::make_shared<EditEntry<Row, Value>>();
    value->owner = this->shared_from_this();
    value->definition = definition;
    value->target = {position.resource, writeSession, *position.row, definition->key};
    value->target.sourceSession = *incarnation;
    if (definition->endpoint) {
      value->endpoint = definition->endpoint;
    } else {
      auto model = EditAccess::model<Row>(actualSource);
      if (EditAccess::sequence(model)) {
        value->endpoint = std::make_shared<OwnedFieldEndpoint<Row, Value>>(
          model, definition->member, value->target, options.maxRetainedBytes, definition->maxTextBytes);
      } else {
        value->endpoint = std::make_shared<ReadOnlyFieldEndpoint<Row, Value>>(
          model, definition->member, position.resource, options.maxRetainedBytes, definition->maxTextBytes);
      }
    }
    auto ready = value->refresh();
    if (!ready) {
      return std::unexpected(ready.error());
    }
    auto size = value->endpoint->ownedBytes(*value->accepted);
    if (!size) {
      return std::unexpected(size.error());
    }
    if (options.maxRetainedBytes < sizeof(*value) || *size > options.maxRetainedBytes - sizeof(*value)) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }
    entries.reserve(entries.size() + 1);
    auto initialCharge = value->retainedCharge(nullptr, 0);
    if (!initialCharge) {
      return std::unexpected(initialCharge.error());
    }
    auto credit = admit(*value, *initialCharge, false);
    if (!credit) {
      return std::unexpected(credit.error());
    }
    entries.push_back(value);
    return value;
  }

  void accepted(const EditReceipt &receipt) {
    for (const auto &entry : entries) {
      if (entry->target.resource == receipt.target.resource && entry->target.session == receipt.target.session &&
          entry->target.row == receipt.target.row && entry->target.sourceSession == receipt.target.sourceSession) {
        entry->rebase(receipt.target.baseVersion, receipt.acceptedVersion);
      }
    }
  }

  std::expected<void, TreeError>
  beforeTransition(const NavigationPosition *next,
                   const std::shared_ptr<portals::detail::ManagedModelSource> &source) override {
    if (accounting) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    const auto retainedBindings = bindings;
    const auto retainedDefinitions = definitions;
    for (const auto &weak : retainedBindings) {
      auto binding = weak.lock();
      if (binding && !binding->closed && binding->preserveValidation) {
        auto preserved = binding->preserveValidation();
        if (!preserved) {
          return preserved;
        }
      }
    }
    if (next && next->row && next->resolution == PositionResolution::EXACT) {
      auto incarnation = EditAccess::sourceSession<Row>(source);
      if (!incarnation) {
        return std::unexpected(incarnation.error());
      }
      prunePristine(*next, *incarnation);
      const auto previousCount = entries.size();

      struct Preparing {
        bool &active;

        ~Preparing() {
          active = false;
        }
      } guard{preparingTargets};

      preparingTargets = true;
      auto prepareTargets = [&]() -> std::expected<void, TreeError> {
        for (const auto &definition : retainedDefinitions) {
          auto admitted = definition->prepare(*next, source);
          if (!admitted) {
            return admitted;
          }
        }
        for (const auto &weak : retainedBindings) {
          auto binding = weak.lock();
          if (binding && !binding->closed && binding->preflight) {
            auto admitted = binding->preflight(*next, source);
            if (!admitted) {
              return admitted;
            }
          }
        }
        return {};
      };
      auto admitted = prepareTargets();
      if (!admitted) {
        while (entries.size() > previousCount) {
          auto retired = std::move(entries.back());
          entries.pop_back();
          bytes -= retired->charge;
        }
        return admitted;
      }
    }
    if (options.mode != EditMode::ON_COMPLETION) {
      return {};
    }
    for (const auto &weak : retainedBindings) {
      auto binding = weak.lock();
      if (!binding || binding->closed || !binding->displayedEntry || !binding->displayed->dirty ||
          !preferredEditBinding(retainedBindings, binding)) {
        continue;
      }
      auto result = binding->complete(false);
      if (!result) {
        return std::unexpected(result.error());
      }
      if (result->status != LineEditStatus::COMMITTED && result->status != LineEditStatus::EDITED) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
    }
    auto position = current();
    if (!position) {
      return std::unexpected(position.error());
    }
    const auto retainedEntries = entries;
    for (const auto &entry : retainedEntries) {
      if (entry->target.resource != position->resource || position->row != entry->target.row || !entry->hasDraft()) {
        continue;
      }
      auto result = entry->commit();
      if (!result) {
        return std::unexpected(result.error());
      }
      if (result->status != EditStatus::CLEAN && result->status != EditStatus::ACCEPTED) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
    }
    return {};
  }

  void closed() noexcept override {
    endpointClosed = true;
  }
};

template <class Row, class Value> std::expected<EditReceipt, TreeError> EditEntry<Row, Value>::commit() {
  admissionRefused = false;
  auto state = owner.lock();
  if (!state || state->endpointClosed) {
    return std::unexpected(TreeError::CLOSED);
  }
  if (state->accounting) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }
  if (discarded || !endpoint) {
    return EditReceipt{target, EditStatus::TARGET_MISSING};
  }
  const auto endpointOwner = endpoint;
  const auto capabilities = endpointOwner->capabilities();
  if (!capabilities.writable) {
    return EditReceipt{target, EditStatus::READ_ONLY};
  }
  if (text && text->dirty && !definition->textCodec) {
    if constexpr (std::same_as<Value, std::string>) {
      if (!draft || *draft != text->text.bytes()) {
        draft = std::make_shared<const Value>(text->text.bytes());
      }
    }
  } else if (text && text->dirty) {
    auto parsed = parse(text->text.bytes());
    if (!parsed) {
      if (parsed.error() != TreeError::INVALID_ARGUMENT) {
        return std::unexpected(parsed.error());
      }
      status = EditStatus::INVALID;

      return EditReceipt{target, status};
    }
    auto candidate = std::make_shared<const Value>(*parsed);
    auto chargedBytes = retainedCharge(candidate.get(), textCharge);
    if (!chargedBytes) {
      return std::unexpected(chargedBytes.error());
    }
    auto admitted = state->admit(*this, *chargedBytes);
    if (!admitted) {
      return std::unexpected(admitted.error());
    }
    draft = std::move(candidate);
  }
  if (!draft) {
    return status == EditStatus::ACCEPTED ? lastReceipt : EditReceipt{target, status};
  }
  const auto candidate = draft;
  const auto previousGeneration = text ? text->generation : generation;
  const auto previousText = text.get();
  const auto previousTarget = target;
  const bool editorValidated = text && validatedGeneration == text->generation;
  const auto policy = selectEditPolicy(state->bindings, definition.get());
  if (policy) {
    requiresAsyncValidation = policy->asynchronous;
  }
  // An admitted async draft keeps its approval barrier after widget removal. A replacement
  // mounted editor may supply a new policy, but a cold recipe cannot authorize publication.
  if (requiresAsyncValidation && !editorValidated) {
    return EditReceipt{target, EditStatus::REJECTED, 0, TreeError::WOULD_BLOCK};
  }
  bool policyValidated{};
  if (policy && !editorValidated) {
    std::optional<std::string> formatted;
    std::string_view bytes;
    if (text) {
      // Semantic validators may reenter the typed setter and replace this retained draft.
      // Keep the entire candidate alive independently of the entry until validation returns.
      formatted.emplace(text->text.bytes());
      bytes = *formatted;
    } else if constexpr (std::same_as<Value, std::string>) {
      if (!definition->textCodec) {
        bytes = *candidate;
      } else {
        auto value = format(*candidate);
        if (!value) {
          return std::unexpected(value.error());
        }
        formatted.emplace(*value);
        bytes = *formatted;
      }
    } else {
      auto value = format(*candidate);
      if (!value) {
        return std::unexpected(value.error());
      }
      formatted.emplace(*value);
      bytes = *formatted;
    }
    for (const auto *validate : {&policy->validateBeforeParse, &policy->validateForCommit}) {
      if (!*validate) {
        continue;
      }
      auto checked = (*validate)(bytes);
      if (state->endpointClosed || discarded || endpoint != endpointOwner) {
        return std::unexpected(TreeError::CLOSED);
      }
      // Check supersession before interpreting a result or entering the next parsing phase.
      // A reentrant inner publication owns its status and must not be overwritten by this call.
      if (policy->closed || draft != candidate || text.get() != previousText || target != previousTarget ||
          previousGeneration != (text ? text->generation : generation)) {
        return EditReceipt{previousTarget, EditStatus::CONFLICT};
      }
      if (!checked) {
        return std::unexpected(checked.error());
      }
      if (!*checked) {
        status = EditStatus::INVALID;
        return EditReceipt{target, status};
      }
    }
    policyValidated = true;
  }
  const bool modelValidated = editorValidated || (policyValidated && policy->validatedByEditor);
  const auto rule = definition->validate;
  const bool modelValid = !rule || modelValidated || rule(*candidate);
  if (state->endpointClosed || discarded || endpoint != endpointOwner) {
    return std::unexpected(TreeError::CLOSED);
  }
  if (draft != candidate || text.get() != previousText || target != previousTarget ||
      previousGeneration != (text ? text->generation : generation)) {
    return EditReceipt{previousTarget, EditStatus::CONFLICT};
  }
  if (!modelValid) {
    status = EditStatus::INVALID;
    return EditReceipt{target, status};
  }
  if (!pending.empty() && (!capabilities.conditional || !capabilities.ordered)) {
    status = EditStatus::PENDING;
    return EditReceipt{pending.back().target, status};
  }
  if (state->sequence == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
  }
  auto staged = pending;
  staged.reserve(staged.size() + 1);
  auto pendingCharge = retainedCharge(draft.get(), textCharge, false, staged.capacity());
  if (!pendingCharge) {
    admissionRefused = pendingCharge.error() == TreeError::NODE_LIMIT;
    return std::unexpected(pendingCharge.error());
  }
  if (discarded || endpoint != endpointOwner || state->endpointClosed) {
    return std::unexpected(TreeError::CLOSED);
  }
  if (draft != candidate || previousGeneration != (text ? text->generation : generation)) {
    return EditReceipt{target, EditStatus::CONFLICT};
  }
  auto credited = state->admit(*this, *pendingCharge);
  if (!credited) {
    admissionRefused = credited.error() == TreeError::NODE_LIMIT;
    return std::unexpected(credited.error());
  }
  EditTarget request = target;
  request.sequence = ++state->sequence;
  staged.push_back(Pending{request, candidate, previousGeneration});
  pending.swap(staged);
  std::expected<EditReceipt, TreeError> receipt;
  try {
    receipt = endpointOwner->submit(EditRequest<Value>{request, candidate});
  } catch (const std::bad_alloc &) {
    receipt = std::unexpected(TreeError::NO_MEMORY);
  } catch (...) {
    receipt = std::unexpected(TreeError::CALLBACK_FAILED);
  }
  if (!receipt || !editReceiptMatches(request, *receipt)) {
    std::erase_if(pending, [&](const auto &value) {
      return value.target == request;
    });
    return std::unexpected(receipt ? TreeError::INVALID_ARGUMENT : receipt.error());
  }
  lastReceipt = *receipt;
  if (receipt->status == EditStatus::PENDING) {
    status = EditStatus::PENDING;
    lastReceipt = *receipt;
    return *receipt;
  }
  auto applied = reply(*receipt);
  return applied ? receipt : std::unexpected(applied.error());
}

template <class Row, class Value>
std::expected<bool, TreeError> EditEntry<Row, Value>::reply(const EditReceipt &receipt) {
  const auto found = std::ranges::find_if(pending, [&](const Pending &value) {
    return editReceiptMatches(value.target, receipt);
  });
  if (found == pending.end() || receipt.status == EditStatus::PENDING) {
    return false;
  }
  if (receipt.status != EditStatus::ACCEPTED && receipt.status != EditStatus::INVALID &&
      receipt.status != EditStatus::CONFLICT && receipt.status != EditStatus::TARGET_MISSING &&
      receipt.status != EditStatus::READ_ONLY && receipt.status != EditStatus::CLOSED &&
      receipt.status != EditStatus::REJECTED) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  if (receipt.status == EditStatus::ACCEPTED &&
      (!receipt.acceptedVersion || receipt.acceptedVersion < receipt.target.baseVersion)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }
  const auto operation = *found;
  pending.erase(found);
  if (receipt.target.sequence < acceptedSequence) {
    return false;
  }
  if (receipt.status == EditStatus::ACCEPTED && receipt.acceptedVersion < std::max(observedVersion, witnessedVersion)) {
    acceptedSequence = std::max(acceptedSequence, receipt.target.sequence);
    lastReceipt = receipt;
    status = hasDraft() ? EditStatus::CONFLICT : EditStatus::CLEAN;
    return false;
  }
  const auto currentGeneration = text ? text->generation : generation;
  auto publishNewer = [&] {
    auto state = owner.lock();
    if (!state || !hasDraft() || currentGeneration == operation.generation || !pending.empty() ||
        state->options.mode != EditMode::IMMEDIATE) {
      return;
    }
    try {
      auto next = commit();
      if (!next) {
        status = EditStatus::REJECTED;
        lastReceipt = {target, status, 0, next.error()};
      }
    } catch (const std::bad_alloc &) {
      status = EditStatus::REJECTED;
      lastReceipt = {target, status, 0, TreeError::NO_MEMORY};
    } catch (...) {
      status = EditStatus::REJECTED;
      lastReceipt = {target, status, 0, TreeError::CALLBACK_FAILED};
    }
  };
  status = receipt.status;
  lastReceipt = receipt;
  if (receipt.status != EditStatus::ACCEPTED) {
    if (currentGeneration != operation.generation && hasDraft() &&
        (receipt.status == EditStatus::REJECTED || receipt.status == EditStatus::INVALID)) {
      status = EditStatus::DIRTY;
      publishNewer();
    }
    return true;
  }
  acceptedSequence = receipt.target.sequence;
  observedVersion = receipt.acceptedVersion;
  witnessedVersion = std::max(witnessedVersion, receipt.acceptedVersion);
  const auto previousAccepted = std::exchange(accepted, operation.value);
  target.baseVersion = receipt.acceptedVersion;
  if (text && text->revision == receipt.target.baseVersion &&
      (currentGeneration == operation.generation || hasDraft())) {
    text->revision = receipt.acceptedVersion;
  }
  if (currentGeneration == operation.generation) {
    draft.reset();
    requiresAsyncValidation = false;
    if (text) {
      text->dirty = false;
    }
  } else {
    status = hasDraft() ? EditStatus::DIRTY : EditStatus::ACCEPTED;
  }
  if (auto state = owner.lock()) {
    if (endpoint->capabilities().recordVersion) {
      state->accepted(receipt);
    }
  }
  publishNewer();
  return true;
}

template <class Row, class Value> std::expected<void, TreeError> EditEntry<Row, Value>::cancel(bool discard) {
  const auto state = owner.lock();
  if (!state) {
    return std::unexpected(TreeError::CLOSED);
  }
  if (state->accounting) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }
  if (discard && !pending.empty() && !state->endpointClosed) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }

  // Retire owning values only after the whole target state and its charge agree.
  const auto previousDraft = draft;
  const auto previousAccepted = accepted;
  const auto previousEndpoint = endpoint;
  const auto previousValidation = validation;
  std::vector<Pending> retiredPending(0);
  if (!discard) {
    auto latest = refresh();
    if (!latest) {
      return latest;
    }
  }
  const auto expectedDraft = draft;
  const auto expectedAccepted = accepted;
  const auto beforeGeneration = text ? text->generation : generation;
  std::optional<PreparedTextHistory> prepared;
  std::unique_ptr<GraphemeText> mask;
  std::shared_ptr<const PropertyChange<std::string>> emptySnapshot;
  auto nextTextCharge = textCharge;
  if (text) {
    if (discard) {
      emptySnapshot = std::make_shared<const PropertyChange<std::string>>();
    }
    if (discard || text->dirty || text->history->operationCount() || text->text.bytes() != textSnapshot->value) {
      const auto bytes = discard ? std::string_view{} : std::string_view(textSnapshot->value);
      auto baseline = text->history->prepareResetBaseline({std::string(bytes), bytes.size(), bytes.size()});
      if (!baseline) {
        return std::unexpected(TreeError::NO_MEMORY);
      }
      prepared.emplace(std::move(*baseline));
      if (text->mask && !discard) {
        auto masked =
          GraphemeText::from(std::string(prepared->text()->graphemes().size(), '*'), text->text.policy(), text->limits);
        if (!masked) {
          return std::unexpected(TreeError::INVALID_ARGUMENT);
        }
        mask = std::make_unique<GraphemeText>(std::move(*masked));
      }
      auto measured =
        portals::detail::EditStorageAccess::draftBytes(*prepared->text(), mask.get(), prepared->retainedBytes());
      if (!measured) {
        return std::unexpected(measured.error());
      }
      nextTextCharge = *measured;
    }
  }
  auto finalCharge =
    discard ? std::expected<std::size_t, TreeError>(sizeof(*this)) : retainedCharge(nullptr, nextTextCharge);
  if (!finalCharge) {
    return std::unexpected(finalCharge.error());
  }
  if (draft != expectedDraft || accepted != expectedAccepted || endpoint != previousEndpoint ||
      beforeGeneration != (text ? text->generation : generation)) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }
  if (!discard && state->endpointClosed) {
    return std::unexpected(TreeError::CLOSED);
  }
  if (prepared) {
    auto ready = text->history->checkPrepared(*prepared);
    if (!ready) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
  }
  auto admitted = state->admit(*this, *finalCharge, false);
  if (!admitted) {
    return admitted;
  }
  if (prepared) {
    const auto cursor = prepared->cursor();
    const auto anchor = prepared->anchor();
    prepared->takeText(text->text);
    auto committed = text->history->commit(std::move(*prepared));
    if (!committed) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    text->mask = std::move(mask);
    text->cursor = cursor;
    text->anchor = anchor;
    text->dirty = false;
    ++text->generation;
  }
  textCharge = nextTextCharge;
  draft.reset();
  requiresAsyncValidation = false;
  ++generation;
  if (discard) {
    pending.swap(retiredPending);
    accepted.reset();
    discarded = true;
    endpoint.reset();
    validation.reset();
    validationCharge = 0;
    textSnapshot = std::move(emptySnapshot);
    snapshotCharge = 0;
  } else {
    target.baseVersion = observedVersion;
    if (text) {
      text->revision = observedVersion;
    }
  }
  status = pending.empty() ? EditStatus::CLEAN : EditStatus::PENDING;
  return {};
}

/// @endcond
} // namespace portals::ui::detail
