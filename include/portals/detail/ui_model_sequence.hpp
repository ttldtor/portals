// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/collection.hpp>
#include <portals/model_position.hpp>

namespace portals::ui::detail {
/// @cond INTERNAL
struct OwnedChange {
  std::uint64_t revision;
  std::uint64_t beforeCount;
  PositionChange operation;
};

class OwnedHistory final : public PositionChangeHistory {
public:
  OwnedHistory(std::uint64_t revision, PositionMapOptions options) : revision_(revision), options_(options) {
  }

  OwnedHistory(const OwnedHistory &previous, std::uint64_t beforeCount, PositionChange operation)
      : revision_(previous.revision_ + 1), options_(previous.options_) {
    const auto skip =
      previous.changes_.size() >= options_.maxChanges ? previous.changes_.size() - options_.maxChanges + 1 : 0;
    changes_.reserve(previous.changes_.size() - skip + 1);
    changes_.insert(changes_.end(), previous.changes_.begin() + skip, previous.changes_.end());
    changes_.push_back({previous.revision_, beforeCount, operation});
  }

  std::expected<PositionHistoryReply, SourceError> changesFrom(std::uint64_t revision) const override {
    if (!revision || revision > revision_) {
      return std::unexpected(SourceError::REVISION_CHANGED);
    }
    const auto floor = changes_.empty() ? revision_ : changes_.front().revision;
    if (revision < floor) {
      return PositionHistoryReply{{}, floor, PositionResolution::UNAVAILABLE};
    }
    if (revision == revision_) {
      return PositionHistoryReply{{}, floor, PositionResolution::EXACT};
    }
    const auto first = std::ranges::lower_bound(changes_, revision, {}, &OwnedChange::revision);
    if (first == changes_.end() || first->revision != revision) {
      return std::unexpected(SourceError::REVISION_CHANGED);
    }
    try {
      std::vector<PositionChange> suffix(0);
      suffix.reserve(static_cast<std::size_t>(changes_.end() - first));
      for (auto entry = first; entry != changes_.end(); ++entry) {
        suffix.push_back(entry->operation);
      }
      auto proof = PositionChangeMap::create(revision, revision_, first->beforeCount, suffix, options_);
      return proof ? std::expected<PositionHistoryReply, SourceError>(
                       PositionHistoryReply{std::move(*proof), floor, PositionResolution::EXACT})
                   : std::unexpected(proof.error());
    } catch (const std::bad_alloc &) {
      return std::unexpected(SourceError::NO_MEMORY);
    } catch (...) {
      return std::unexpected(SourceError::CALLBACK_FAILED);
    }
  }

private:
  std::vector<OwnedChange> changes_ = std::vector<OwnedChange>(0);
  std::uint64_t revision_;
  PositionMapOptions options_;
};

template <class T> class OwnedRows final : public ModelSnapshot<T> {
public:
  OwnedRows(const std::vector<ModelRow<T>> &rows, std::uint64_t revision, std::shared_ptr<const OwnedHistory> history)
      : ModelSnapshot<T>(revision, rows.size()), rows_(rows), history_(std::move(history)) {
    index_.reserve(rows_.size());
    for (std::size_t offset = 0; offset < rows_.size(); ++offset) {
      index_.emplace_back(rows_[offset].id.value, offset);
    }
    std::ranges::sort(index_);
  }

  ModelRows<T> read(RowRange range) const override {
    return {RangeState::READY, std::span(rows_).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }

  RowLookup locateRow(RowId id) const override {
    const auto found = std::ranges::lower_bound(index_, id.value, {}, &std::pair<std::uint64_t, std::uint64_t>::first);
    return id.generation == 1 && id.value && found != index_.end() && found->first == id.value
             ? RowLookup{RowLookupState::FOUND, found->second}
             : RowLookup{RowLookupState::NOT_FOUND, 0};
  }

  std::shared_ptr<const PositionChangeHistory> positionHistory() const override {
    return history_;
  }

  std::span<const ModelRow<T>> rows() const noexcept {
    return rows_;
  }

  const OwnedHistory &history() const noexcept {
    return *history_;
  }

private:
  std::vector<ModelRow<T>> rows_;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> index_ = std::vector<std::pair<std::uint64_t, std::uint64_t>>(0);
  std::shared_ptr<const OwnedHistory> history_;
};

template <class T> struct OwnedSequence {
  std::shared_ptr<const OwnedRows<T>> current;
  std::size_t capacity{CollectionLimits{}.maxRows};
  PositionMapOptions history;
  std::uint64_t nextId{1};
  bool editing{};
};

/// @endcond
} // namespace portals::ui::detail
