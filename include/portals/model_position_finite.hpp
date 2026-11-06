// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/model.hpp>
#include <portals/model_position.hpp>

namespace portals {
/** @brief Read-only finite model navigation without retaining the source or any row payload.
 * Snapshot ownership remains with the model/application. Each call temporarily locks
 * the weak snapshot, returning CLOSED after its last owner retires. Calls may run
 * concurrently only when the snapshot's read/index implementation is worker-safe.
 * locateRow must be a bounded index. No fallback scans or ordinal guessing are used.
 * For UI use, keep change proofs small; large configurable proofs run on the existing
 * worker path. This adapter neither schedules work nor subscribes to model updates.
 * @tparam T Immutable domain row type; no domain values are copied.
 */
template <class T> class FinitePositionResolver final : public PositionResolver {
public:
  /** @param resource Logical resource stable across compatible snapshots.
   * @param session Existing current delivery session, qualified on every resolve.
   * @param snapshot Non-null immutable finite snapshot, stored weakly.
   * @param changes Optional complete ordering proof ending in snapshot->revision().
   * @return Resolver, or INVALID_ARGUMENT for inconsistent identity/map metadata.
   */
  static std::expected<FinitePositionResolver, SourceError> create(ModelResource resource, SourceStamp session,
                                                                   std::shared_ptr<const ModelSnapshot<T>> snapshot,
                                                                   PositionChangeMap changes = {}) {
    if (!snapshot || resource.identity.instance == 0 || resource.identity.epoch == 0 || resource.lineage == 0 ||
        session.instance == 0 || session.epoch == 0 || snapshot->revision() == 0 ||
        (changes && (changes.toRevision() != snapshot->revision() || changes.afterCount() != snapshot->rowCount()))) {
      return std::unexpected(SourceError::INVALID_ARGUMENT);
    }

    return FinitePositionResolver(resource, session, snapshot, std::move(changes));
  }

  /** @param index One current finite row to capture; reads exactly one row, without scanning.
   * @param affinity Preferred neighbor after a later deletion.
   * @param options Metadata capacity for the resulting bookmark.
   * @return Owning bookmark; WOULD_BLOCK for pending data, CLOSED, or typed provider failure.
   */
  std::expected<ModelBookmark, SourceError> capture(std::uint64_t index,
                                                    PositionAffinity affinity = PositionAffinity::FORWARD,
                                                    BookmarkOptions options = {}) const {
    auto snapshot = snapshot_.lock();
    const auto resource = resource_;

    if (!snapshot) {
      return std::unexpected(SourceError::CLOSED);
    }

    auto row = readRow(snapshot, index);

    if (!row) {
      return std::unexpected(row.error());
    }

    if (!*row) {
      return std::unexpected(SourceError::WOULD_BLOCK);
    }

    return ModelBookmark::create(resource, {PositionKind::ITEM, **row, index, snapshot->revision(), affinity}, {},
                                 options);
  }

  /** @param request Bookmark plus current delivery session/revision/request generation.
   * @param stop Cooperative cancellation checked before and after provider calls.
   * @return Qualified EXACT/NEAREST/EMPTY/PENDING/UNSUPPORTED/INCOMPATIBLE receipt or typed error.
   * The model, bookmark and caller's current/selection/viewport are never modified.
   */
  std::expected<PositionReply, SourceError> resolve(const PositionRequest &request,
                                                    std::stop_token stop = {}) const override {
    if (request.sequence == 0 || request.revision == 0 || request.bookmark.position().revision == 0) {
      return std::unexpected(SourceError::INVALID_ARGUMENT);
    }

    if (request.session != session_) {
      return std::unexpected(SourceError::STALE_SOURCE);
    }

    if (stop.stop_requested()) {
      return std::unexpected(SourceError::CANCELLED);
    }

    auto snapshot = snapshot_.lock();

    if (!snapshot) {
      return std::unexpected(SourceError::CLOSED);
    }

    if (request.revision != snapshot->revision()) {
      return std::unexpected(SourceError::REVISION_CHANGED);
    }

    auto reply = receipt(request);
    const auto position = request.bookmark.position();

    if (request.bookmark.resource() != resource_ || position.revision > snapshot->revision()) {
      reply.state = PositionResolution::INCOMPATIBLE;

      return reply;
    }

    if (position.kind == PositionKind::TEXT || !request.bookmark.metadata().empty()) {
      reply.state = PositionResolution::UNSUPPORTED;

      return reply;
    }

    if (position.kind == PositionKind::GAP) {
      return resolveGap(reply, position, *snapshot);
    }

    // Provider calls may release external owners. Retain the complete remaining
    // resolution state on the stack before invoking either provider operation.
    const auto changes = changes_;

    try {
      return resolveItem(reply, position, snapshot, changes, stop);
    } catch (const std::bad_alloc &) {
      return std::unexpected(SourceError::NO_MEMORY);
    } catch (...) {
      return std::unexpected(SourceError::CALLBACK_FAILED);
    }
  }

private:
  FinitePositionResolver(ModelResource resource, SourceStamp session,
                         const std::shared_ptr<const ModelSnapshot<T>> &snapshot, PositionChangeMap changes)
      : resource_(resource), session_(session), snapshot_(snapshot), changes_(std::move(changes)) {
  }

  PositionReply receipt(const PositionRequest &request) const noexcept {
    return {resource_, session_, request.revision, request.sequence, PositionResolution::INCOMPATIBLE, {}};
  }

  static std::expected<std::optional<RowId>, SourceError>
  readRow(const std::shared_ptr<const ModelSnapshot<T>> &snapshot, std::uint64_t index) {
    if (index >= snapshot->rowCount()) {
      return std::unexpected(SourceError::INVALID_ARGUMENT);
    }

    auto range = ModelRange<T>::read(snapshot, {index, 1}, 1);

    if (!range) {
      return std::unexpected(detail::sourceTreeError(range.error()));
    }

    if (range->state() == RangeState::PENDING) {
      return std::optional<RowId>{};
    }

    if (range->state() != RangeState::READY) {
      return std::unexpected(SourceError::CALLBACK_FAILED);
    }

    const auto id = range->rows().front().id;

    if (id.value == 0 || id.generation == 0) {
      return std::unexpected(SourceError::INVALID_REPLY);
    }

    return std::optional<RowId>{id};
  }

  std::expected<PositionReply, SourceError> resolveGap(PositionReply reply, ModelPosition position,
                                                       const ModelSnapshot<T> &snapshot) const noexcept {
    if (position.revision != snapshot.revision()) {
      if (!changes_ || changes_.fromRevision() != position.revision) {
        return reply;
      }

      auto mapped = changes_.map(position);

      if (!mapped) {
        return std::unexpected(mapped.error());
      }

      position.offset = mapped->offset;
    }

    if (position.offset > snapshot.rowCount()) {
      return std::unexpected(SourceError::INVALID_ARGUMENT);
    }

    position.revision = snapshot.revision();
    reply.state = PositionResolution::EXACT;
    reply.position = position;

    return reply;
  }

  static std::expected<PositionReply, SourceError> resolveItem(PositionReply reply, ModelPosition position,
                                                               const std::shared_ptr<const ModelSnapshot<T>> &snapshot,
                                                               const PositionChangeMap &changes, std::stop_token stop) {
    const auto found = snapshot->locateRow(position.row);

    if (stop.stop_requested()) {
      return std::unexpected(SourceError::CANCELLED);
    }

    if (found.state == RowLookupState::UNSUPPORTED) {
      reply.state = PositionResolution::UNSUPPORTED;

      return reply;
    }

    auto offset = found.index;
    auto outcome = PositionResolution::EXACT;

    if (found.state == RowLookupState::NOT_FOUND) {
      if (!changes || changes.fromRevision() != position.revision) {
        return reply;
      }

      auto mapped = changes.map(position);

      if (!mapped) {
        return std::unexpected(mapped.error());
      }

      if (!mapped->removed) {
        return reply;
      }

      if (snapshot->rowCount() == 0) {
        reply.state = PositionResolution::EMPTY;

        return reply;
      }

      offset = mapped->offset;

      if (position.affinity == PositionAffinity::BACKWARD && offset != 0) {
        --offset;
      }

      offset = std::min(offset, snapshot->rowCount() - 1);
      outcome = PositionResolution::NEAREST;
    } else if (found.state != RowLookupState::FOUND || offset >= snapshot->rowCount()) {
      return std::unexpected(SourceError::INVALID_REPLY);
    }

    auto row = readRow(snapshot, offset);

    if (!row) {
      return std::unexpected(row.error());
    }

    if (stop.stop_requested()) {
      return std::unexpected(SourceError::CANCELLED);
    }

    if (!*row) {
      reply.state = PositionResolution::PENDING;

      return reply;
    }

    if (outcome == PositionResolution::EXACT && **row != position.row) {
      return std::unexpected(SourceError::INVALID_REPLY);
    }

    position.row = **row;
    position.offset = offset;
    position.revision = snapshot->revision();
    reply.state = outcome;
    reply.position = position;

    return reply;
  }

  ModelResource resource_;
  SourceStamp session_;
  std::weak_ptr<const ModelSnapshot<T>> snapshot_;
  PositionChangeMap changes_;
};
} // namespace portals
