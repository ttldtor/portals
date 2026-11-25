// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/model_position.hpp>

#include <string_view>
#include <vector>

namespace portals {
/** @brief Purpose of a saved location; EDIT_ANCHOR never conveys write authority. */
enum class NavigationCheckpointRole {
  CURRENT,        ///< Current record or text caret location.
  READING_ANCHOR, ///< Reading location, independently of current selection.
  SELECTION,      ///< One selected location; multiple entries preserve their order.
  EDIT_ANCHOR     ///< Destination location only; capabilities and versions must be checked again.
};

/** @brief A metadata-only position and its purpose; never a model value or draft. */
struct NavigationCheckpointEntry {
  NavigationCheckpointRole role{NavigationCheckpointRole::CURRENT}; ///< Intended navigation channel.
  ModelBookmark bookmark; ///< Qualified in-memory position; provider codecs translate its resource identity.
};

/** @brief Persistence failures are separate from navigation resolution outcomes. */
enum class NavigationCheckpointError {
  INVALID_ARGUMENT,          ///< Invalid options, role or input bookmark.
  CORRUPT_INPUT,             ///< Malformed envelope, invalid checksum or inconsistent provider result.
  TOO_LARGE,                 ///< Configured envelope, entry, identity or count limit exceeded.
  UNSUPPORTED_VERSION,       ///< Unknown envelope schema.
  UNSUPPORTED_CODEC_VERSION, ///< Matching provider codec name uses an unsupported schema version.
  UNSUPPORTED_CODEC,         ///< Saved codec identifier does not match the supplied provider.
  WRONG_RESOURCE,            ///< Portable resource qualification does not match the supplied provider.
  UNSUPPORTED,               ///< Provider or storage does not implement persistence for this location.
  CANCELLED,                 ///< Cooperative cancellation before publishing a result.
  CLOSED,                    ///< Provider or storage has closed its owning scope.
  STORAGE_FAILURE,           ///< Storage read/write failed; existing memory values remain unchanged.
  NO_MEMORY,                 ///< Allocation or representational capacity failed.
  PROVIDER_FAILURE           ///< A provider/storage callback threw an unexpected exception.
};

/** @brief Positive metadata capacities; none depends on the logical model extent. */
struct NavigationCheckpointOptions {
  std::size_t maxBytes{64 * 1024};    ///< Maximum encoded envelope bytes, including all headers and payloads.
  std::size_t maxEntries{128};        ///< Maximum owning positions restored by one checkpoint.
  std::size_t maxEntryBytes{4096};    ///< Maximum provider payload for one location.
  std::size_t maxIdentityBytes{1024}; ///< Maximum bytes of each codec name and portable resource key.
  BookmarkOptions bookmarks{};        ///< Maximum charged bytes of each decoded in-memory bookmark.
};

/** @brief Provider-issued durable identity; not a path inferred by the checkpoint library. */
struct NavigationCodecDescriptor {
  std::string_view name;     ///< Nonempty provider codec identifier, stable for its declared schema.
  std::uint32_t version{};   ///< Positive codec schema version.
  std::string_view resource; ///< Nonempty portable resource key including compatible lineage qualification.
};

/** @brief Explicit provider conversion; no generic serializer for process-local RowId/SourceStamp exists.
 * Caller keeps this object alive and serializes operations. Descriptor strings stay valid
 * throughout each outer operation. Callbacks run synchronously without a new runtime;
 * providers performing I/O must be invoked on the application's existing worker path.
 * Codec payloads contain location metadata only, excluding values, secrets, drafts,
 * Undo/Redo history and write authority. Callback-internal allocations are provider-owned
 * and are not accounted by the envelope limits. No codec or borrowed span is retained.
 */
class NavigationBookmarkCodec {
public:
  virtual ~NavigationBookmarkCodec() = default;
  /** @return Portable codec/resource qualification valid throughout the outer operation. */
  virtual NavigationCodecDescriptor descriptor() const noexcept = 0;
  /** @param entry Qualified source bookmark and location purpose.
   * @param output Bounded output capacity; no bytes outside this span may be written.
   * @param stop Cooperative cancellation.
   * @return Number of written bytes, or a typed provider failure.
   */
  virtual std::expected<std::size_t, NavigationCheckpointError>
  encode(const NavigationCheckpointEntry &entry, std::span<std::byte> output, std::stop_token stop = {}) const = 0;
  /** @param payload Provider location metadata borrowed during this call only.
   * @param kind Saved position kind. @param affinity Saved insertion/deletion side.
   * @param role Saved purpose; EDIT_ANCHOR does not authorize a write.
   * @param stop Cooperative cancellation.
   * @return Requalified in-memory bookmark for the current provider resource, or typed failure.
   */
  virtual std::expected<ModelBookmark, NavigationCheckpointError> decode(std::span<const std::byte> payload,
                                                                         PositionKind kind, PositionAffinity affinity,
                                                                         NavigationCheckpointRole role,
                                                                         std::stop_token stop = {}) const = 0;
  /** @return Live in-memory resource domain for decoded bookmarks; rechecked after each callback. */
  virtual ModelResource resource() const noexcept = 0;
};

/** @brief Immutable owning checkpoint; copies share bytes and never retain providers or UI objects.
 * A default value is empty/invalid and allocates nothing. Capture/load stage a new value;
 * failures never replace a caller's existing checkpoint or navigation state. Envelope
 * checksum detects accidental corruption, not malicious modification or authenticity.
 * Parse validates structure only; restore requires a matching provider and does not apply
 * positions to views, grant write permissions, or resolve a position against live data.
 */
class NavigationCheckpoint {
public:
  static constexpr std::uint32_t FORMAT_VERSION = 1; ///< Binary envelope schema, independent of provider version.
  NavigationCheckpoint() = default;
  /** @param entries Bounded metadata-only locations in the provider's single resource domain.
   * @param codec Explicit portable identity/position conversion.
   * @param options Maximum encoded and restored metadata.
   * @param stop Cooperative cancellation, checked before and after callbacks.
   * @return New immutable envelope or a typed validation/provider/allocation failure.
   */
  static std::expected<NavigationCheckpoint, NavigationCheckpointError>
  capture(std::span<const NavigationCheckpointEntry> entries, const NavigationBookmarkCodec &codec,
          NavigationCheckpointOptions options = {}, std::stop_token stop = {});
  /** @param bytes Untrusted encoded envelope, copied only after a full bounded preflight.
   * @param options Envelope/entry/identity capacities. @param stop Cooperative cancellation.
   * @return Owning structurally validated checkpoint or typed rejection.
   */
  static std::expected<NavigationCheckpoint, NavigationCheckpointError>
  parse(std::span<const std::byte> bytes, NavigationCheckpointOptions options = {}, std::stop_token stop = {});
  /** @param codec Matching portable resource and codec; decoded bookmarks are revalidated.
   * @param options Envelope and decoded bookmark limits. @param stop Cooperative cancellation.
   * @return Complete staged positions, without changing any navigation channel or view.
   */
  std::expected<std::vector<NavigationCheckpointEntry>, NavigationCheckpointError>
  restore(const NavigationBookmarkCodec &codec, NavigationCheckpointOptions options = {},
          std::stop_token stop = {}) const;
  /** @return Immutable encoded bytes, valid while this value or a copy survives. */
  std::span<const std::byte> bytes() const noexcept;

private:
  std::shared_ptr<const std::vector<std::byte>> bytes_;
};

/** @brief Optional synchronous storage seam; no filesystem or scheduler is installed implicitly.
 * Caller owns and serializes this adapter. Invoke load/save on an existing worker when
 * the adapter can block; these functions do not dispatch work or enforce UI affinity.
 * Read/write failures may affect external storage, but cannot alter memory checkpoints
 * or navigation state. Atomic replacement/durability is the adapter's explicit contract.
 */
class NavigationCheckpointStorage {
public:
  virtual ~NavigationCheckpointStorage() = default;
  /** @param output Maximum allowed envelope bytes. @param stop Cooperative cancellation.
   * @return Actual bytes read, or typed failure; overflow is rejected by the caller.
   */
  virtual std::expected<std::size_t, NavigationCheckpointError> read(std::span<std::byte> output,
                                                                     std::stop_token stop = {}) = 0;
  /** @param bytes Immutable complete envelope. @param stop Cooperative cancellation.
   * @return Successful write or typed storage/cancellation failure.
   */
  virtual std::expected<void, NavigationCheckpointError> write(std::span<const std::byte> bytes,
                                                               std::stop_token stop = {}) = 0;
};

/** @param storage Explicit caller-owned storage. @param options Bounded read/parse capacities.
 * @param stop Cooperative cancellation. @return New staged checkpoint; no current state is replaced.
 */
std::expected<NavigationCheckpoint, NavigationCheckpointError>
loadNavigationCheckpoint(NavigationCheckpointStorage &storage, NavigationCheckpointOptions options = {},
                         std::stop_token stop = {});
/** @param checkpoint Existing immutable value. @param storage Explicit caller-owned storage.
 * @param stop Cooperative cancellation. @return Storage result; checkpoint remains unchanged on every path.
 */
std::expected<void, NavigationCheckpointError> saveNavigationCheckpoint(const NavigationCheckpoint &checkpoint,
                                                                        NavigationCheckpointStorage &storage,
                                                                        std::stop_token stop = {});
} // namespace portals
