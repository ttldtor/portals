// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/detail/model_edit.hpp>
#include <portals/managed.hpp>

namespace portals {
namespace detail {
std::expected<ModelResource, TreeError> modelNavigationResource(ManagedModelSource &source) {
  if (source.retired) {
    return std::unexpected(TreeError::CLOSED);
  }
  if (!source.navigationResource.identity.instance) {
    auto resource = ModelResource::create();

    if (!resource) {
      return std::unexpected(navigationTreeError(resource.error()));
    }

    source.navigationResource = *resource;
  }

  return source.navigationResource;
}

std::expected<std::shared_ptr<ModelViewHistory>, TreeError> ModelViewHistory::create(std::weak_ptr<const void> lifetime,
                                                                                     ModelCheckpointOptions options) {
  if (lifetime.expired() || !options.bookmarks.maxBytes) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  auto budget = SourceBudget::create({.maxBytes = options.maxBytes});

  if (!budget) {
    return std::unexpected(navigationTreeError(budget.error()));
  }

  constexpr std::size_t SESSION_ALLOCATION_ALLOWANCE = 256;
  auto credit = (*budget)->reserve({sizeof(ModelViewHistory) + SESSION_ALLOCATION_ALLOWANCE, 0, 0, 0});

  if (!credit) {
    return std::unexpected(navigationTreeError(credit.error()));
  }

  try {
    auto result = std::make_shared<ModelViewHistory>();

    result->credit_ = std::move(*credit);
    result->budget_ = std::move(*budget);
    result->lifetime_ = std::move(lifetime);
    result->options_ = options;

    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

ModelViewHistoryEntry *ModelViewHistory::find(const std::shared_ptr<ManagedModelSource> &model, std::uint64_t role,
                                              bool lineageFallback) const noexcept {
  ModelViewHistoryEntry *fallback{};
  for (auto *entry = entries_.get(); entry; entry = entry->next.get()) {
    if (entry->role != role) {
      continue;
    }
    if (entry->resource == model->navigationResource) {
      return entry;
    }
    if (lineageFallback && entry->resource.identity == model->navigationResource.identity) {
      fallback = entry;
    }
  }

  return fallback;
}

void ModelViewHistory::prune() noexcept {
  auto *entry = &entries_;

  while (*entry) {
    auto model = (*entry)->model.lock();

    if ((!(*entry)->explicitResource && (!model || model->retired)) ||
        ((*entry)->role && (*entry)->participant.expired())) {
      auto removed = std::move(*entry);

      *entry = std::move(removed->next);
    } else {
      entry = &(*entry)->next;
    }
  }
}

std::expected<ModelViewHistoryChange, TreeError> ModelViewHistory::stage(std::shared_ptr<ManagedModelSource> model,
                                                                         std::uint64_t role, ModelViewCheckpoint value,
                                                                         std::weak_ptr<const void> participant) {
  if (lifetime_.expired() || !model || model->retired) {
    return std::unexpected(TreeError::CLOSED);
  }
  auto resource = modelNavigationResource(*model);
  if (!resource) {
    return std::unexpected(resource.error());
  }

  std::size_t metadata{};

  for (const auto *bookmark : {&value.current, &value.reading}) {
    if (*bookmark) {
      if ((*bookmark)->storageBytes() > options_.bookmarks.maxBytes) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }

      const auto bytes = (*bookmark)->storageBytes() - sizeof(ModelBookmark);
      if (bytes > std::numeric_limits<std::size_t>::max() - metadata) {
        return std::unexpected(TreeError::NODE_LIMIT);
      }
      metadata += bytes;
    }
  }

  if (!value.selection.rows().empty()) {
    constexpr std::size_t SELECTION_ALLOCATION_ALLOWANCE = 256;
    const auto rows = value.selection.rows().size();

    if (metadata > std::numeric_limits<std::size_t>::max() - SELECTION_ALLOCATION_ALLOWANCE ||
        rows > (std::numeric_limits<std::size_t>::max() - metadata - SELECTION_ALLOCATION_ALLOWANCE) / sizeof(RowId)) {
      return std::unexpected(TreeError::NODE_LIMIT);
    }

    metadata += rows * sizeof(RowId) + SELECTION_ALLOCATION_ALLOWANCE;
  }

  prune();
  ModelViewHistoryChange change;

  change.existing = find(model, role, false);
  change.model = model;
  change.resource = *resource;
  change.explicitResource = model->navigationExplicit || (change.existing && change.existing->explicitResource);
  change.role = role;
  change.participant = participant;
  change.value = std::move(value);

  if (change.existing) {
    if (change.existing->revision == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
    }

    change.revision = change.existing->revision;

    if (!metadata) {
      change.replaceMetadata = true;
      return change;
    }
    if (metadata <= change.existing->metadataCredit.usage().bytes) {
      return change;
    }
  }

  auto payload = budget_->reserve({metadata, 0, 0, 0});

  if (!payload) {
    return std::unexpected(navigationTreeError(payload.error()));
  }

  change.metadataCredit = std::move(*payload);
  change.replaceMetadata = true;

  if (change.existing) {
    return change;
  }

  constexpr std::size_t ENTRY_ALLOCATION_ALLOWANCE = 128;
  auto credit = budget_->reserve({sizeof(ModelViewHistoryEntry) + ENTRY_ALLOCATION_ALLOWANCE, 0, 0, 0});

  if (!credit) {
    return std::unexpected(navigationTreeError(credit.error()));
  }

  try {
    change.added = std::make_unique<ModelViewHistoryEntry>();
    change.added->credit = std::move(*credit);
    change.added->metadataCredit = std::move(change.metadataCredit);
    change.added->model = std::move(model);
    change.added->resource = change.resource;
    change.added->explicitResource = change.explicitResource;
    change.added->role = role;
    change.added->participant = std::move(participant);
    change.added->value = std::move(change.value);

    return change;
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

bool ModelViewHistory::valid(const ModelViewHistoryChange &change) const noexcept {
  auto model = change.model.lock();

  if (lifetime_.expired() || !model || model->retired) {
    return false;
  }

  if (model->navigationResource != change.resource) {
    return false;
  }
  const auto *entry = find(model, change.role, false);

  return change.existing ? entry == change.existing && entry->revision == change.revision : entry == nullptr;
}

void ModelViewHistory::commit(ModelViewHistoryChange change) noexcept {
  if (change.existing) {
    change.existing->value = std::move(change.value);
    change.existing->model = std::move(change.model);
    change.existing->explicitResource = change.explicitResource;
    change.existing->participant = std::move(change.participant);
    ++change.existing->revision;

    if (change.replaceMetadata) {
      change.existing->metadataCredit = std::move(change.metadataCredit);
    }
  } else {
    change.added->next = std::move(entries_);
    entries_ = std::move(change.added);
  }
}

std::expected<std::uint64_t, TreeError> ModelViewHistory::role() {
  if (nextRole_ == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
  }

  return nextRole_++;
}

std::expected<SourceCredit, TreeError> ModelViewHistory::reserveState() {
  constexpr std::size_t STATE_ALLOCATION_ALLOWANCE = 256;
  auto reserved = budget_->reserve({sizeof(ModelViewRestoreState) + STATE_ALLOCATION_ALLOWANCE, 0, 0, 0});
  return reserved ? std::expected<SourceCredit, TreeError>(std::move(*reserved))
                  : std::unexpected(navigationTreeError(reserved.error()));
}

std::expected<SourceCredit, TreeError> ModelViewHistory::reserveRole() {
  constexpr std::size_t ROLE_ALLOCATION_ALLOWANCE = 256;
  auto reserved = budget_->reserve({sizeof(SourceCredit) + ROLE_ALLOCATION_ALLOWANCE, 0, 0, 0});
  return reserved ? std::expected<SourceCredit, TreeError>(std::move(*reserved))
                  : std::unexpected(navigationTreeError(reserved.error()));
}

std::expected<std::shared_ptr<ModelSelectionPositions>, TreeError>
ModelViewHistory::selectedPositions(std::size_t count) {
  constexpr std::size_t POSITIONS_ALLOCATION_ALLOWANCE = 256;
  constexpr std::size_t BASE_BYTES = sizeof(ModelSelectionPositions) + POSITIONS_ALLOCATION_ALLOWANCE;
  if (count > (std::numeric_limits<std::size_t>::max() - BASE_BYTES) / sizeof(ModelBookmark)) {
    return std::unexpected(TreeError::NODE_LIMIT);
  }
  auto credit = budget_->reserve({BASE_BYTES + count * sizeof(ModelBookmark), 0, 0, 0});
  if (!credit) {
    return std::unexpected(navigationTreeError(credit.error()));
  }
  try {
    auto result = std::make_shared<ModelSelectionPositions>();
    result->credit = std::move(*credit);
    result->positions.reserve(count);
    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  } catch (const std::length_error &) {
    return std::unexpected(TreeError::NODE_LIMIT);
  }
}

std::expected<std::shared_ptr<ModelCanonicalRestore>, TreeError> ModelViewHistory::canonicalRestore() {
  constexpr std::size_t RESTORE_ALLOCATION_ALLOWANCE = 256;
  auto credit = budget_->reserve({sizeof(ModelCanonicalRestore) + RESTORE_ALLOCATION_ALLOWANCE, 0, 0, 0});
  if (!credit) {
    return std::unexpected(navigationTreeError(credit.error()));
  }
  try {
    auto result = std::make_shared<ModelCanonicalRestore>();
    result->credit = std::move(*credit);
    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

const ModelCheckpointOptions &ModelViewHistory::options() const noexcept {
  return options_;
}

LogCheckpointEntry *LogNavigationSession::find(ModelResource resource) const noexcept {
  for (auto *entry = entries.get(); entry; entry = entry->next.get()) {
    if (entry->resource.identity == resource.identity && !entry->lifetime.expired()) {
      return entry;
    }
  }

  return nullptr;
}

void LogNavigationSession::prune() noexcept {
  auto *entry = &entries;

  while (*entry) {
    if ((*entry)->lifetime.expired()) {
      auto removed = std::move(*entry);

      *entry = std::move(removed->next);
    } else {
      entry = &(*entry)->next;
    }
  }
}

void LogNavigationSession::reset(ModelResource resource) noexcept {
  auto *entry = &entries;

  while (*entry) {
    if ((*entry)->resource.identity == resource.identity) {
      auto removed = std::move(*entry);

      *entry = std::move(removed->next);

      return;
    }

    entry = &(*entry)->next;
  }
}

void LogNavigationSession::close() noexcept {
  closed = true;
  binding.reset();

  while (entries) {
    auto removed = std::move(entries);

    entries = std::move(removed->next);
  }
}

std::expected<LogCheckpointChange, SourceError> LogNavigationSession::stage(ModelResource resource,
                                                                            std::weak_ptr<const void> resourceLifetime,
                                                                            LogReadingCheckpoint value) {
  if (closed || lifetime.expired()) {
    return std::unexpected(SourceError::CLOSED);
  }
  if (!resource.identity.instance || !resource.identity.epoch || !resource.lineage || resourceLifetime.expired()) {
    return std::unexpected(SourceError::INVALID_ARGUMENT);
  }

  prune();
  const auto bookmarkBytes = value.bookmark ? value.bookmark->storageBytes() : 0;

  if (bookmarkBytes > options.bookmarks.maxBytes) {
    return std::unexpected(SourceError::WOULD_BLOCK);
  }

  // The entry already contains the optional bookmark's scalar storage. Only an
  // opaque allocation needs separate credit. Updating inline values cannot need
  // another complete entry reservation when the history ledger is full.
  const auto metadata = value.bookmark ? bookmarkBytes - sizeof(ModelBookmark) : 0;
  LogCheckpointChange change;

  change.existing = find(resource);
  change.resource = resource;
  change.lifetime = resourceLifetime;
  change.value = std::move(value);

  if (change.existing && metadata <= change.existing->metadataCredit.usage().bytes) {
    return change;
  }

  auto payload = budget->reserve({metadata, 0, 0, 0});

  if (!payload) {
    return std::unexpected(payload.error());
  }

  change.metadataCredit = std::move(*payload);
  change.replaceMetadata = true;

  if (change.existing) {
    return change;
  }

  constexpr std::size_t ENTRY_ALLOCATION_ALLOWANCE = 128;
  auto reserved = budget->reserve({sizeof(LogCheckpointEntry) + ENTRY_ALLOCATION_ALLOWANCE, 0, 0, 0});

  if (!reserved) {
    return std::unexpected(reserved.error());
  }

  try {
    auto entry = std::make_unique<LogCheckpointEntry>();

    entry->credit = std::move(*reserved);
    entry->metadataCredit = std::move(change.metadataCredit);
    entry->resource = resource;
    entry->lifetime = std::move(resourceLifetime);
    entry->value = std::move(change.value);
    change.added = std::move(entry);

    return change;
  } catch (const std::bad_alloc &) {
    return std::unexpected(SourceError::NO_MEMORY);
  }
}

void LogNavigationSession::commit(LogCheckpointChange change) noexcept {
  if (change.existing) {
    change.existing->resource = change.resource;
    change.existing->lifetime = std::move(change.lifetime);
    change.existing->value = std::move(change.value);

    if (change.replaceMetadata) {
      change.existing->metadataCredit = std::move(change.metadataCredit);
    }

    return;
  }

  change.added->next = std::move(entries);
  entries = std::move(change.added);
}
} // namespace detail

std::expected<std::shared_ptr<detail::LogNavigationSession>, SourceError> LogNavigation::lock() const {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(owner_ == std::thread::id{} ? SourceError::CLOSED : SourceError::WRONG_THREAD);
  }

  auto session = session_.lock();

  if (!session || session->closed || session->lifetime.expired()) {
    return std::unexpected(SourceError::CLOSED);
  }

  return session;
}

std::expected<LogRestoreStatus, SourceError> LogNavigation::status() const {
  auto session = lock();

  return session ? std::expected<LogRestoreStatus, SourceError>((*session)->status) : std::unexpected(session.error());
}

std::expected<std::optional<LogReadingCheckpoint>, SourceError>
LogNavigation::checkpoint(ModelResource resource) const {
  auto session = lock();

  if (!session) {
    return std::unexpected(session.error());
  }

  const auto *entry = (*session)->find(resource);

  return entry ? std::optional(entry->value) : std::optional<LogReadingCheckpoint>{};
}

std::expected<void, SourceError> LogNavigation::reset(ModelResource resource) const {
  auto session = lock();

  if (!session) {
    return std::unexpected(session.error());
  }

  (*session)->reset(resource);

  return {};
}

std::expected<LogNavigation, ManagedError> ManagedScene::logNavigation(LogCheckpointOptions options) {
  auto allowed = boundary(ManagedPhase::STATE, root_);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }
  if (!options.bookmarks.maxBytes) {
    return std::unexpected(error(ManagedPhase::STATE, root_, TreeError::INVALID_ARGUMENT));
  }

  auto budget = SourceBudget::create({.maxBytes = options.maxBytes});

  if (!budget) {
    return std::unexpected(error(ManagedPhase::STATE, root_, detail::navigationTreeError(budget.error())));
  }

  constexpr std::size_t SESSION_ALLOCATION_ALLOWANCE = 256;
  auto credit = (*budget)->reserve({sizeof(detail::LogNavigationSession) + SESSION_ALLOCATION_ALLOWANCE, 0, 0, 0});

  if (!credit) {
    return std::unexpected(error(ManagedPhase::STATE, root_, detail::navigationTreeError(credit.error())));
  }

  try {
    if (!navigation_) {
      navigation_ = std::make_unique<detail::ManagedNavigationRegistry>();
    }

    navigation_->logSessions.reserve(navigation_->logSessions.size() + 1);
    auto session = std::make_shared<detail::LogNavigationSession>();

    session->credit = std::move(*credit);
    session->budget = std::move(*budget);
    session->lifetime = lifetime_;
    session->scene = root_.id();
    session->options = options;
    navigation_->logSessions.push_back(session);
    LogNavigation handle;

    handle.session_ = session;
    handle.owner_ = owner_;

    return handle;
  } catch (const std::bad_alloc &) {
    return std::unexpected(error(ManagedPhase::STATE, root_, TreeError::NO_MEMORY));
  } catch (const std::length_error &) {
    return std::unexpected(error(ManagedPhase::STATE, root_, TreeError::NODE_LIMIT));
  }
}

namespace {
NavigationPosition logicalPosition(ModelResource resource, const std::optional<ModelBookmark> &bookmark,
                                   PositionResolution resolution) {
  NavigationPosition value;

  value.resource = resource;
  value.resolution = resolution;
  if (bookmark) {
    const auto position = bookmark->position();

    value.kind = position.kind;
    value.affinity = position.affinity;
    if (position.kind == PositionKind::ITEM) {
      value.row = position.row;
    } else {
      value.offset = position.offset;
    }
  }

  return value;
}
} // namespace

std::span<const RowId> NavigationSelection::rows() const noexcept {
  return rows_ ? std::span<const RowId>(*rows_) : std::span<const RowId>{};
}

ModelResource NavigationSelection::resource() const noexcept {
  return resource_;
}

PositionResolution NavigationSelection::resolution() const noexcept {
  return resolution_;
}

bool NavigationSelection::operator==(const NavigationSelection &other) const noexcept {
  return resource_ == other.resource_ && resolution_ == other.resolution_ && std::ranges::equal(rows(), other.rows());
}

namespace detail {
std::expected<ManagedNavigationRecord::Publication, TreeError>
ManagedNavigationRecord::preparePublication(NavigationPosition nextCurrent, NavigationSelection nextSelection,
                                            NavigationPosition nextReading) {
  auto currentOwner = current.lock();
  auto selectionOwner = selection.lock();
  auto readingOwner = reading.lock();

  if (!currentOwner || !selectionOwner || !readingOwner) {
    return std::unexpected(!currentOwner     ? currentOwner.error()
                           : !selectionOwner ? selectionOwner.error()
                                             : readingOwner.error());
  }

  auto preparedCurrent = (*currentOwner)->prepareChange(std::move(nextCurrent));

  if (!preparedCurrent) {
    return std::unexpected(preparedCurrent.error());
  }

  auto preparedSelection = (*selectionOwner)->prepareChange(std::move(nextSelection));

  if (!preparedSelection) {
    return std::unexpected(preparedSelection.error());
  }

  auto preparedReading = (*readingOwner)->prepareChange(std::move(nextReading));

  if (!preparedReading) {
    return std::unexpected(preparedReading.error());
  }

  return Publication{*currentOwner,
                     *selectionOwner,
                     *readingOwner,
                     std::move(*preparedCurrent),
                     std::move(*preparedSelection),
                     std::move(*preparedReading)};
}

std::expected<void, TreeError> ManagedNavigationRecord::validatePublication(const Publication &publication) const {
  if (closed || lifetime.expired()) {
    return std::unexpected(TreeError::CLOSED);
  }

  auto currentValidation = Property<NavigationPosition>::validateChange(publication.current);
  auto selectionValidation = Property<NavigationSelection>::validateChange(publication.selection);
  auto readingValidation = Property<NavigationPosition>::validateChange(publication.reading);

  return !currentValidation ? currentValidation : !selectionValidation ? selectionValidation : readingValidation;
}

std::expected<void, TreeError> ManagedNavigationRecord::invalidatePublication(const Publication &publication) const {
  auto currentInvalidation = Property<NavigationPosition>::invalidateChange(publication.current);

  if (!currentInvalidation) {
    return currentInvalidation;
  }

  auto selectionInvalidation = Property<NavigationSelection>::invalidateChange(publication.selection);

  return selectionInvalidation ? Property<NavigationPosition>::invalidateChange(publication.reading)
                               : selectionInvalidation;
}

void ManagedNavigationRecord::installPublication(const Publication &publication) noexcept {
  Property<NavigationPosition>::installChange(publication.current);
  Property<NavigationSelection>::installChange(publication.selection);
  Property<NavigationPosition>::installChange(publication.reading);
}

std::optional<SchedulerError> ManagedNavigationRecord::publishPublication(const Publication &publication) {
  const auto current = Property<NavigationPosition>::publishChange(publication.current);
  const auto selection = Property<NavigationSelection>::publishChange(publication.selection);
  const auto reading = Property<NavigationPosition>::publishChange(publication.reading);
  return current.notification_error     ? current.notification_error
         : selection.notification_error ? selection.notification_error
                                        : reading.notification_error;
}

TreeError navigationTreeError(SourceError error) noexcept {
  switch (error) {
  case SourceError::WRONG_THREAD:
    return TreeError::WRONG_THREAD;
  case SourceError::CLOSED:
  case SourceError::CANCELLED:
    return TreeError::CLOSED;
  case SourceError::NO_MEMORY:
    return TreeError::NO_MEMORY;
  case SourceError::IDENTITY_EXHAUSTED:
    return TreeError::IDENTITY_EXHAUSTED;
  case SourceError::CALLBACK_FAILED:
    return TreeError::CALLBACK_FAILED;
  case SourceError::WOULD_BLOCK:
  case SourceError::UNSUPPORTED:
  case SourceError::ROW_NOT_FOUND:
    return TreeError::WOULD_BLOCK;
  default:
    return TreeError::INVALID_ARGUMENT;
  }
}

void ManagedNavigationRecord::cancel() noexcept {
  auto previous = std::move(cancellation);

  cancellation = std::stop_source(std::nostopstate);
  request.reset();
  // Stop callbacks may reenter navigation. All visible fields already agree.
  previous.request_stop();
}

void ManagedNavigationRecord::close() noexcept {
  if (editors) {
    for (const auto &weak : *editors) {
      if (auto participant = weak.lock()) {
        participant->closed();
      }
    }
  }

  closed = true;
  auto oldCapture = std::move(capture);
  auto oldSynchronize = std::move(synchronize);
  auto oldHistory = std::move(history);

  cancel();
}

std::expected<void, TreeError> ManagedNavigationRecord::setPosition(std::optional<ModelBookmark> value,
                                                                    PositionResolution resolution, bool readingAnchor) {
  // An incompatible lineage is retained as owning metadata for explicit reset or
  // a later attested reopen. It is never admitted as a resolved current item.
  const bool incompatibleLineage =
    value && resolution == PositionResolution::INCOMPATIBLE && value->resource().identity == resource.identity;
  if (value && ((value->resource() != resource && !incompatibleLineage) || value->position().revision == 0 ||
                value->storageBytes() > options.bookmarks.maxBytes)) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  if (operation == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
  }

  auto logical = logicalPosition(resource, value, resolution);
  auto updated = (readingAnchor ? reading : current).set(logical);

  if (!updated) {
    return std::unexpected(updated.error());
  }

  ++operation;
  // Metadata advances even when logical equality suppresses Property publication.
  if (readingAnchor) {
    readingBookmark = std::move(value);
  } else {
    bookmark = std::move(value);
    cancel();
  }

  return {};
}
} // namespace detail

std::expected<std::shared_ptr<detail::ManagedNavigationRecord>, TreeError> ManagedNavigation::lock() const {
  if (owner_ != std::thread::id{} && owner_ != std::this_thread::get_id()) {
    return std::unexpected(TreeError::WRONG_THREAD);
  }

  auto record = record_.lock();

  if (!record || record->closed || record->lifetime.expired()) {
    return std::unexpected(TreeError::CLOSED);
  }
  if (record->finite) {
    auto source = record->model.lock();

    if (!source || source->retired) {
      return std::unexpected(TreeError::CLOSED);
    }
    if (source->origin) {
      auto live = source->origin->check();

      if (!live) {
        return std::unexpected(source->origin->retired || source->origin->closed ? TreeError::CLOSED : live.error());
      }
    }
  }

  return record;
}

std::expected<ManagedSource<NavigationPosition>, TreeError> ManagedNavigation::current() const {
  auto record = lock();

  return record ? std::expected<ManagedSource<NavigationPosition>, TreeError>(ManagedSource((*record)->current))
                : std::unexpected(record.error());
}

std::expected<ManagedSource<NavigationSelection>, TreeError> ManagedNavigation::selection() const {
  auto record = lock();

  return record ? std::expected<ManagedSource<NavigationSelection>, TreeError>(ManagedSource((*record)->selection))
                : std::unexpected(record.error());
}

std::expected<ManagedSource<NavigationPosition>, TreeError> ManagedNavigation::readingAnchor() const {
  auto record = lock();

  return record ? std::expected<ManagedSource<NavigationPosition>, TreeError>(ManagedSource((*record)->reading))
                : std::unexpected(record.error());
}

std::expected<std::optional<ModelBookmark>, TreeError> ManagedNavigation::bookmark() const {
  auto record = lock();

  return record ? std::expected<std::optional<ModelBookmark>, TreeError>((*record)->bookmark)
                : std::unexpected(record.error());
}

std::expected<std::optional<ModelBookmark>, TreeError> ManagedNavigation::readingBookmark() const {
  auto record = lock();

  return record ? std::expected<std::optional<ModelBookmark>, TreeError>((*record)->readingBookmark)
                : std::unexpected(record.error());
}

std::expected<void, TreeError> ManagedNavigation::navigate(RowId id) const {
  auto record = lock();
  if (!record) {
    return std::unexpected(record.error());
  }
  if (!(*record)->capture) {
    return std::unexpected(TreeError::WOULD_BLOCK);
  }
  auto capture = (*record)->capture;
  auto value = (*capture)(*record, id);
  if (!value) {
    return std::unexpected(value.error());
  }
  return setCurrent(std::move(*value));
}

std::expected<void, TreeError> ManagedNavigation::setCurrent(ModelBookmark bookmark) const {
  auto record = lock();
  if (!record) {
    return std::unexpected(record.error());
  }
  const auto position = bookmark.position();
  NavigationPosition next{bookmark.resource(),
                          position.kind,
                          position.kind == PositionKind::ITEM ? std::optional(position.row) : std::nullopt,
                          position.kind == PositionKind::ITEM ? 0 : position.offset,
                          position.affinity,
                          PositionResolution::EXACT};
  auto admitted = (*record)->beforeEditTransition(&next);
  if (!admitted) {
    return std::unexpected(admitted.error());
  }
  record = lock();
  return record ? (*record)->setPosition(std::move(bookmark), PositionResolution::EXACT)
                : std::unexpected(record.error());
}

std::expected<void, TreeError> ManagedNavigation::clearCurrent() const {
  auto record = lock();
  if (!record) {
    return std::unexpected(record.error());
  }
  NavigationPosition next;
  next.resource = (*record)->resource;
  auto admitted = (*record)->beforeEditTransition(&next);
  if (!admitted) {
    return std::unexpected(admitted.error());
  }
  record = lock();
  return record ? (*record)->setPosition({}, PositionResolution::EMPTY) : std::unexpected(record.error());
}

std::expected<void, TreeError> ManagedNavigation::setSelection(std::span<const RowId> rows) const {
  auto record = lock();

  if (!record) {
    return std::unexpected(record.error());
  }
  if (rows.size() > (*record)->options.maxSelectedRows) {
    return std::unexpected(TreeError::NODE_LIMIT);
  }
  if (std::ranges::any_of(rows, [](RowId id) {
        return !id.value || !id.generation;
      })) {
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  try {
    NavigationSelection value;

    value.resource_ = (*record)->resource;
    value.resolution_ = rows.empty() ? PositionResolution::EMPTY : PositionResolution::EXACT;

    if (!rows.empty()) {
      auto payload = std::make_shared<std::vector<RowId>>(rows.begin(), rows.end());

      std::ranges::sort(*payload, [](RowId left, RowId right) {
        return std::tie(left.value, left.generation) < std::tie(right.value, right.generation);
      });
      auto duplicates = std::ranges::unique(*payload);

      payload->erase(duplicates.begin(), duplicates.end());
      value.rows_ = std::move(payload);
    }

    auto updated = (*record)->selection.set(value);

    return updated ? std::expected<void, TreeError>{} : std::unexpected(updated.error());
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  } catch (const std::length_error &) {
    return std::unexpected(TreeError::NODE_LIMIT);
  }
}

std::expected<void, TreeError> ManagedNavigation::setReadingAnchor(ModelBookmark bookmark) const {
  auto record = lock();

  return record ? (*record)->setPosition(std::move(bookmark), PositionResolution::EXACT, true)
                : std::unexpected(record.error());
}

std::expected<void, TreeError> ManagedNavigation::clearReadingAnchor() const {
  auto record = lock();

  return record ? (*record)->setPosition({}, PositionResolution::EMPTY, true) : std::unexpected(record.error());
}

std::expected<void, TreeError> ManagedNavigation::setChangeMap(PositionChangeMap changes) const {
  auto record = lock();

  if (!record) {
    return std::unexpected(record.error());
  }

  if ((*record)->operation == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
  }
  if (auto model = (*record)->model.lock()) {
    if (model->navigationRevision == std::numeric_limits<std::uint64_t>::max()) {
      return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
    }
    model->navigationChanges = changes;
    ++model->navigationRevision;
  }
  (*record)->changes = std::move(changes);
  (*record)->frameRevision = 0;
  ++(*record)->operation;

  return {};
}

std::expected<PositionRequest, TreeError> ManagedNavigation::beginResolve(std::uint64_t revision) const {
  auto record = lock();

  if (!record) {
    return std::unexpected(record.error());
  }
  if (!revision || !(*record)->bookmark) {
    return std::unexpected(revision ? TreeError::WOULD_BLOCK : TreeError::INVALID_ARGUMENT);
  }
  if ((*record)->sequence == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(TreeError::IDENTITY_EXHAUSTED);
  }

  try {
    std::stop_source next;
    PositionRequest request{(*record)->session, revision, (*record)->sequence + 1, *(*record)->bookmark};
    auto previous = std::move((*record)->cancellation);

    (*record)->sequence = request.sequence;
    (*record)->request = request;
    (*record)->cancellation = std::move(next);
    previous.request_stop();

    return request;
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

std::expected<bool, TreeError> ManagedNavigation::applyReply(const PositionReply &reply) const {
  auto record = lock();

  if (!record) {
    return std::unexpected(record.error());
  }
  if (!(*record)->request || !positionReplyMatches(*(*record)->request, reply)) {
    return false;
  }

  auto bookmark = (*record)->bookmark;

  switch (reply.state) {
  case PositionResolution::EXACT:
  case PositionResolution::NEAREST: {
    if (reply.position.revision != reply.revision) {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    }

    auto value = ModelBookmark::create(reply.resource, reply.position, (*record)->request->bookmark.metadata(),
                                       (*record)->options.bookmarks);

    if (!value) {
      return std::unexpected(detail::navigationTreeError(value.error()));
    }

    bookmark = std::move(*value);

    break;
  }
  case PositionResolution::EMPTY:
    bookmark.reset();

    break;
  case PositionResolution::PENDING:
  case PositionResolution::UNSUPPORTED:
  case PositionResolution::INCOMPATIBLE:
  case PositionResolution::UNAVAILABLE:
    break;
  default:
    return std::unexpected(TreeError::INVALID_ARGUMENT);
  }

  auto updated = (*record)->setPosition(std::move(bookmark), reply.state);

  return updated ? std::expected<bool, TreeError>(true) : std::unexpected(updated.error());
}

std::expected<std::stop_token, TreeError> ManagedNavigation::stopToken() const {
  auto record = lock();

  return record ? std::expected<std::stop_token, TreeError>((*record)->cancellation.get_token())
                : std::unexpected(record.error());
}

std::expected<ManagedNavigation, ManagedError> ManagedScene::navigation(ModelResource resource, SourceStamp session,
                                                                        NavigationOptions options) {
  auto allowed = boundary(ManagedPhase::STATE, root_);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }
  if (!resource.identity.instance || !resource.identity.epoch || !resource.lineage || !session.instance ||
      !session.epoch || !options.maxSelectedRows || !options.bookmarks.maxBytes || !options.maxCheckpointBytes) {
    return std::unexpected(error(ManagedPhase::STATE, root_, TreeError::INVALID_ARGUMENT));
  }
  if (options_.maxStates - std::min(options_.maxStates, properties_.size()) < 3) {
    return std::unexpected(error(ManagedPhase::STATE, root_, TreeError::NODE_LIMIT));
  }

  const auto checkpoint = properties_.size();
  const auto rollback = [&](ManagedError failure) -> std::expected<ManagedNavigation, ManagedError> {
    while (properties_.size() > checkpoint) {
      properties_.pop_back();
    }

    return std::unexpected(std::move(failure));
  };

  try {
    if (!navigation_) {
      navigation_ = std::make_unique<detail::ManagedNavigationRegistry>();
    }
    navigation_->contexts.reserve(navigation_->contexts.size() + 1);
    auto record = std::make_shared<detail::ManagedNavigationRecord>();
    auto current = state(logicalPosition(resource, {}, PositionResolution::EMPTY));

    if (!current) {
      return rollback(current.error());
    }

    NavigationSelection selected;

    selected.resource_ = resource;
    auto selection = state(std::move(selected));

    if (!selection) {
      return rollback(selection.error());
    }

    auto reading = state(logicalPosition(resource, {}, PositionResolution::EMPTY));

    if (!reading) {
      return rollback(reading.error());
    }

    record->current = *current;
    record->owner = this;
    record->selection = *selection;
    record->reading = *reading;
    record->lifetime = lifetime_;
    record->scene = root_.id();
    record->resource = resource;
    record->session = session;
    record->options = options;
    navigation_->contexts.push_back(record);
    ManagedNavigation handle;

    handle.record_ = record;
    handle.owner_ = owner_;

    return handle;
  } catch (const std::bad_alloc &) {
    return rollback(error(ManagedPhase::STATE, root_, TreeError::NO_MEMORY));
  } catch (const std::length_error &) {
    return rollback(error(ManagedPhase::STATE, root_, TreeError::NODE_LIMIT));
  }
}

std::expected<void, TreeError> ManagedScene::synchronizeNavigation() {
  if (!navigation_) {
    return {};
  }

  pruneNavigation();
  const auto initialCount = navigation_->contexts.size();

  for (std::size_t index = 0; index < initialCount; ++index) {
    if (closed_ || index >= navigation_->contexts.size()) {
      return std::unexpected(TreeError::CLOSED);
    }

    auto record = navigation_->contexts[index];

    if (record->closed || !record->synchronize) {
      continue;
    }

    auto action = record->synchronize;
    auto result = (*action)(record);

    if (!result) {
      return result;
    }
    if (closed_ || index >= navigation_->contexts.size()) {
      return std::unexpected(TreeError::CLOSED);
    }
  }

  return {};
}

std::expected<void, TreeError> ManagedScene::navigationBefore(Node<Widget> node) {
  if (!navigation_) {
    return {};
  }

  for (const auto &binding : navigation_->bindings) {
    if (binding->node == node) {
      auto owner = binding;

      return owner->before();
    }
  }

  return {};
}

std::expected<void, TreeError> ManagedScene::navigationAfter(Node<Widget> node) {
  if (!navigation_) {
    return {};
  }

  for (const auto &binding : navigation_->bindings) {
    if (binding->node == node) {
      auto owner = binding;

      return owner->after();
    }
  }

  return {};
}

void ManagedScene::pruneNavigation() {
  if (!navigation_) {
    return;
  }

  std::erase_if(navigation_->logSessions, [](const auto &session) {
    session->prune();

    if (session->implicit && session->binding.expired()) {
      session->close();

      return true;
    }

    return false;
  });

  std::erase_if(navigation_->bindings, [&](const auto &binding) {
    auto live = app_.tree().withScope(binding->node, [](const UiScope &) {
    });

    return !live && (live.error() == TreeError::STALE_NODE || live.error() == TreeError::CLOSED);
  });
  const auto initialCount = navigation_->contexts.size();

  for (std::size_t index = 0; index < initialCount; ++index) {
    if (closed_ || index >= navigation_->contexts.size()) {
      return;
    }

    auto record = navigation_->contexts[index];

    if (record->history) {
      record->history->prune();
    }
    if (!record->finite || record->closed) {
      continue;
    }

    auto source = record->model.lock();
    bool closed = !source || source->retired;

    if (!closed && source->origin) {
      auto live = source->origin->check();

      closed = !live && (source->origin->retired || source->origin->closed);
    }
    if (closed) {
      record->close();
      if (closed_ || index >= navigation_->contexts.size()) {
        return;
      }
    }
  }
}

void ManagedScene::retireNavigation() noexcept {
  if (!navigation_) {
    return;
  }

  while (!navigation_->contexts.empty()) {
    auto record = navigation_->contexts.back();

    navigation_->contexts.pop_back();
    record->close();
  }
  navigation_->bindings.clear();

  for (const auto &session : navigation_->logSessions) {
    session->close();
  }

  navigation_->logSessions.clear();
}
} // namespace portals

namespace portals::detail {
std::expected<void, TreeError>
ManagedNavigationRecord::beforeEditTransition(const NavigationPosition *next,
                                              const std::shared_ptr<ManagedModelSource> &nextSource) {
  try {
    editVetoed = false;
    if (!editors) {
      return {};
    }
    if (admittingEdit) {
      return std::unexpected(TreeError::WOULD_BLOCK);
    }
    const auto previousOperation = operation;
    const auto previousSession = session;

    struct Admission {
      bool &active;

      ~Admission() {
        active = false;
      }
    } guard{admittingEdit};

    admittingEdit = true;
    for (std::size_t index = 0; index < editors->size(); ++index) {
      auto participant = (*editors)[index].lock();
      if (!participant) {
        continue;
      }
      auto accepted = participant->beforeTransition(next, nextSource ? nextSource : model.lock());
      if (!accepted) {
        editVetoed = accepted.error() == TreeError::WOULD_BLOCK || accepted.error() == TreeError::NODE_LIMIT;
        return accepted;
      }
      if (closed || lifetime.expired()) {
        return std::unexpected(TreeError::CLOSED);
      }
      if (operation != previousOperation || session != previousSession) {
        return std::unexpected(TreeError::WOULD_BLOCK);
      }
    }
    return {};
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  } catch (...) {
    return std::unexpected(TreeError::CALLBACK_FAILED);
  }
}
} // namespace portals::detail
