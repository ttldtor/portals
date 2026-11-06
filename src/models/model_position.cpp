// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/model_position.hpp>

#include <algorithm>
#include <limits>
#include <new>

namespace portals {
namespace {
// Covers common shared-array control-block/alignment overhead; a charge is not allocator RSS.
constexpr std::size_t BOOKMARK_ALLOCATION_ALLOWANCE = 128;

bool validStamp(SourceStamp stamp) noexcept {
  return stamp.instance != 0 && stamp.epoch != 0;
}

bool validPosition(ModelPosition position) noexcept {
  if (position.revision == 0 ||
      (position.affinity != PositionAffinity::FORWARD && position.affinity != PositionAffinity::BACKWARD)) {
    return false;
  }

  switch (position.kind) {
  case PositionKind::ITEM:
    return position.row.value != 0 && position.row.generation != 0;
  case PositionKind::GAP:
  case PositionKind::TEXT:
    return position.row == RowId{};
  }

  return false;
}

std::uint64_t mapGapSplice(std::uint64_t offset, PositionChange change, PositionAffinity affinity) noexcept {
  const auto end = change.first + change.removed;

  if (offset > end) {
    offset -= change.removed;
  } else if (offset >= change.first) {
    offset = change.first;
  }

  if (offset > change.first || (offset == change.first && affinity == PositionAffinity::FORWARD)) {
    offset += change.inserted;
  }

  return offset;
}

std::uint64_t mapMove(std::uint64_t offset, PositionChange change, bool gap, PositionAffinity affinity) noexcept {
  const auto end = change.first + change.removed;
  const bool inside = gap ? (offset > change.first && offset < end) ||
                              (offset == change.first && affinity == PositionAffinity::FORWARD) ||
                              (offset == end && affinity == PositionAffinity::BACKWARD)
                          : offset >= change.first && offset < end;

  if (inside) {
    return change.destination + (offset - change.first);
  }

  if (gap) {
    offset = mapGapSplice(offset, PositionChange::splice(change.first, change.removed, 0), affinity);

    return mapGapSplice(offset, PositionChange::splice(change.destination, 0, change.removed), affinity);
  }

  if (offset >= end) {
    offset -= change.removed;
  }

  if (offset >= change.destination) {
    offset += change.removed;
  }

  return offset;
}
} // namespace

std::expected<ModelResource, SourceError> ModelResource::create() {
  auto identity = detail::windowSourceStamp();

  if (!identity) {
    return std::unexpected(identity.error());
  }

  return ModelResource{*identity, 1};
}

std::expected<ModelBookmark, SourceError> ModelBookmark::create(ModelResource resource, ModelPosition position,
                                                                std::span<const std::byte> metadata,
                                                                BookmarkOptions options) {
  if (!validStamp(resource.identity) || resource.lineage == 0 || !validPosition(position) || options.maxBytes == 0) {
    return std::unexpected(SourceError::INVALID_ARGUMENT);
  }

  const auto header = sizeof(ModelBookmark) + (metadata.empty() ? 0 : BOOKMARK_ALLOCATION_ALLOWANCE);

  if (header > options.maxBytes || metadata.size() > options.maxBytes - header) {
    return std::unexpected(SourceError::WOULD_BLOCK);
  }

  try {
    ModelBookmark result;

    result.resource_ = resource;
    result.position_ = position;
    result.metadataSize_ = metadata.size();

    if (!metadata.empty()) {
      auto owned = std::make_shared<std::byte[]>(metadata.size());

      std::copy(metadata.begin(), metadata.end(), owned.get());
      result.metadata_ = std::move(owned);
    }

    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(SourceError::NO_MEMORY);
  }
}

ModelResource ModelBookmark::resource() const noexcept {
  return resource_;
}

ModelPosition ModelBookmark::position() const noexcept {
  return position_;
}

std::span<const std::byte> ModelBookmark::metadata() const noexcept {
  return {metadata_.get(), metadataSize_};
}

std::size_t ModelBookmark::storageBytes() const noexcept {
  return sizeof(ModelBookmark) + metadataSize_ + (metadataSize_ == 0 ? 0 : BOOKMARK_ALLOCATION_ALLOWANCE);
}

bool positionReplyMatches(const PositionRequest &request, const PositionReply &reply) noexcept {
  return request.sequence != 0 && request.revision != 0 && validStamp(request.session) &&
         validStamp(request.bookmark.resource().identity) && request.bookmark.resource().lineage != 0 &&
         request.bookmark.position().revision != 0 && request.bookmark.resource() == reply.resource &&
         request.session == reply.session && request.revision == reply.revision && request.sequence == reply.sequence;
}

struct PositionChangeMap::Data {
  std::uint64_t fromRevision{};
  std::uint64_t toRevision{};
  std::uint64_t beforeCount{};
  std::uint64_t afterCount{};
  std::unique_ptr<PositionChange[]> changes;
  std::size_t count{};
};

std::expected<PositionChangeMap, SourceError>
PositionChangeMap::create(std::uint64_t fromRevision, std::uint64_t toRevision, std::uint64_t beforeCount,
                          std::span<const PositionChange> changes, PositionMapOptions options) {
  if (fromRevision == 0 || toRevision <= fromRevision || options.maxChanges == 0) {
    return std::unexpected(SourceError::INVALID_ARGUMENT);
  }

  if (changes.size() > options.maxChanges) {
    return std::unexpected(SourceError::WOULD_BLOCK);
  }

  auto count = beforeCount;

  for (const auto &change : changes) {
    if (change.first > count || change.removed > count - change.first) {
      return std::unexpected(SourceError::INVALID_ARGUMENT);
    }

    switch (change.kind) {
    case PositionChangeKind::SPLICE:
      if (change.destination != 0 ||
          change.inserted > std::numeric_limits<std::uint64_t>::max() - (count - change.removed)) {
        return std::unexpected(SourceError::INVALID_ARGUMENT);
      }

      count = count - change.removed + change.inserted;
      break;
    case PositionChangeKind::MOVE:
      if (change.removed == 0 || change.inserted != 0 || change.destination > count - change.removed) {
        return std::unexpected(SourceError::INVALID_ARGUMENT);
      }

      break;
    default:
      return std::unexpected(SourceError::INVALID_ARGUMENT);
    }
  }

  try {
    auto data = std::make_shared<Data>();

    data->fromRevision = fromRevision;
    data->toRevision = toRevision;
    data->beforeCount = beforeCount;
    data->afterCount = count;
    data->count = changes.size();

    if (!changes.empty()) {
      data->changes = std::make_unique<PositionChange[]>(changes.size());
      std::copy(changes.begin(), changes.end(), data->changes.get());
    }

    PositionChangeMap result;

    result.data_ = std::move(data);

    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(SourceError::NO_MEMORY);
  }
}

PositionChangeMap::operator bool() const noexcept {
  return static_cast<bool>(data_);
}

std::uint64_t PositionChangeMap::fromRevision() const noexcept {
  return data_ ? data_->fromRevision : 0;
}

std::uint64_t PositionChangeMap::toRevision() const noexcept {
  return data_ ? data_->toRevision : 0;
}

std::uint64_t PositionChangeMap::afterCount() const noexcept {
  return data_ ? data_->afterCount : 0;
}

std::expected<PositionMapping, SourceError> PositionChangeMap::map(ModelPosition position) const noexcept {
  if (!data_ || position.revision != data_->fromRevision) {
    return std::unexpected(SourceError::REVISION_CHANGED);
  }

  if (!validPosition(position)) {
    return std::unexpected(SourceError::INVALID_ARGUMENT);
  }

  if (position.kind == PositionKind::TEXT) {
    return std::unexpected(SourceError::UNSUPPORTED);
  }

  if (position.offset > data_->beforeCount ||
      (position.kind == PositionKind::ITEM && position.offset == data_->beforeCount)) {
    return std::unexpected(SourceError::INVALID_ARGUMENT);
  }

  PositionMapping result{position.offset, false};

  for (std::size_t index = 0; index != data_->count; ++index) {
    const auto &change = data_->changes[index];
    const bool gap = position.kind == PositionKind::GAP || result.removed;

    if (change.kind == PositionChangeKind::MOVE) {
      result.offset = mapMove(result.offset, change, gap, position.affinity);
    } else if (gap) {
      result.offset = mapGapSplice(result.offset, change, position.affinity);
    } else if (result.offset >= change.first && result.offset < change.first + change.removed) {
      result.offset = change.first + (position.affinity == PositionAffinity::FORWARD ? change.inserted : 0);
      result.removed = true;
    } else if (result.offset >= change.first + change.removed) {
      result.offset = result.offset - change.removed + change.inserted;
    }
  }

  return result;
}
} // namespace portals
