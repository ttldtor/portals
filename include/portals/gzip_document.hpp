// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/document_source.hpp>
#include <type_traits>

namespace portals {
/** @brief Gzip cause carried in DocumentError::native; implicitly convertible to std::error_code. */
enum class GzipError {
  INVALID_HEADER = 1, ///< Input does not start with a supported gzip header.
  CORRUPT_DATA,       ///< Invalid compressed data.
  CHECKSUM_MISMATCH,  ///< A member or header checksum did not match.
  SIZE_MISMATCH,      ///< A member's modulo-2^32 size check did not match.
  TRUNCATED,          ///< Physical EOF arrived before the complete member trailer.
  TRAILING_DATA,      ///< Bytes after a complete member are not another gzip member.
  HEADER_LIMIT,       ///< The configured compressed header limit was exceeded.
  INFLATE_LIMIT       ///< The configured aggregate live inflater allocation limit was exceeded.
};

/** @param error Typed gzip content or resource-limit cause.
 * @return Stable std::error_code retaining the gzip category and value.
 */
// NOLINTNEXTLINE(readability-identifier-naming): Required std::error_code ADL customization spelling.
std::error_code make_error_code(GzipError error) noexcept;

/** @brief Fixed RAM and replay policy; limits do not restrict logical document size. */
struct GzipDocumentOptions {
  static constexpr std::size_t MAX_CHECKPOINTS = 64; ///< Supported bound for nonblocking optional-cache retirement.
  std::size_t compressedBlockBytes{64 * 1024};       ///< Positive input quantum, at most64KiB.
  std::size_t inflateOutputBytes{32 * 1024}; ///< Positive output quantum; cancellation is checked between calls.
  std::uint64_t checkpointStrideBytes{4 * 1024 * 1024}; ///< Positive logical spacing for optional snapshots.
  std::size_t maxCheckpoints{8};           ///< Live restart snapshots, from zero (disabled) through MAX_CHECKPOINTS.
  std::size_t replayBytes{64 * 1024};      ///< Bounded logical byte replay ring; zero disables it.
  std::size_t maxInflateBytes{512 * 1024}; ///< Aggregate zlib allocation cap, including active and copied states.
  std::size_t maxHeaderBytes{1024 * 1024}; ///< Positive compressed-byte cap for each member header.
};

/** @brief Optional gzip-only source with bounded RAM checkpoints and cancellable replay.
 * Logical offsets are decompressed byte offsets. Exact logical extent is unknown
 * until every concatenated member and physical EOF have been validated. Gzip
 * headers, CRC/ISIZE, truncation and trailing garbage are checked strictly.
 * Previously returned bytes can precede a later member-integrity error; they do
 * not prove whole-archive validity. A failing read publishes none of that call's
 * partial output, and never reports clean EOF.
 *
 * One provider serializes its operations. Do not concurrently access the same
 * compressed provider through other wrappers; share this gzip provider instead.
 * The first observed compressed revision is frozen. A change requires a new
 * adapter; live compressed append/follow mode and disk caching are not provided.
 * Cold seeks can replay from the beginning and occupy a worker for a long time.
 * Cancellation is checked between bounded inflate/I/O quanta; a mutex wait or
 * an underlying blocking read cannot be forcibly interrupted by this adapter.
 */
class GzipDocumentProvider final : public ByteDocumentProvider {
public:
  /** @param compressed Seekable compressed provider retained by the adapter.
   * @param budget The same root subsequently used by ByteDocumentSource/views.
   * @param options Bounded workspace, checkpoints, replay and header limits.
   * @return Cold adapter without metadata/read/file I/O, or typed failure.
   * All adapter-owned buffers and zlib allocations are charged to budget.
   */
  static std::expected<std::shared_ptr<GzipDocumentProvider>, DocumentError>
  create(std::shared_ptr<ByteDocumentProvider> compressed, std::shared_ptr<SourceBudget> budget,
         GzipDocumentOptions options = {});

  ~GzipDocumentProvider() override; ///< Release all states before their allocator/root owners.
  ByteDocumentCapabilities capabilities() const noexcept override; ///< @return Seekable, frozen logical revision.
  /** @param root Root whose non-evictable storage is observed.
   * @return Fixed provider bytes on that root; idle inflater/checkpoints can be reclaimed.
   */
  std::size_t pinnedBudgetBytes(const SourceBudget &root) const noexcept override;
  /** @return Whether idle inflater/checkpoint storage was reclaimed without blocking or I/O.
   * Busy operations service a deferred request at completion; replay and document data remain valid.
   */
  bool tryReleaseOptionalStorage() noexcept override;
  /** @param stop Cooperative cancellation; underlying metadata obeys its own cancellation contract.
   * @return Frozen revision and only a proven logical extent, or a typed source failure.
   */
  std::expected<ByteDocumentMetadata, DocumentError> metadata(std::stop_token stop) override;
  /** @param stop Cooperative cancellation between bounded inflate/input quanta.
   * @return Frozen revision and validated logical extent after all members and physical EOF, or a typed failure.
   * May replay the compressed source to EOF; decoded output is discarded in bounded storage without text layout.
   */
  std::expected<ByteDocumentMetadata, DocumentError> discoverExtent(std::stop_token stop) override;
  /** @param command Frozen revision and requested decompressed byte offset.
   * @param destination Bounded output storage borrowed for this call only.
   * @param stop Cooperative cancellation between bounded inflate/input quanta.
   * @return Initialized prefix and confirmed archive EOF, or a typed failure without a published page.
   */
  std::expected<ByteReadResult, DocumentError> read(const ByteReadCommand &command, std::span<std::byte> destination,
                                                    std::stop_token stop) override;

private:
  struct State;
  explicit GzipDocumentProvider(std::unique_ptr<State> state);
  std::unique_ptr<State> state_;
};
} // namespace portals

/// @cond INTERNAL
template <> struct std::is_error_code_enum<portals::GzipError> : std::true_type {};

/// @endcond
