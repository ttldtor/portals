// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <concepts>
#include <optional>
#include <portals/model_position.hpp>
#include <thread>
#include <vector>

namespace portals {
class ManagedScene;
struct ManagedError;
template <class T> class ManagedModel;
template <std::equality_comparable T> class ManagedSource;

/// @cond INTERNAL
namespace ui::detail {
struct Access;
struct EditAccess;
} // namespace ui::detail

/// @endcond

namespace detail {
struct ManagedNavigationRecord;
struct EditNavigationParticipant;
struct ManagedNavigationRegistry;
} // namespace detail

/** @brief Remember a resource position by default; RESET explicitly selects its initial policy. */
enum class ModelRestorePolicy {
  REMEMBER, ///< Restore this navigation session's saved resource position.
  RESET     ///< Forget the target checkpoint and use its initial position/selection.
};

/** @brief Bounded history options, independent of model row count. */
struct ModelCheckpointOptions {
  std::size_t maxBytes{256 * 1024}; ///< Aggregate entry/selection/bookmark metadata credit.
  BookmarkOptions bookmarks{};      ///< Existing 4096-byte per-bookmark default.
};

/** @brief Explicit rebinding policy; omitted history options inherit the session's settings. */
struct ModelRebindOptions {
  ModelRestorePolicy restore{ModelRestorePolicy::REMEMBER}; ///< Default return-to-resource behavior.
  bool retainPreparedOverride{}; ///< Low-level opt-in to retaining a previous view callback across models.
  std::optional<ModelCheckpointOptions> checkpoints; ///< Initial session capacity; immutable once history exists.
};

/** @brief Published logical position, independent of provider revisions and screen coordinates.
 * Full revision-qualified bookmarks are read separately from ManagedNavigation::bookmark().
 * ITEM positions compare by complete row identity; their offset is always zero.
 */
struct NavigationPosition {
  ModelResource resource;                ///< Logical source domain, including compatible-history lineage.
  PositionKind kind{PositionKind::ITEM}; ///< Provider position kind.
  std::optional<RowId> row;              ///< Current item; empty for no-current or non-item positions.
  std::uint64_t offset{};                ///< Logical gap/text coordinate; zero for ITEM.
  PositionAffinity affinity{PositionAffinity::FORWARD};     ///< Explicit deletion/insertion side.
  PositionResolution resolution{PositionResolution::EMPTY}; ///< Last declared resolution outcome.
  /** @param other Logical value to compare.
   * @return True when no navigation delivery is needed.
   */
  bool operator==(const NavigationPosition &other) const noexcept = default;
};

/** @brief Immutable bounded selected identities; copies share payload without allocating. */
class NavigationSelection {
public:
  NavigationSelection() = default; ///< Empty selection, without allocating.
  /** @return Borrowed logical selected identities in resource()'s domain, valid while this value or a copy survives.
   * IDs alone do not prove current row presence/readiness. Unresolved restoration metadata
   * must not be applied as confirmed selection in another resource domain.
   */
  std::span<const RowId> rows() const noexcept;
  /** @return Domain qualifying every selected RowId, including an empty selected set. */
  ModelResource resource() const noexcept;
  /** @return Declared identity outcome; manual nonempty sets are EXACT, empty sets are EMPTY. */
  PositionResolution resolution() const noexcept;
  /** @param other Selection to compare by complete identities in canonical order.
   * @return True when both selected sets match.
   */
  bool operator==(const NavigationSelection &other) const noexcept;

private:
  friend class ManagedNavigation;
  friend class ManagedScene;
  friend struct detail::ManagedNavigationRecord;
  ModelResource resource_;
  PositionResolution resolution_{PositionResolution::EMPTY};
  std::shared_ptr<const std::vector<RowId>> rows_;
};

/** @brief Positive bounded metadata limits; no source extent or viewport is retained. */
struct NavigationOptions {
  std::size_t maxSelectedRows{1024};          ///< Maximum explicit selected set, independent of current/focus.
  BookmarkOptions bookmarks{};                ///< Maximum owning metadata for each captured position.
  std::size_t maxCheckpointBytes{256 * 1024}; ///< Aggregate remembered resource metadata for this context.
};

/** @brief Explicit projection-domain agreement; absence from a projection never proves deletion. */
enum class NavigationProjection {
  SAME_MODEL,       ///< Binding must use the context's original managed model.
  PRESERVES_ROW_IDS ///< Caller guarantees identical complete RowIds in the original source domain.
};

/** @brief Independent opt-in view channels; scrolling never changes current by itself. */
struct NavigationBindingOptions {
  NavigationProjection projection{NavigationProjection::SAME_MODEL}; ///< Explicit source-domain agreement.
  bool current{true};   ///< Share stable current using the control's existing single-selection policy.
  bool readingAnchor{}; ///< Share logical first-row anchors, mapped independently in each projection.
};

/** @brief Weak UI-affine navigation handle owned by an existing scene/model scope.
 * Removing a view does not remove its context. Closing the scene cancels pending resolution
 * and retires all callbacks. Handles retain no model, snapshot, widget, source or runtime.
 * Copied immutable channel snapshots and bookmarks remain ordinary independent values.
 */
class ManagedNavigation {
public:
  ManagedNavigation() = default; ///< Empty handle; operations return CLOSED.
  /** @tparam T Immutable row type shared by the context and all participating views.
   * @param model Same-scene destination; provider work is bounded and never performs UI I/O.
   * @param options Remember/reset and bounded history policy.
   * @return Atomic group admission, or a checked error preserving the installed group.
   */
  template <class T>
  std::expected<void, ManagedError> setModel(ManagedModel<T> model, ModelRebindOptions options = {}) const;
  /** @tparam T Immutable common domain row type.
   * @param model Destination model. @param options Same policy as setModel.
   * @return True on admission; false reports the original error once through the scene.
   */
  template <class T> bool model(ManagedModel<T> model, ModelRebindOptions options = {}) const;
  /** @return Read-only current channel, WRONG_THREAD, or CLOSED. */
  std::expected<ManagedSource<NavigationPosition>, TreeError> current() const;
  /** @return Read-only selected-set channel, independent of current and keyboard focus. */
  std::expected<ManagedSource<NavigationSelection>, TreeError> selection() const;
  /** @return Explicit logical reading-anchor channel, independent of current and pixels. */
  std::expected<ManagedSource<NavigationPosition>, TreeError> readingAnchor() const;
  /** @return Latest revision-qualified current metadata, or checked access failure. */
  std::expected<std::optional<ModelBookmark>, TreeError> bookmark() const;
  /** @return Latest revision-qualified reading metadata, or checked access failure. */
  std::expected<std::optional<ModelBookmark>, TreeError> readingBookmark() const;
  /** @param id Stable row in the original finite model, resolved through its bounded index.
   * @return Success, WOULD_BLOCK for unavailable identity/data, or checked access/provider failure.
   */
  std::expected<void, TreeError> navigate(RowId id) const;
  /** @param bookmark Owning compatible source metadata, without retaining a provider.
   * @return Success or validation/access failure; same logical current emits no notification.
   */
  std::expected<void, TreeError> setCurrent(ModelBookmark bookmark) const;
  /** @return Success or checked access failure; cancels any older resolution request. */
  std::expected<void, TreeError> clearCurrent() const;
  /** @param rows Selected IDs in this context's source domain; copied and canonicalized.
   * @return Success, NODE_LIMIT for the configured limit, or validation/access/allocation failure.
   */
  std::expected<void, TreeError> setSelection(std::span<const RowId> rows) const;
  /** @param bookmark Owning logical reading anchor, never a viewport offset.
   * @return Success or validation/access failure; current and selection are unchanged.
   */
  std::expected<void, TreeError> setReadingAnchor(ModelBookmark bookmark) const;
  /** @return Success or checked access failure; leaves current and selection unchanged. */
  std::expected<void, TreeError> clearReadingAnchor() const;
  /** @param changes Complete bounded proof for a future finite-model reconciliation.
   * @return Success or checked access failure. Missing proof never permits ordinal guessing.
   */
  std::expected<void, TreeError> setChangeMap(PositionChangeMap changes) const;
  /** @param revision Exact target provider revision in this context's delivery session.
   * @return Qualified metadata-only request, WOULD_BLOCK without current, or checked failure.
   * The caller uses its existing worker path; this method neither dispatches nor blocks.
   */
  std::expected<PositionRequest, TreeError> beginResolve(std::uint64_t revision) const;
  /** @param reply Qualified provider receipt; stale results cannot replace newer navigation.
   * @return True when admitted, false when stale, or malformed/access/allocation failure.
   */
  std::expected<bool, TreeError> applyReply(const PositionReply &reply) const;
  /** @return Cooperative token for the currently admitted request, or checked access failure. */
  std::expected<std::stop_token, TreeError> stopToken() const;

private:
  friend class ManagedScene;
  friend struct ui::detail::Access;
  friend struct ui::detail::EditAccess;
  std::expected<std::shared_ptr<detail::ManagedNavigationRecord>, TreeError> lock() const;
  std::weak_ptr<detail::ManagedNavigationRecord> record_;
  std::thread::id owner_{};
};
} // namespace portals
