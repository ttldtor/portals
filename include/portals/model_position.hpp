// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/window_source.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <stop_token>
#include <utility>

namespace portals {
/** @brief Logical resource namespace, distinct from its current delivery session.
 * Reuse an existing SourceStamp as the resource origin; no second identity issuer is
 * needed. Keep lineage stable only while the provider can map old positions. A new
 * unrelated document at the same path needs a different identity or lineage.
 */
struct ModelResource {
  SourceStamp identity;    ///< Existing origin namespace; both fields must be nonzero.
  std::uint64_t lineage{}; ///< Nonzero provider-maintained compatible-history generation.
  /** @return New resource using the existing source identity issuer, with initial lineage one.
   * Returns IDENTITY_EXHAUSTED instead of reusing a namespace.
   */
  static std::expected<ModelResource, SourceError> create();
  /** @param other Resource namespace to compare.
   * @return True when identity and compatible-history generation match.
   */
  bool operator==(const ModelResource &other) const noexcept = default;
};

/** @brief Position interpretation; a row, a gap, and a text mark are not interchangeable. */
enum class PositionKind {
  ITEM, ///< Full RowId with a version-qualified ordinal hint.
  GAP,  ///< Boundary between rows, including the entry and end gaps.
  TEXT  ///< Provider-defined text mark; providers must preserve grapheme boundaries.
};

/** @brief Preferred surviving side when a position is deleted or text is inserted beside a mark. */
enum class PositionAffinity {
  FORWARD, ///< Prefer the following item; an end boundary falls back to the preceding item.
  BACKWARD ///< Prefer the preceding item; an entry boundary falls back to the following item.
};

/** @brief Owning scalar position; offset is meaningful only in the declared data revision. */
struct ModelPosition {
  PositionKind kind{PositionKind::ITEM};                ///< Provider operation to resolve.
  RowId row;                                            ///< Full item identity; empty for GAP/TEXT.
  std::uint64_t offset{};                               ///< Ordinal/gap/text hint, never a pointer.
  std::uint64_t revision{};                             ///< Positive revision in which the hint was captured.
  PositionAffinity affinity{PositionAffinity::FORWARD}; ///< Explicit insertion/deletion preference.
};

/** @brief Checked bookmark capacity, independent of logical source length. */
struct BookmarkOptions {
  std::size_t maxBytes{4096}; ///< Header, opaque bytes and conservative payload-allocation allowance.
};

/** @brief Copyable bookmark owning metadata only; never retains model, snapshot, view or page.
 * Copies share immutable opaque bytes without allocation. Resource identity is
 * process-local unless the provider supplies a portable identity/codec on export.
 * This value alone cannot reopen a source or recreate missing model data.
 */
class ModelBookmark {
public:
  static constexpr std::uint32_t FORMAT_VERSION = 1; ///< Version of this in-memory bookmark schema.
  /** @brief Empty invalid bookmark; constructing it allocates no memory. */
  ModelBookmark() = default;
  /** @param other Immutable metadata owner to share without allocating. */
  ModelBookmark(const ModelBookmark &other) = default;

  /** @param other Metadata owner to transfer, leaving an empty invalid bookmark. */
  ModelBookmark(ModelBookmark &&other) noexcept
      : resource_(std::exchange(other.resource_, {})), position_(std::exchange(other.position_, {})),
        metadata_(std::move(other.metadata_)), metadataSize_(std::exchange(other.metadataSize_, 0)) {
  }

  /** @param other Complete replacement; old storage is released after all fields change.
   * @return This value; copying immutable payload ownership does not allocate.
   */
  ModelBookmark &operator=(ModelBookmark other) noexcept {
    std::swap(resource_, other.resource_);
    std::swap(position_, other.position_);
    std::swap(metadata_, other.metadata_);
    std::swap(metadataSize_, other.metadataSize_);

    return *this;
  }

  /** @param resource Logical resource and compatible-history generation.
   * @param position Valid typed position in a positive data revision.
   * @param metadata Optional provider-owned opaque bytes, copied before success.
   * @param options Maximum conservatively charged metadata size.
   * @return Immutable bookmark, INVALID_ARGUMENT, WOULD_BLOCK or NO_MEMORY.
   */
  static std::expected<ModelBookmark, SourceError> create(ModelResource resource, ModelPosition position,
                                                          std::span<const std::byte> metadata = {},
                                                          BookmarkOptions options = {});
  /** @return Logical identity; empty for a default bookmark. */
  ModelResource resource() const noexcept;
  /** @return Captured scalar position, with no borrowed row data. */
  ModelPosition position() const noexcept;
  /** @return Metadata borrowed until this value or a copy is released. */
  std::span<const std::byte> metadata() const noexcept;
  /** @return Conservatively charged metadata bytes, not process RSS. */
  std::size_t storageBytes() const noexcept;

  /** @return In-memory schema version; persistence codecs are a separate capability. */
  constexpr std::uint32_t formatVersion() const noexcept {
    return FORMAT_VERSION;
  }

private:
  ModelResource resource_;
  ModelPosition position_;
  std::shared_ptr<const std::byte[]> metadata_;
  std::size_t metadataSize_{};
};

/** @brief Successful protocol outcomes; absence of support differs from missing history. */
enum class PositionResolution {
  EXACT,        ///< The same item or a proven mapped gap/mark is present.
  NEAREST,      ///< The original item was removed; a declared surviving neighbor was selected.
  EMPTY,        ///< Compatible source is empty and has no current item.
  PENDING,      ///< Provider has not materialized the required bounded data yet.
  UNSUPPORTED,  ///< The provider does not implement the requested kind or identity capability.
  INCOMPATIBLE, ///< Resource, lineage or change proof cannot identify this position safely.
  UNAVAILABLE   ///< Supported position history or storage is no longer available.
};

/** @brief Immutable qualified navigation request; does not start work by itself. */
struct PositionRequest {
  SourceStamp session;      ///< Current delivery session, not the bookmark origin.
  std::uint64_t revision{}; ///< Exact target data revision.
  std::uint64_t sequence{}; ///< Positive request generation assigned by the existing caller.
  ModelBookmark bookmark;   ///< Owning source position, independent of the originating view.
};

/** @brief Owning resolution receipt; meaningful only for its matching live request. */
struct PositionReply {
  ModelResource resource;                                ///< Logical resource qualified by the request.
  SourceStamp session;                                   ///< Delivery session that resolved the request.
  std::uint64_t revision{};                              ///< Exact target data revision.
  std::uint64_t sequence{};                              ///< Original positive request generation.
  PositionResolution state{PositionResolution::PENDING}; ///< Outcome, not an inferred row count.
  ModelPosition position;                                ///< Meaningful for EXACT/NEAREST, empty for EMPTY.
};

/** @param request Still-current navigation request.
 * @param reply Completed provider receipt.
 * @return True when resource, session, target revision and request generation all match.
 * This does not validate provider-specific position semantics or apply any UI state.
 */
bool positionReplyMatches(const PositionRequest &request, const PositionReply &reply) noexcept;

/** @brief Bounded provider extension independent of views, schedulers, and total extent.
 * Implementations must not perform I/O or unbounded scans on the UI thread. I/O
 * resolvers run on the existing source worker path and honor cooperative cancellation.
 * Return PENDING rather than block for absent data. No second task/runtime is created.
 */
class PositionResolver {
public:
  virtual ~PositionResolver() = default; ///< Release only the resolver's own bounded metadata.
  /** @param request Owning position and exact target qualification.
   * @param stop Cooperative cancellation from the existing delivery scope.
   * @return Qualified outcome or typed failure; no view/model state is changed.
   */
  virtual std::expected<PositionReply, SourceError> resolve(const PositionRequest &request,
                                                            std::stop_token stop = {}) const = 0;
};

/** @brief Operation in a provider-declared finite ordering change map. */
enum class PositionChangeKind {
  SPLICE, ///< Remove a contiguous range, then insert rows at that boundary.
  MOVE    ///< Move a range to an index in the sequence after removing that range.
};

/** @brief Scalar ordering operation; payload and row identities remain in the model. */
struct PositionChange {
  PositionChangeKind kind{PositionChangeKind::SPLICE}; ///< Operation interpretation.
  std::uint64_t first{};                               ///< Range start in the preceding operation's result.
  std::uint64_t removed{};                             ///< Removed or moved row count.
  std::uint64_t inserted{};                            ///< New count for SPLICE; zero for MOVE.
  std::uint64_t destination{};                         ///< MOVE destination after removing the range.

  /** @param first Range start.
   * @param removed Removed count.
   * @param inserted Inserted count.
   * @return An ordering splice, without allocating payload.
   */
  static constexpr PositionChange splice(std::uint64_t first, std::uint64_t removed, std::uint64_t inserted) noexcept {
    return {PositionChangeKind::SPLICE, first, removed, inserted, 0};
  }

  /** @param first Range start.
   * @param count Number of rows to move.
   * @param destination Index in the sequence after removing the range.
   * @return A move preserving identities, without allocating payload.
   */
  static constexpr PositionChange move(std::uint64_t first, std::uint64_t count, std::uint64_t destination) noexcept {
    return {PositionChangeKind::MOVE, first, count, 0, destination};
  }
};

/** @brief Bounded change-proof admission options; no model rows are retained. */
struct PositionMapOptions {
  std::size_t maxChanges{1024}; ///< Maximum scalar operations in one retained map.
};

/** @brief Proven mapped ordinal or gap; removed items become a gap with the requested affinity. */
struct PositionMapping {
  std::uint64_t offset{}; ///< Current item ordinal, or gap when removed is true.
  bool removed{};         ///< The original item was removed by the declared operations.
};

/** @brief Immutable bounded provider proof between two revisions of a finite order.
 * The provider must report all ordering changes truthfully; validation checks shape
 * and arithmetic, not the contents of two snapshots. O(operation count), never O(rows).
 * The operation cap is configurable. Large proofs belong on the existing worker
 * path; constructing a map does not dispatch work or make a UI scan acceptable.
 */
class PositionChangeMap {
public:
  PositionChangeMap() = default; ///< No proof; does not allocate.
  /** @param fromRevision Positive revision in which positions were captured.
   * @param toRevision Strictly newer target revision.
   * @param beforeCount Row count in fromRevision; metadata only.
   * @param changes Complete ordered splices/moves, copied before success.
   * @param options Maximum retained operation count.
   * @return Validated immutable proof, INVALID_ARGUMENT, WOULD_BLOCK or NO_MEMORY.
   */
  static std::expected<PositionChangeMap, SourceError> create(std::uint64_t fromRevision, std::uint64_t toRevision,
                                                              std::uint64_t beforeCount,
                                                              std::span<const PositionChange> changes,
                                                              PositionMapOptions options = {});
  /** @return True when this value contains a declared revision mapping. */
  explicit operator bool() const noexcept;
  /** @return Original revision, or zero for no proof. */
  std::uint64_t fromRevision() const noexcept;
  /** @return Target revision, or zero for no proof. */
  std::uint64_t toRevision() const noexcept;
  /** @return Target finite row count, or zero for no proof. */
  std::uint64_t afterCount() const noexcept;
  /** @param position Item/gap captured in fromRevision.
   * @return Mapped ordinal/gap, or INVALID_ARGUMENT/UNSUPPORTED/REVISION_CHANGED.
   */
  std::expected<PositionMapping, SourceError> map(ModelPosition position) const noexcept;

private:
  struct Data;
  std::shared_ptr<const Data> data_;
};
} // namespace portals
