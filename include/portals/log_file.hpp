// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/log_file_error.hpp>
#include <portals/log_navigation.hpp>
#include <portals/window_source.hpp>

#include <filesystem>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace portals {
/** @brief Immutable UTF-8 record with its original byte position; separators are excluded. */
struct LogLine {
  std::uint64_t byteOffset{}; ///< Position of the first byte in the source file generation.
  std::pmr::string text;      ///< Owning UTF-8 payload allocated in the delivery page.

  /** @param offset Original record position. @param utf8 Complete validated UTF-8 record.
   * @param resource Owning page arena; must outlive this value.
   */
  LogLine(std::uint64_t offset, std::string_view utf8, std::pmr::memory_resource *resource);
};

/** @brief Fixed bounds independent of file length; no row index is constructed. */
struct FileLogOptions {
  std::size_t maxRecordBytes{32 * 1024};   ///< Maximum record payload; ceiling 64 KiB.
  std::size_t ioBlockBytes{16 * 1024};     ///< Single-operation block cache; ceiling 64 KiB.
  std::size_t maxSearchBytes{1024 * 1024}; ///< Maximum bytes examined by one literal-search call.
  std::size_t maxSearchPatternBytes{4096}; ///< Maximum literal pattern; ceiling 4096 bytes.
};

/** @brief Worker-observed byte metadata; byteExtent is never a row count. */
struct FileLogMetadata {
  std::uint64_t byteExtent{}; ///< Exact extent observed on an opened file handle.
  std::uint64_t generation{}; ///< Positive model-local identity; changes on rotation/truncation.
};

/** @brief One bounded search step; repeat from nextByteOffset when reachedEnd is false. */
struct FileLogSearchResult {
  std::optional<std::uint64_t> matchByteOffset; ///< First literal match within this step.
  std::uint64_t nextByteOffset{};               ///< Resume position, including overlap needed for a split match.
  std::uint64_t generation{};                   ///< Generation searched by this step.
  bool reachedEnd{};                            ///< This opened file's observed byte boundary was reached.
};

/** @brief Exact failure of one worker load; safe to copy into that job's completion. */
struct FileLogReadFailure {
  SourceError sourceError{SourceError::INVALID_ARGUMENT}; ///< Delivery protocol failure.
  std::optional<FileLogFailure> fileFailure;              ///< Native/domain cause, absent for protocol-only failures.

  /** @param error Protocol-only failure. */
  FileLogReadFailure(SourceError error) : sourceError(error) {
  }

  /** @param error Delivery classification. @param cause File operation diagnostic. */
  FileLogReadFailure(SourceError error, FileLogFailure cause) : sourceError(error), fileFailure(cause) {
  }
};

/** @brief File-backed rich model with optional bounded WindowSource delivery.
 * Creation owns the path without opening it. All I/O methods are worker-only; they do not start
 * threads. Every operation owns its native handle and bounded block cache. No row history, dense
 * row index, mapping or complete-file text is retained. Callers schedule operations on their
 * existing executor. Published pages use the WindowSource budget and remain immutable.
 */
class FileLogModel final : public WindowProvider<LogLine>, public LogBookmarkProvider {
public:
  /** @param path Owned path; no filesystem access is performed here.
   * @param options Positive bounded limits.
   * @param continuation Optional explicit logical resource for reopen; native identity and witness
   * are still validated by resolve. Empty issues an independent resource without file access.
   * @return Owning model or invalid-option/allocation failure.
   */
  static std::expected<std::shared_ptr<FileLogModel>, FileLogFailure>
  create(std::filesystem::path path, FileLogOptions options = {}, ModelResource continuation = {});
  /** @return Backward, stable-row seek and available-tail support. */
  SourceCapabilities capabilities() const noexcept override;
  /** @return Cold logical identity, independent of path and delivery sessions. */
  ModelResource resource() const noexcept override;
  /** @param command Exact source delivery qualification. @param row Worker-borrowed record.
   * @param options Owning bookmark capacity. @param stop Cooperative worker cancellation.
   * @return Native identity and bounded witness bookmark; no row index is built.
   */
  std::expected<ModelBookmark, SourceError> capture(const SourceReadCommand &command, const WindowRow<LogLine> &row,
                                                    BookmarkOptions options, std::stop_token stop = {}) const override;
  /** @param request Owning captured metadata and current delivery qualification.
   * @param stop Cooperative worker cancellation.
   * @return Qualified bounded native validation result; no path-based identity guessing.
   */
  std::expected<PositionReply, SourceError> resolve(const PositionRequest &request,
                                                    std::stop_token stop = {}) const override;
  /** @param command Bounded worker command; cursors are byte gaps in a file generation.
   * @param out Existing shared-budget page/workspace; all dynamic row payload uses its arena.
   * @param stop Cooperative cancellation checked between bounded reads and records.
   * @return Page with explicit boundary, HISTORY_UNAVAILABLE after generation change, or failure.
   * Native failures are available through lastFailure(). An unterminated final record is visible
   * at TEMPORARY_TAIL; appending to it preserves RowId and changes its value revision.
   */
  std::expected<SourceReadReply<LogLine>, SourceError> load(const SourceReadCommand &command, PageBuilder<LogLine> &out,
                                                            std::stop_token stop) override;
  /** @param command Bounded worker command. @param out Shared-budget page and workspace.
   * @param stop Cooperative cancellation.
   * @return Same result as load, preserving this invocation's exact file diagnostic on failure.
   */
  std::expected<SourceReadReply<LogLine>, FileLogReadFailure> loadFile(const SourceReadCommand &command,
                                                                       PageBuilder<LogLine> &out, std::stop_token stop);
  /** @param stop Cooperative worker cancellation. @return Observed byte extent and generation. */
  std::expected<FileLogMetadata, FileLogFailure> metadata(std::stop_token stop = {});
  /** @param byteOffset Arbitrary byte position; no row lookup or row-count scan is performed.
   * @param destination Caller-owned bounded buffer, at most ioBlockBytes bytes.
   * @param stop Cooperative worker cancellation.
   * @return Bytes read, including zero at the observed extent, or a native/argument failure.
   */
  std::expected<std::size_t, FileLogFailure> readBytes(std::uint64_t byteOffset, std::span<std::byte> destination,
                                                       std::stop_token stop = {});
  /** @param pattern Nonempty byte literal, at most maxSearchPatternBytes; not a regular expression.
   * @param byteOffset First candidate byte position.
   * @param maxScanBytes Positive work bound, at most options.maxSearchBytes and at least pattern.size().
   * @param stop Cooperative worker cancellation.
   * @return First match or explicit continuation/end; search covers file bytes, not cached rows.
   */
  std::expected<FileLogSearchResult, FileLogFailure> findLiteral(std::string_view pattern, std::uint64_t byteOffset,
                                                                 std::size_t maxScanBytes, std::stop_token stop = {});
  /** @return Last file-specific load failure, not per-job provenance under concurrent operations.
   * Copied under a short mutex never held during I/O; use loadFile for the exact causal failure.
   */
  std::optional<FileLogFailure> lastFailure() const noexcept;
  /** @return Immutable explicit file bounds. */
  FileLogOptions options() const noexcept;

private:
  struct Observation {
    std::uint64_t identityHigh{};
    std::uint64_t identityLow{};
    std::uint64_t byteExtent{};
    std::uint64_t modified{};
  };

  FileLogModel(std::filesystem::path path, FileLogOptions options, ModelResource resource, SourceStamp instance);
  std::expected<std::uint64_t, FileLogFailure> beginOperation(std::stop_token stop) const;
  std::expected<std::uint64_t, FileLogFailure> observe(Observation observation, std::uint64_t operation,
                                                       std::stop_token stop) const;
  void recordFailure(FileLogFailure failure) noexcept;
  std::filesystem::path path_;
  FileLogOptions options_;
  ModelResource resource_;
  SourceStamp instance_;
  mutable std::mutex mutex_;
  mutable Observation observed_;
  mutable std::uint64_t generation_{};
  mutable std::uint64_t operationSequence_{};
  mutable std::uint64_t observedOperation_{};
  std::optional<FileLogFailure> failure_;
};
} // namespace portals
