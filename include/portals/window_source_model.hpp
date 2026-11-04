// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/model.hpp>
#include <portals/window_source.hpp>

namespace portals {
/** @brief Opt-in finite Model adapter without changing exact-count Model semantics.
 * Keeps the existing immutable domain snapshot externally owned; copies only inline values into
 * charged pages. This read-only delivery adapter does not forward model editing, query,
 * search, or extent APIs; retain the original model binding for those operations.
 * Dynamic values need an explicit storage codec and are deliberately not accepted.
 * The snapshot must satisfy ModelSnapshot's worker-safe read/destruction contract.
 * Create the WindowSource with initialRevision equal to snapshot->revision().
 * @tparam T Trivially copyable immutable inline domain value.
 */
template <class T>
  requires std::is_trivially_copyable_v<T>
class FiniteWindowProvider final : public WindowProvider<T> {
public:
  /** @param snapshot Non-null immutable finite data; external domain storage is not a page cache.
   * @param seekRow Enable only when locateRow is a bounded worker-safe index.
   */
  explicit FiniteWindowProvider(std::shared_ptr<const ModelSnapshot<T>> snapshot, bool seekRow = false)
      : snapshot_(std::move(snapshot)), seekRow_(seekRow) {
  }

  /** @return Finite backward/tail capability and the explicitly enabled identity index. */
  SourceCapabilities capabilities() const noexcept override {
    return {true, seekRow_, true};
  }

  /** @param command Bounded cursor/entry/tail/identity request.
   * @param out Reserved page builder.
   * @param stop Cooperative cancellation.
   * @return Up to maxRows; finite boundaries use FINAL_END, never a guessed count.
   */
  std::expected<SourceReadReply<T>, SourceError> load(const SourceReadCommand &command, PageBuilder<T> &out,
                                                      std::stop_token stop) override {
    if (!snapshot_) {
      return std::unexpected(SourceError::INVALID_ARGUMENT);
    }

    if (command.dataRevision != snapshot_->revision()) {
      return std::unexpected(SourceError::REVISION_CHANGED);
    }

    auto position = std::uint64_t{};

    switch (command.anchor.kind) {
    case SourceAnchorKind::ENTRY:
      position = command.direction == SourceDirection::FORWARD ? 0 : snapshot_->rowCount();
      break;
    case SourceAnchorKind::TAIL:
      position = snapshot_->rowCount();
      break;
    case SourceAnchorKind::CURSOR:
      if (command.anchor.cursor.token[1] != 0 || command.anchor.cursor.token[0] > snapshot_->rowCount()) {
        return std::unexpected(SourceError::INVALID_CURSOR);
      }

      position = command.anchor.cursor.token[0];
      break;
    case SourceAnchorKind::ROW: {
      auto located = snapshot_->locateRow(command.anchor.row);

      if (!seekRow_ || located.state == RowLookupState::UNSUPPORTED) {
        return std::unexpected(SourceError::UNSUPPORTED);
      }

      if (located.state != RowLookupState::FOUND || located.index >= snapshot_->rowCount()) {
        return std::unexpected(SourceError::ROW_NOT_FOUND);
      }

      position = located.index + (command.direction == SourceDirection::BACKWARD ? 1 : 0);
      break;
    }
    }

    const bool backwards = command.direction == SourceDirection::BACKWARD;
    // TAIL addresses the available final window regardless of the traversal direction.
    const bool tail = command.anchor.kind == SourceAnchorKind::TAIL;
    auto available = backwards || tail ? position : snapshot_->rowCount() - position;
    auto count = static_cast<std::size_t>(std::min<std::uint64_t>(available, command.maxRows));
    auto first = backwards || tail ? position - count : position;
    auto range = ModelRange<T>::read(snapshot_, {first, count}, command.maxRows);

    if (!range) {
      return std::unexpected(detail::sourceTreeError(range.error()));
    }

    if (range->state() == RangeState::PENDING) {
      SourceReadReply<T> pending;
      pending.observedRevision = command.dataRevision;

      return pending;
    }

    if (range->state() == RangeState::FAILED) {
      return std::unexpected(SourceError::CALLBACK_FAILED);
    }

    for (const auto &row : range->rows()) {
      if (stop.stop_requested()) {
        return std::unexpected(SourceError::CANCELLED);
      }

      auto appended = out.emplace(row.id, snapshot_->revision(), row.value);

      if (!appended) {
        return std::unexpected(appended.error());
      }
    }

    auto edge =
      (backwards ? first == 0 : first + count == snapshot_->rowCount()) ? SourceEdge::FINAL_END : SourceEdge::MORE;

    return out.ready({command.source, {first, 0}}, {command.source, {first + count, 0}}, edge);
  }

private:
  std::shared_ptr<const ModelSnapshot<T>> snapshot_;
  bool seekRow_;
};
} // namespace portals
