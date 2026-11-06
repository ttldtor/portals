// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/managed.hpp>

namespace portals {
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

bool NavigationSelection::operator==(const NavigationSelection &other) const noexcept {
  return std::ranges::equal(rows(), other.rows());
}

namespace detail {
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
  closed = true;
  auto oldCapture = std::move(capture);
  auto oldSynchronize = std::move(synchronize);

  cancel();
}

std::expected<void, TreeError> ManagedNavigationRecord::setPosition(std::optional<ModelBookmark> value,
                                                                    PositionResolution resolution, bool readingAnchor) {
  if (value && (value->resource() != resource || value->position().revision == 0 ||
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
  auto value = (*capture)(id);

  if (!value) {
    return std::unexpected(value.error());
  }

  record = lock();

  return record ? (*record)->setPosition(std::move(*value), PositionResolution::EXACT)
                : std::unexpected(record.error());
}

std::expected<void, TreeError> ManagedNavigation::setCurrent(ModelBookmark bookmark) const {
  auto record = lock();

  return record ? (*record)->setPosition(std::move(bookmark), PositionResolution::EXACT)
                : std::unexpected(record.error());
}

std::expected<void, TreeError> ManagedNavigation::clearCurrent() const {
  auto record = lock();

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

  (*record)->changes = std::move(changes);
  (*record)->frameRevision = 0;

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
      !session.epoch || !options.maxSelectedRows || !options.bookmarks.maxBytes) {
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

    auto selection = state(NavigationSelection{});

    if (!selection) {
      return rollback(selection.error());
    }

    auto reading = state(logicalPosition(resource, {}, PositionResolution::EMPTY));

    if (!reading) {
      return rollback(reading.error());
    }

    record->current = *current;
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
}
} // namespace portals
