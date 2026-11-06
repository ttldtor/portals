// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/model_position.hpp>

#include <optional>
#include <thread>

namespace portals {
class ManagedScene;
struct LogLine;

namespace detail {
struct LogNavigationSession;
}

namespace ui::detail {
struct ExternalLogAccess;
}

/** @brief Bounded worker extension for log positions; unrelated to total extent.
 * The provider owns its logical resource. Capture and resolve run on the existing
 * application worker and must never retain borrowed commands or row payload.
 */
class LogBookmarkProvider : public PositionResolver {
public:
  /** @return Stable logical resource; obtaining it performs no I/O. */
  virtual ModelResource resource() const noexcept = 0;
  /** @param command Delivery session and exact requested revision.
   * @param row Immutable logical record borrowed for this call only.
   * @param options Maximum owning metadata allocation, including its envelope.
   * @param stop Cooperative worker cancellation.
   * @return Owning bookmark, or typed unsupported/history/allocation failure.
   */
  virtual std::expected<ModelBookmark, SourceError> capture(const SourceReadCommand &command,
                                                            const WindowRow<LogLine> &row, BookmarkOptions options,
                                                            std::stop_token stop = {}) const = 0;

  /** @param request Owning exact-resolution request on the application worker.
   * @param readiness Existing weak source notifier; a pending provider may copy it and publish
   * a strictly newer data revision when bounded resolution becomes available.
   * @param stop Cooperative worker cancellation.
   * @return Qualified outcome; the default delegates to PositionResolver::resolve.
   * Keeping the notifier does not retain a scene, source session or runtime.
   */
  virtual std::expected<PositionReply, SourceError> resolveLog(const PositionRequest &request,
                                                               std::weak_ptr<const WindowNotifier<LogLine>> readiness,
                                                               std::stop_token stop = {}) const {
    (void)readiness;

    return resolve(request, stop);
  }

  /** @param reply Qualified resolved position. @param session Current delivery namespace.
   * @return Worker-created transport anchor; cursor providers may override this mapping.
   * The default requires ITEM positions and the transport's seekRow capability.
   */
  virtual std::expected<SourceAnchor, SourceError> readAnchor(const PositionReply &reply, SourceStamp session) const {
    (void)session;

    if (reply.position.kind != PositionKind::ITEM) {
      return std::unexpected(SourceError::UNSUPPORTED);
    }

    return SourceAnchor{SourceAnchorKind::ROW, {}, reply.position.row};
  }
};

/** @brief Recoverable restore diagnostics; no outcome implicitly enables following. */
enum class LogRestoreReason {
  NONE,                ///< Exact restoration or no restore was requested.
  UNSUPPORTED,         ///< The provider has no bookmark capability.
  RESOURCE_MISMATCH,   ///< Resource, lineage or native identity is incompatible.
  HISTORY_UNAVAILABLE, ///< Supported historical data cannot be recovered.
  ANCHOR_MISMATCH,     ///< Bounded anchor validation failed.
  DELETED_SEGMENT,     ///< Provider proved deletion and selected a surviving neighbor.
  CHECKPOINT_CAPACITY  ///< Metadata admission failed before replacement.
};

/** @brief Observed status belonging to a navigation session, not a widget. */
struct LogRestoreStatus {
  PositionResolution resolution{PositionResolution::EXACT}; ///< Provider outcome.
  LogRestoreReason reason{LogRestoreReason::NONE};          ///< Why restoration could not be exact.
  std::optional<SourceError> error;                         ///< Optional typed operation failure.
};

/** @brief Configurable metadata bounds; no source-count limit or LRU eviction. */
struct LogCheckpointOptions {
  std::size_t maxBytes{256 * 1024}; ///< Aggregate session metadata capacity, including accounting allowances.
  BookmarkOptions bookmarks{};      ///< Maximum metadata for one owning bookmark; default 4096 bytes.
};

/** @brief Owning saved logical reading position; contains no source or view references. */
struct LogReadingCheckpoint {
  std::optional<ModelBookmark> bookmark; ///< Last valid native position; preserved on unavailable resolution.
  std::size_t byteInRecord{};            ///< Grapheme-aligned UTF-8 coordinate inside the logical record.
  bool following{};                      ///< Independent per-resource follow mode.
};

/** @brief Weak UI-affine checkpoint session owned by an existing scene.
 * Removing a view leaves this session alive. Handles retain no source, view or runtime.
 * Reusing a handle explicitly resumes its history; views otherwise navigate independently.
 */
class LogNavigation {
public:
  LogNavigation() = default; ///< Empty handle; queries return CLOSED.
  /** @return Latest recoverable restore status, WRONG_THREAD, or CLOSED. */
  std::expected<LogRestoreStatus, SourceError> status() const;
  /** @param resource Logical resource whose checkpoint is requested.
   * @return Owning saved values, an empty optional, WRONG_THREAD, or CLOSED.
   */
  std::expected<std::optional<LogReadingCheckpoint>, SourceError> checkpoint(ModelResource resource) const;
  /** @param resource Logical resource whose saved position is explicitly forgotten.
   * @return Success, WRONG_THREAD, or CLOSED; other resources are unchanged.
   * This erases only the saved entry, without moving an active view or requesting
   * redraw/work. Later ordinary preparation or navigation may capture its position again.
   */
  std::expected<void, SourceError> reset(ModelResource resource) const;

private:
  friend class ManagedScene;
  friend struct ui::detail::ExternalLogAccess;
  std::expected<std::shared_ptr<detail::LogNavigationSession>, SourceError> lock() const;
  std::weak_ptr<detail::LogNavigationSession> session_;
  std::thread::id owner_{};
};
} // namespace portals
