// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <filesystem>
#include <memory>
#include <portals/log_file_error.hpp>
#include <portals/window_source.hpp>
#include <string>
#include <system_error>

namespace portals {
/** @brief Failure with an optional native provider cause, preserved by source operations. */
struct DocumentError {
  SourceError code{SourceError::INVALID_ARGUMENT}; ///< Protocol/admission failure.
  std::error_code native;                          ///< Native operation cause, empty for protocol failures.
  std::optional<FileLogFailure> fileFailure;       ///< Exact file semantic/operation cause when applicable.
  std::optional<SourceBudgetUsage>
    budgetDemand{}; ///< Exact failed reservation on this source root; absent for provider waiting.
  std::optional<SourceBudgetUsage> budgetPeak{}; ///< Minimum transient retry overlap on the same root; excludes pinned
                                                 ///< provider storage and is extended by enclosing source operations.
};

/** @brief Exact document namespace and provider revision. */
struct DocumentRevision {
  SourceStamp source;                                                 ///< Immutable source namespace.
  std::uint64_t dataRevision{};                                       ///< Nonzero provider revision.
  bool operator==(const DocumentRevision &) const noexcept = default; ///< @return Compare both qualifiers.
};

/** @brief Capabilities captured without I/O during cold source creation. */
struct ByteDocumentCapabilities {
  bool seekable{true};  ///< Arbitrary byte offsets can be requested.
  bool immutable{true}; ///< Provider revision never changes.
  bool readSessions{};  ///< Worker-owned provisional read sessions are supported.
};

/** @brief Worker-observed provider metadata. */
struct ByteDocumentMetadata {
  std::uint64_t dataRevision{};            ///< Nonzero exact revision.
  std::optional<std::uint64_t> byteExtent; ///< Exact extent, or unknown without a forced scan.
};

/** @brief Metadata qualified by the source namespace. */
struct DocumentMetadata {
  DocumentRevision revision;               ///< Exact namespace/provider revision.
  std::optional<std::uint64_t> byteExtent; ///< Exact byte extent when known.
  ByteDocumentCapabilities capabilities;   ///< Captured immutable capabilities.
};

/** @brief Exact provider request; destination size bounds the byte window. */
struct ByteReadCommand {
  std::uint64_t dataRevision{}; ///< Required revision; never silently substituted.
  std::uint64_t offset{};       ///< First requested byte, including offsets above4GiB.
};

/** @brief Short reads alone never imply EOF. */
struct ByteReadResult {
  std::size_t bytesRead{}; ///< Initialized prefix of the borrowed destination.
  bool finalEnd{};         ///< Definitive EOF immediately after that prefix.
};

/** @brief One worker-owned provisional read transaction; destroy it on its creating worker.
 * A successful seal qualifies all preceding pages. Until then callers must not publish
 * them. Implementations retain bounded metadata/handles only, never destination aliases.
 */
class ByteDocumentReadSession {
public:
  virtual ~ByteDocumentReadSession() = default; ///< Release worker-owned backend storage.
  /** @return Immutable metadata captured by opening this session, without further I/O. */
  virtual ByteDocumentMetadata metadata() const noexcept = 0;
  /** @param command Captured revision and bounded original byte offset.
   * @param destination Writable bytes borrowed only for this call.
   * @param stop Cooperative cancellation.
   * @return Provisional initialized bytes/EOF, or an exact typed failure.
   */
  virtual std::expected<ByteReadResult, DocumentError> read(const ByteReadCommand &command,
                                                            std::span<std::byte> destination, std::stop_token stop) = 0;
  /** @param stop Cooperative cancellation of final path/revision validation.
   * @return Success qualifying preceding reads, or failure that forbids publishing them.
   */
  virtual std::expected<void, DocumentError> seal(std::stop_token stop) = 0;
};

/** @brief Read-only worker provider; callers serialize operations using the existing executor.
 * Provider/domain storage is external to page accounting, as with WindowProvider.
 * No destination or mutable alias may escape read().
 */
class ByteDocumentProvider {
public:
  virtual ~ByteDocumentProvider() = default; ///< Safe final destruction after worker release.
  /** @return Capabilities without allocation, opening files or other I/O. */
  virtual ByteDocumentCapabilities capabilities() const noexcept = 0;

  /** @param root Credit root whose mandatory retained bytes are observed.
   * @return Thread-safe allocation-free snapshot of provider-owned pinned bytes on that root.
   * Excludes optional cache and source/view credits counted by their owners. No I/O or blocking lock is allowed.
   */
  virtual std::size_t pinnedBudgetBytes(const SourceBudget &root) const noexcept {
    (void)root;
    return 0;
  }

  /** @return Whether optional storage was released without changing document data or navigation state.
   * Thread-safe, bounded and nonblocking; no I/O, decoding, allocation or user callback is allowed.
   * A busy implementation may defer the request to its current worker before that worker completes.
   */
  virtual bool tryReleaseOptionalStorage() noexcept {
    return false;
  }

  /** @param stop Cooperative cancellation.
   * @return Current exact revision/optional extent, or typed failure.
   */
  virtual std::expected<ByteDocumentMetadata, DocumentError> metadata(std::stop_token stop) = 0;

  /** @param stop Cooperative cancellation of worker-only extent discovery.
   * @return Current revision and optional extent, or typed failure.
   * The default only observes metadata. Adapters may discover their exact end using
   * bounded storage without formatting document content; an unknown extent remains valid.
   * The source serializes this operation with metadata/read and discards late cancellation.
   */
  virtual std::expected<ByteDocumentMetadata, DocumentError> discoverExtent(std::stop_token stop) {
    return metadata(stop);
  }

  /** @param command Exact revision/offset.
   * @param destination Writable bounded storage borrowed for this call only.
   * @param stop Cooperative cancellation, also checked after returning by the source.
   * @return Initialized byte count and explicit EOF, or typed failure.
   */
  virtual std::expected<ByteReadResult, DocumentError> read(const ByteReadCommand &command,
                                                            std::span<std::byte> destination, std::stop_token stop) = 0;

  /** @param stop Cooperative worker cancellation.
   * @return Optional bounded read session, or UNSUPPORTED without opening the source.
   * Appended capability; ordinary metadata/read remain unchanged for other providers.
   */
  virtual std::expected<std::unique_ptr<ByteDocumentReadSession>, DocumentError> openReadSession(std::stop_token stop) {
    (void)stop;
    return std::unexpected(DocumentError{SourceError::UNSUPPORTED, {}, {}});
  }
};

/** @brief Adapter retaining existing immutable shared text without copying whole input. */
class MemoryDocumentProvider final : public ByteDocumentProvider {
public:
  /** @param text Existing immutable shared storage; null is rejected on use.
   * @param revision Nonzero exact revision, default1.
   */
  explicit MemoryDocumentProvider(std::shared_ptr<const std::string> text, std::uint64_t revision = 1);
  ByteDocumentCapabilities capabilities() const noexcept override; ///< @return Immutable and seekable.
  std::expected<ByteDocumentMetadata, DocumentError>
  metadata(std::stop_token stop) override; ///< @param stop Cooperative cancellation. @return Observe extent.
  std::expected<ByteReadResult, DocumentError> read(const ByteReadCommand &command, std::span<std::byte> destination,
                                                    std::stop_token stop)
    override; ///< @param command Exact revision and byte offset. @param destination Borrowed writable bytes. @param
              ///< stop Cooperative cancellation. @return Copy only one window.
private:
  std::shared_ptr<const std::string> text_;
  std::uint64_t revision_;
};

class FileLogModel;
struct FileLogMetadata;

/** @brief Cold file adapter reusing FileLogModel raw worker I/O without record limits.
 * Exact document revision advances on every observed extent or file generation change.
 * Calls are serialized by the caller; native metadata/read handles are owned by each operation.
 */
class FileDocumentProvider final : public ByteDocumentProvider {
public:
  /** @param path Owned source path; creation never opens it.
   * @param ioBlockBytes Positive maximum raw read, at most64KiB; default64KiB.
   * @return Cold adapter or typed validation/allocation failure.
   */
  static std::expected<std::shared_ptr<FileDocumentProvider>, DocumentError>
  create(std::filesystem::path path, std::size_t ioBlockBytes = 64 * 1024);
  /** @param model Existing cold file model; no reopen or replacement of its identity.
   * @return Adapter sharing the model, or typed validation/allocation failure.
   */
  static std::expected<std::shared_ptr<FileDocumentProvider>, DocumentError>
  create(std::shared_ptr<FileLogModel> model);
  ByteDocumentCapabilities capabilities() const noexcept override; ///< @return Seekable, revision may change.
  std::expected<ByteDocumentMetadata, DocumentError>
  metadata(std::stop_token stop) override; ///< @param stop Cooperative cancellation. @return Observe exact snapshot.
  std::expected<ByteReadResult, DocumentError> read(const ByteReadCommand &command, std::span<std::byte> destination,
                                                    std::stop_token stop)
    override; ///< @param command Exact revision and byte offset. @param destination Borrowed writable bytes. @param
              ///< stop Cooperative cancellation. @return Validate before/after raw read.
  /** @param stop Cooperative worker cancellation.
   * @return One bounded native-handle transaction with deferred fresh-path sealing.
   */
  std::expected<std::unique_ptr<ByteDocumentReadSession>, DocumentError> openReadSession(std::stop_token stop) override;

private:
  explicit FileDocumentProvider(std::shared_ptr<FileLogModel> model);
  std::expected<ByteDocumentMetadata, DocumentError> observeMetadata(FileLogMetadata observed);
  std::shared_ptr<FileLogModel> model_;
  std::uint64_t fileGeneration_{};
  std::uint64_t byteExtent_{};
  std::uint64_t revision_{};
};

/** @brief Page bounds limit windows, never logical line length. */
struct DocumentSourceOptions {
  std::size_t maxPageBytes{64 * 1024}; ///< Maximum byte payload per read.
  std::size_t envelopeBytes{4096};     ///< Source/work metadata allowance; minimum4096.
};

/** @brief Owning request dispatched through the existing executor. */
struct DocumentReadRequest {
  DocumentRevision revision;  ///< Required exact namespace/revision.
  std::uint64_t offset{};     ///< First requested byte.
  std::size_t maxBytes{};     ///< Positive bounded payload size.
  std::uint64_t generation{}; ///< Caller navigation generation echoed without interpretation.
};

/// @cond INTERNAL
namespace detail {
struct DocumentPageData;
}

/// @endcond

/** @brief Immutable window sharing page credit until its final owner releases it. */
class DocumentBytePage {
public:
  DocumentBytePage() = default;                      ///< Empty allocation-free pin.
  std::span<const std::byte> bytes() const noexcept; ///< @return Initialized bytes valid for this pin's lifetime.
  DocumentRevision revision() const noexcept;        ///< @return Exact revision, or zero for an empty pin.
  std::uint64_t offset() const noexcept;             ///< @return First byte offset.
  std::uint64_t generation() const noexcept;         ///< @return Caller generation from the request.
  bool finalEnd() const noexcept;                    ///< @return Definitive EOF after the returned prefix.
private:
  friend class ByteDocumentSource;
  explicit DocumentBytePage(std::shared_ptr<const detail::DocumentPageData> data);
  std::shared_ptr<const detail::DocumentPageData> data_;
};

/** @brief Cold source; inspect/read reject its creating (UI) thread.
 * Dispatch synchronous operations through the existing executor; no scheduler is created.
 * One shared source serializes provider operations on workers. Separate sources sharing a provider
 * require provider-level serialization. Pages can be retained by several views.
 */
class ByteDocumentSource {
public:
  /** @param provider Shared worker provider; creation invokes capabilities only.
   * @param budget Existing root shared by views/replacement sources.
   * @param options Bounded payload and metadata capacities.
   * @return Cold source without open/metadata/read I/O, or typed failure.
   */
  static std::expected<std::shared_ptr<ByteDocumentSource>, DocumentError>
  create(std::shared_ptr<ByteDocumentProvider> provider, std::shared_ptr<SourceBudget> budget,
         DocumentSourceOptions options = {});

  /** @return Existing shared accounting root; no I/O or new allocation is performed. */
  std::shared_ptr<SourceBudget> budget() const noexcept {
    return budget_;
  }

  SourceStamp stamp() const noexcept;                     ///< @return Immutable namespace, available without I/O.
  ByteDocumentCapabilities capabilities() const noexcept; ///< @return Captured capabilities without I/O.
  std::size_t maxPageBytes() const noexcept;              ///< @return Immutable maximum read payload.

  /** @return Existing positive source/work metadata allowance; available on UI without I/O. */
  std::size_t envelopeBytes() const noexcept {
    return options_.envelopeBytes;
  }

  /** @return Nonblocking provider-owned mandatory bytes on this source's root; excludes source/page/view storage. */
  std::size_t pinnedBudgetBytes() const noexcept;
  /** @return Whether bounded optional provider storage was reclaimed without I/O or blocking. */
  bool tryReleaseOptionalStorage() noexcept;

  /** @param stop Cooperative cancellation.
   * @return Qualified metadata, or worker-affinity/provider/cancellation failure.
   */
  std::expected<DocumentMetadata, DocumentError> inspect(std::stop_token stop = {});
  /** @param stop Cooperative cancellation, including provider extent discovery.
   * @return Qualified metadata after optional worker-only end discovery, or typed failure.
   * Providers without this capability retain the ordinary inspect behavior.
   */
  std::expected<DocumentMetadata, DocumentError> inspectEnd(std::stop_token stop = {});

  /** @param request Exact bounded window and caller generation.
   * @param stop Cooperative cancellation; late cancellation discards the page.
   * @return Owning page or typed failure; no publication/callback occurs here.
   */
  std::expected<DocumentBytePage, DocumentError> read(DocumentReadRequest request, std::stop_token stop = {});

private:
  friend class DocumentReadBatch;
  std::expected<DocumentBytePage, DocumentError> readImpl(DocumentReadRequest request, std::stop_token stop,
                                                          ByteDocumentReadSession *session);
  std::expected<DocumentMetadata, DocumentError> inspectOnWorker(std::stop_token stop, bool discoverEnd);
  ByteDocumentSource(std::shared_ptr<ByteDocumentProvider> provider, std::shared_ptr<SourceBudget> budget,
                     SourceCredit credit, DocumentSourceOptions options, SourceStamp stamp);
  std::shared_ptr<ByteDocumentProvider> provider_;
  std::shared_ptr<SourceBudget> budget_;
  SourceCredit credit_;
  DocumentSourceOptions options_;
  SourceStamp stamp_;
  ByteDocumentCapabilities capabilities_;
  std::thread::id ownerThread_;
  std::mutex providerMutex_;
};

/** @brief Explicit source-owning provisional batch, confined to its creating worker.
 * Read pages can outlive the batch, but must not be published before successful seal.
 * Destruction closes the backend before returning metadata credit; it performs no I/O
 * validation and cannot turn an abandoned batch into a committed result.
 */
class DocumentReadBatch {
public:
  static constexpr std::size_t READ_BATCH_METADATA_BYTES = 4096; ///< Held only by an actual supported batch.
  /** @param source Strong existing source; its creating UI thread is rejected.
   * @param stop Cooperative cancellation, also checked after provider callbacks.
   * @return Worker-owned batch with captured qualified metadata, or typed failure.
   */
  static std::expected<DocumentReadBatch, DocumentError> create(std::shared_ptr<ByteDocumentSource> source,
                                                                std::stop_token stop = {});
  /** @param other Worker-local ownership transferred without allocation. */
  DocumentReadBatch(DocumentReadBatch &&other) noexcept = default;
  /** @param other Replacement worker-local batch. @return This owner after safely releasing old storage. */
  DocumentReadBatch &operator=(DocumentReadBatch other) noexcept;
  DocumentReadBatch(const DocumentReadBatch &) = delete;
  ~DocumentReadBatch() = default; ///< Destroy on the creating worker; does not terminate on misuse.

  /** @return Borrowed captured metadata, valid until this batch moves or dies; no provider I/O. */
  const DocumentMetadata &metadata() const noexcept {
    return metadata_;
  }

  /** @param request Exact captured revision, source namespace and bounded byte window.
   * @param stop Cooperative cancellation; late cancellation discards the provisional page.
   * @return Owning provisional page, or the first batch failure. Wrong-thread calls never mutate the batch.
   */
  std::expected<DocumentBytePage, DocumentError> read(DocumentReadRequest request, std::stop_token stop = {});
  /** @param stop Cooperative cancellation of final validation.
   * @return Success qualifying preceding pages, or the first failure; a successful batch is single-use.
   */
  std::expected<void, DocumentError> seal(std::stop_token stop = {});

private:
  DocumentReadBatch(SourceCredit credit, std::shared_ptr<ByteDocumentSource> source,
                    std::unique_ptr<ByteDocumentReadSession> session, DocumentMetadata metadata);
  SourceCredit credit_; // Backend storage is destroyed before returning this credit.
  std::shared_ptr<ByteDocumentSource> source_;
  std::unique_ptr<ByteDocumentReadSession> session_;
  DocumentMetadata metadata_;
  std::thread::id workerThread_;
  std::optional<DocumentError> failure_;
  bool sealed_{};
};

/** @brief Policy qualifying every index checkpoint; UTF16 requires a decoder adapter. */
enum class DocumentLinePolicy {
  RAW_BYTES, ///< CR/LF/CRLF byte separators without decoding.
  UTF8,      ///< UTF8 byte separators; offsets remain raw byte positions.
  UTF16_LE,  ///< Unsupported by this bytewise index.
  UTF16_BE   ///< Unsupported by this bytewise index.
};

/** @brief Precision of a requested line start. */
enum class DocumentLocationState {
  EXACT,   ///< Requested line start proven.
  PENDING, ///< More bounded worker scans required.
  UNKNOWN  ///< EOF proved that the line does not exist.
};

/** @brief Exact sparse anchor, never an approximate requested location. */
struct DocumentCheckpoint {
  std::uint64_t line{};   ///< Zero-based logical line.
  std::uint64_t offset{}; ///< Exact line-start byte.
};

/** @brief Bounded lookup result. */
struct DocumentLineLocation {
  DocumentLocationState state{DocumentLocationState::PENDING}; ///< Precision/availability.
  DocumentCheckpoint checkpoint; ///< Requested start if EXACT; preceding anchor otherwise.
};

/** @brief Prefix knowledge; exact total exists only after EOF. */
struct DocumentIndexProgress {
  std::uint64_t scannedBytes{};                ///< Examined contiguous prefix.
  std::uint64_t completedLines{};              ///< Separators proven in that prefix.
  std::optional<std::uint64_t> exactLineCount; ///< Includes trailing empty line, only at EOF.
  std::size_t checkpoints{};                   ///< Bounded resident anchor count.
};

/** @brief Fixed RAM and per-call worker bounds. */
struct DocumentIndexOptions {
  std::size_t maxCheckpoints{256};                     ///< At least2 anchors, reserved once.
  std::uint64_t checkpointBytes{1024 * 1024};          ///< Initial byte distance between stored line starts.
  std::size_t scanBytes{64 * 1024};                    ///< Maximum read payload per advance/lookup call.
  DocumentLinePolicy policy{DocumentLinePolicy::UTF8}; ///< Fixed newline policy identity.
};

/** @brief Worker-owned sparse index with adaptive thinning and resumable bounded scans.
 * Serialize mutable operations using the existing executor. Lines are never materialized.
 * Empty documents have one empty line; a final separator introduces a trailing empty line.
 */
class SparseDocumentIndex {
public:
  /** @param budget Same root as document/views, including this reservation.
   * @param revision Exact namespace/provider revision for every scan.
   * @param options Checkpoint quota, byte interval and maximum work.
   * @return Reserved index or typed validation/admission/allocation failure.
   */
  static std::expected<std::unique_ptr<SparseDocumentIndex>, DocumentError>
  create(std::shared_ptr<SourceBudget> budget, DocumentRevision revision, DocumentIndexOptions options = {});
  ~SparseDocumentIndex();                          ///< Release fixed arena and its credit.
  DocumentRevision revision() const noexcept;      ///< @return Index namespace/revision.
  DocumentLinePolicy policy() const noexcept;      ///< @return Policy qualifying checkpoints.
  DocumentIndexProgress progress() const noexcept; ///< @return Prefix knowledge without I/O.
  /** @param offset Byte anchor.
   * @return Nearest retained checkpoint not after offset, usable as a bounded scan origin.
   */
  DocumentCheckpoint checkpointBeforeByte(std::uint64_t offset) const noexcept;
  /** @param source Matching source dispatched off its creating thread.
   * @param stop Cooperative cancellation.
   * @return Prefix after at most scanBytes read, or typed failure.
   */
  std::expected<DocumentIndexProgress, DocumentError> advance(ByteDocumentSource &source, std::stop_token stop = {});
  /** @param source Matching seekable source on an existing worker.
   * @param line Zero-based requested line; repeated calls resume pending work.
   * @param stop Cooperative cancellation.
   * @return Exact start, PENDING after bounded work, or UNKNOWN after proven EOF.
   */
  std::expected<DocumentLineLocation, DocumentError> locateLine(ByteDocumentSource &source, std::uint64_t line,
                                                                std::stop_token stop = {});

private:
  struct Cursor {
    std::uint64_t offset{};
    std::uint64_t line{};
    std::uint64_t lineStart{};
    bool pendingCR{};
    bool ended{};
  };

  SparseDocumentIndex(std::shared_ptr<SourceArena> arena, DocumentRevision revision, DocumentIndexOptions options);
  std::expected<void, DocumentError> validate(ByteDocumentSource &source, std::stop_token stop);
  std::expected<void, DocumentError> scan(ByteDocumentSource &source, Cursor &cursor,
                                          std::optional<std::uint64_t> target, bool extendPrefix, std::stop_token stop);
  std::expected<void, DocumentError> nextLine(Cursor &cursor, std::uint64_t start, bool extendPrefix);
  void addCheckpoint(DocumentCheckpoint checkpoint) noexcept;
  DocumentCheckpoint checkpointBeforeLine(std::uint64_t line) const noexcept;
  std::shared_ptr<SourceArena> arena_;
  DocumentCheckpoint *checkpoints_{};
  std::size_t checkpointCount_{1};
  DocumentRevision revision_;
  DocumentIndexOptions options_;
  std::uint64_t interval_;
  Cursor prefix_;
  Cursor lookup_;
  std::optional<std::uint64_t> lookupTarget_;
};
} // namespace portals
