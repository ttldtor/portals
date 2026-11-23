// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/line_edit.hpp>
#include <portals/model_position.hpp>

#include <functional>
#include <string>

namespace portals {
/** @brief Owning field identity; local shorthand is deliberately not persistent. */
class EditFieldKey {
public:
  EditFieldKey() = default;

  /** @param value Nonempty stable key interpreted by the endpoint's schema.
   * @return Owning key or checked validation/allocation failure.
   */
  static std::expected<EditFieldKey, TreeError> create(std::string_view value);

  /** @return Schema key borrowed while this value or a copy survives. */
  std::string_view name() const noexcept;

  /** @return Whether the key was explicitly supplied for persistent adapter use. */
  bool persistent() const noexcept;

  /** @param other Complete key to compare. @return Equality including key domain. */
  bool operator==(const EditFieldKey &other) const noexcept;

  /** @param identity Positive session-local shorthand identity.
   * @return Process-local key; never serialize this as a persistent field identifier.
   */
  static EditFieldKey local(std::uint64_t identity) noexcept;

private:
  std::shared_ptr<const std::string> name_;
  std::uint64_t local_{};
};

/** @brief Frozen write destination; ordinal hints and nearest positions are excluded. */
struct EditTarget {
  ModelResource resource;      ///< Source identity and compatible lineage.
  SourceStamp session;         ///< Unique edit request namespace, independent of source delivery.
  RowId row;                   ///< Complete stable record identity.
  EditFieldKey field;          ///< Owning schema key or explicitly local shorthand.
  std::uint64_t baseVersion{}; ///< Expected version checked together with the backend write.
  std::uint64_t sequence{};    ///< Positive request generation within this edit session.
  SourceStamp sourceSession;   ///< UI source incarnation; the endpoint also guards its own backend incarnation.

  /** @param other Qualified target to compare. @return Exact equality. */
  bool operator==(const EditTarget &other) const noexcept = default;
};

/** @brief Submission and acceptance are separate protocol states. */
enum class EditStatus {
  CLEAN,          ///< Current accepted value, without a draft.
  DIRTY,          ///< Owning unpublished input.
  PENDING,        ///< Admitted work; no backend acceptance has been reported.
  ACCEPTED,       ///< Endpoint applied this exact qualified write.
  INVALID,        ///< Validation rejected the retained draft.
  CONFLICT,       ///< Expected target version is no longer current.
  TARGET_MISSING, ///< Original record is absent; retained draft is orphaned.
  READ_ONLY,      ///< Source permits reads but does not expose writes.
  CLOSED,         ///< Original endpoint/session no longer admits work.
  REJECTED        ///< Backend rejected the request without applying it.
};

/** @brief Owning acknowledgement qualified independently of current navigation. */
struct EditReceipt {
  EditTarget target;                      ///< Original target and expected version.
  EditStatus status{EditStatus::PENDING}; ///< Admission is not acceptance.
  std::uint64_t acceptedVersion{};        ///< Positive new record version only for acceptance.
  std::optional<TreeError> error;         ///< Optional backend/delivery failure.
};

/** @brief Exact qualification; this does not prove a backend applied its precondition.
 * @param target Original owning request qualification. @param receipt Delivered backend acknowledgement.
 * @return True only when every original target component matches.
 */
bool editReceiptMatches(const EditTarget &target, const EditReceipt &receipt) noexcept;

/** @brief Explicit source guarantees; unsupported atomicity is never inferred. */
struct EditCapabilities {
  bool writable{};      ///< False preserves a readable field/navigation interface.
  bool conditional{};   ///< Target/version checked atomically with applying the write.
  bool ordered{};       ///< Backend rejects stale sequences for the same target.
  bool atomicFields{};  ///< Backend capability only; this session still submits fields separately.
  bool recordVersion{}; ///< True when fields share one record version; false means field-local versions.
};

/** @brief Bounded retained editing, independent of collection extent. */
struct EditOptions {
  EditMode mode{EditMode::IMMEDIATE}; ///< Existing editor publication policy.
  std::size_t maxRetainedFields{128}; ///< Dirty fields are never silently evicted.
  std::size_t maxRetainedBytes{
    4 * 1024 *
    1024}; ///< Logical owning payload admission budget; excludes allocator bookkeeping and temporary staging.
};

/** @brief One bounded immutable field-value read, never a whole-provider materialization.
 * @tparam T Owning copyable field value.
 */
template <class T> struct EditRecord {
  std::shared_ptr<const T> value; ///< Independent owning field snapshot.
  std::uint64_t version{};        ///< Positive monotonic record or field version, according to capabilities().
};

/** @brief Owning bounded write request usable on the existing worker path.
 * @tparam T Owning immutable field value; the endpoint applies only target.field.
 */
template <class T> struct EditRequest {
  EditTarget target;              ///< Exact destination and precondition.
  std::shared_ptr<const T> value; ///< Owning candidate; no UI/widget references.
};

/** @brief Source capability adapter; calls must return promptly without UI I/O.
 * External work is submitted through the application's existing scheduler/worker path.
 * submit() returns PENDING for admission, or a qualified final result; applyReply() on
 * the edit session handles later delivery. Unconditional endpoints must serialize
 * backend acceptance for each target; rejecting a second in-flight request is valid.
 * @tparam T Owning field value type selected from the navigation record.
 */
template <class T> class EditEndpoint {
public:
  virtual ~EditEndpoint() = default;

  /** @return Declared backend guarantees. */
  virtual EditCapabilities capabilities() const noexcept = 0;

  /** @param resource Exact logical source. @param row Complete record key.
   * @return One owning field value/version, pending WOULD_BLOCK, or checked failure.
   */
  virtual std::expected<EditRecord<T>, TreeError> read(ModelResource resource, RowId row) = 0;

  /** @param request Owning candidate/precondition. @return Admission or exact final receipt.
   * For conditional endpoints ACCEPTED means the precondition and write happened together.
   * The adapter must check its bound backend incarnation when applying a request, including
   * delayed work. sourceSession qualifies the UI source and is not a server generation.
   * A checked error means no backend admission; uncertain outcomes must remain PENDING.
   * Unconditional endpoints expose only their declared serialized/exclusive-writer guarantee.
   */
  virtual std::expected<EditReceipt, TreeError> submit(EditRequest<T> request) = 0;

  /** @param value Owning field candidate whose complete retained payload is measured.
   * @return Conservative byte charge including dynamic members, or unsupported capability.
   * sizeof(T) alone is insufficient for strings, vectors or nested dynamic storage.
   */
  virtual std::expected<std::size_t, TreeError> ownedBytes(const T &value) const = 0;
};

/** @brief Explicit per-field result of a non-atomic submit. */
struct EditCommitResult {
  std::vector<EditReceipt> fields; ///< Each applied/rejected/pending field remains visible.
  bool atomic{};                   ///< True only when one endpoint transaction provided it.
};
} // namespace portals
