// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <filesystem>
#include <portals/document_source.hpp>

namespace portals {
/** @brief Half-open original logical-byte range; an omitted end requests this snapshot through EOF. */
struct DocumentByteRange {
  std::uint64_t begin{};            ///< Inclusive original byte offset, without decoding or normalization.
  std::optional<std::uint64_t> end; ///< Exclusive original byte offset; absence means the observed end.
};

/** @brief One synchronous sink admission, including partial writes followed by an error. */
struct DocumentWriteReply {
  std::size_t written{};              ///< Exactly accepted bytes from this call, at most the supplied span length.
  std::optional<DocumentError> error; ///< Failure after the reported prefix, or empty to continue.
};

/** @brief Worker sink; storage/output policy belongs to the host implementation.
 * write borrows bytes only for its call and must report a partial prefix even on error.
 * Implementations must not retain the borrowed span or invoke UI code. A blocking sink
 * retains its own cancellation latency; the exporter cannot forcibly interrupt it.
 * Callers serialize an entire export per sink; concurrent write/finish is unsupported.
 */
class DocumentExportSink {
public:
  virtual ~DocumentExportSink() = default; ///< Release owned output handles; no scene is retained.
  /** @param bytes Bounded borrowed logical source bytes.
   * @param stop Cooperative cancellation; account already accepted bytes before returning.
   * @return Exact accepted prefix and optional failure; zero without an error is invalid for nonempty input.
   * An empty successful write begins a validated empty export without supplying payload.
   */
  virtual DocumentWriteReply write(std::span<const std::byte> bytes, std::stop_token stop) = 0;

  /** @param stop Cooperative cancellation; resource release must still complete.
   * @return Output finalization outcome; called exactly once after success, validation/source failure or cancellation.
   * Cleanup must not create a new output. If an earlier failure exists, its cause is preserved
   * and a secondary finalization failure is discarded; the result stores one cause.
   */
  virtual std::expected<void, DocumentError> finish(std::stop_token stop) {
    (void)stop;
    return {};
  }
};

/** @brief Cold exclusive file sink. Existing paths are never overwritten, including the source file.
 * The first worker write atomically creates the output. A failed/cancelled export
 * preserves its explicitly requested partial output; no rollback or durability is promised.
 */
class DocumentFileSink final : public DocumentExportSink {
public:
  /** @param path Owned output path; creation performs no filesystem access.
   * @return Cold sink or allocation/invalid-path failure.
   */
  static std::expected<std::shared_ptr<DocumentFileSink>, DocumentError> create(std::filesystem::path path);
  ~DocumentFileSink() override; ///< Close any still-owned descriptor without deleting a path.
  /** @param bytes Borrowed bounded output. @param stop Cooperative cancellation.
   * @return Actual native write count and native cause, including partial output.
   */
  DocumentWriteReply write(std::span<const std::byte> bytes, std::stop_token stop) override;
  /** @param stop Cancellation does not bypass close. @return Close failure, if any; never creates a path. */
  std::expected<void, DocumentError> finish(std::stop_token stop) override;

private:
  explicit DocumentFileSink(std::filesystem::path path);
  std::expected<void, DocumentError> open();
  std::filesystem::path path_;
  int descriptor_{-1};
  bool finished_{};
};

/** @brief Bounded export work configuration, independent of range/document size. */
struct DocumentExportOptions {
  std::size_t blockBytes{64 * 1024}; ///< Positive read quantum, additionally clamped to the source page maximum.
};

/** @brief Export outcome, including the prefix preserved after an error. */
struct DocumentExportResult {
  DocumentRevision revision;    ///< Source and revision fixed before the first output write.
  DocumentByteRange range;      ///< Requested range, with an observed exact end when available.
  std::uint64_t bytesRead{};    ///< Accepted source bytes, including unwritten pages and a possible range-start probe.
  std::uint64_t bytesWritten{}; ///< Exactly accepted output bytes unless writtenCountExact is false.
  bool writtenCountExact{true}; ///< False if a throwing/invalid sink reply prevented exact admission accounting.
  bool complete{};              ///< True only after the complete range and successful sink finalization.
  std::optional<DocumentError> error; ///< First failure only; a later finish failure is discarded. Empty on success.
};

/** @param source Retained logical source; no view or UI object is retained.
 * @param sink Owning worker-safe output policy. @param range Original logical-byte range, default through EOF.
 * @param options Bounded worker read quantum. @param stop Cooperative read/write cancellation.
 * @return Exact progress and source/sink outcome, including any preserved partial output.
 * Worker-only. Source revision/range are fixed at start and revalidated after the final write.
 * Gzip sources export decompressed bytes; text keeps its original encoding. No implicit clipboard
 * or full-output buffer is created. A changed Viewer model cannot redirect the retained source.
 * A nonzero start with unknown extent is validated by a one-byte preceding probe;
 * that case requires seekability. The sink is finalized even if validation fails.
 */
DocumentExportResult exportDocument(std::shared_ptr<ByteDocumentSource> source,
                                    std::shared_ptr<DocumentExportSink> sink, DocumentByteRange range = {},
                                    DocumentExportOptions options = {}, std::stop_token stop = {});

class DocumentCopy;
/** @param source Retained worker source. @param range Explicit range or known complete extent.
 * @param maxBytes Positive output ceiling; default64KiB, independent of source size.
 * @param stop Cooperative cancellation.
 * @return Immutable owning bytes charged to the source budget, or typed failure.
 * A range exceeding maxBytes is rejected before payload reads. An unknown open-ended
 * range is unsupported; use an explicit range or streaming export. OS clipboard limits
 * and delivery are the host's policy; this function does not write to the clipboard.
 */
std::expected<std::shared_ptr<const DocumentCopy>, DocumentError>
copyDocumentBytes(std::shared_ptr<ByteDocumentSource> source, DocumentByteRange range, std::size_t maxBytes = 64 * 1024,
                  std::stop_token stop = {});

/** @brief Immutable bounded byte copy whose accounting outlives its payload. */
class DocumentCopy {
public:
  ~DocumentCopy() = default; ///< Destroy bytes before returning the retained credit.

  /** @return Borrowed immutable copied bytes; valid until this owning copy is released. */
  std::span<const std::byte> bytes() const noexcept {
    return bytes_;
  }

  /** @return Exact original source qualification. */
  DocumentRevision revision() const noexcept {
    return revision_;
  }

private:
  friend std::expected<std::shared_ptr<const DocumentCopy>, DocumentError>
  copyDocumentBytes(std::shared_ptr<ByteDocumentSource>, DocumentByteRange, std::size_t, std::stop_token);

  DocumentCopy() = default;

  SourceCredit credit_;
  std::shared_ptr<SourceArena> arena_;
  std::span<const std::byte> bytes_;
  DocumentRevision revision_;
};
} // namespace portals
