// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/document_encoding.hpp>
#include <portals/document_source.hpp>

namespace portals {
/** @brief Literal navigation order; wrapping belongs to the caller. */
enum class DocumentSearchDirection {
  NEXT,    ///< First whole-grapheme match starting at or after the requested anchor.
  PREVIOUS ///< Last whole-grapheme match starting strictly before the requested anchor.
};

/** @brief One bounded advance; PENDING is ordinary continuation, not source backpressure. */
enum class DocumentSearchState {
  PENDING, ///< The work quantum finished; call advance again for this same request.
  FOUND,   ///< A selected owning match is available.
  END      ///< This request has no further eligible match; no implicit wrap occurs.
};

/** @brief Query/work/result limits, independent of document extent or total match count. */
struct DocumentSearchOptions {
  std::size_t maxQueryBytes{16 * 1024}; ///< Positive UTF-8 query admission cap.
  std::size_t workBytes{64 * 1024};     ///< Positive original source admission/processing quantum per advance.
  std::size_t maxSnippetBytes{256};     ///< Positive whole-grapheme UTF-8 context cap, including the shown match.
};

/** @brief Source/admission cause and, when applicable, the precise decoder failure. */
struct DocumentOperationFailure {
  DocumentError error; ///< Original source error or typed query/storage failure; budgetDemand remains exact.
  std::optional<DocumentDecodeResult> decode; ///< Present for decoder status, original error offset and length.
};

/** @brief Search-specific spelling of the shared document operation failure. */
using DocumentSearchFailure = DocumentOperationFailure;

class DocumentSearchSession;

/** @brief Immutable owning selection; remains readable after session/source replacement.
 * text is the exact decoded UTF-8 literal. snippet contains bounded preceding
 * whole-grapheme context and the match; it is empty when the match exceeds its
 * snippet cap. Neither string borrows source pages or UI state.
 */
struct DocumentSearchMatch {
private:
  friend class DocumentSearchSession;
  SourceCredit credit_; // Payload is destroyed before returning its independent reservation.

public:
  DocumentRevision revision;                  ///< Exact source namespace and provider revision.
  std::uint64_t byteOffset{};                 ///< Inclusive offset in the original source encoding.
  std::uint64_t byteLength{};                 ///< Original byte length, with an exclusive whole-grapheme end.
  std::string text = std::string(0, '\0');    ///< Complete UTF-8 match, bounded by maxQueryBytes.
  std::string snippet = std::string(0, '\0'); ///< Whole-grapheme context, bounded by maxSnippetBytes.
  std::size_t snippetMatchOffset{};           ///< Inclusive UTF-8 match offset inside snippet.
  std::size_t snippetMatchLength{};           ///< UTF-8 match length inside snippet; zero for an omitted snippet.
  bool snippetTruncated{};                    ///< The complete match cannot fit inside the configured snippet cap.

  /** @return Retained credit bytes; no allocation, locking or source access. */
  std::size_t retainedBytes() const noexcept {
    return credit_.usage().bytes;
  }
};

/** @brief Value-only progress with at most one selected owning result. */
struct DocumentSearchStep {
  DocumentSearchState state{DocumentSearchState::PENDING}; ///< Explicit bounded-search outcome.
  DocumentRevision revision;                               ///< Revision qualified at the current worker advance.
  std::uint64_t scannedByteOffset{};                ///< Exact original-byte decoder progress, not a total hit count.
  std::optional<std::uint64_t> byteExtent;          ///< Current exact extent when known, without a forced EOF scan.
  std::shared_ptr<const DocumentSearchMatch> match; ///< Present only for FOUND.
};

/** @brief Bounded worker cursor for exact, case-sensitive literal search without normalization.
 * Search starts at the real source origin so grapheme boundaries are proven.
 * Query, KMP/scalar maps, scratch and selected results use source->budget().
 * Sources must be seekable; unsupported streams return UNSUPPORTED explicitly.
 * Operations serialize on this session, but start/advance belong on workers:
 * they may wait for another operation, and advance performs source I/O. No UI
 * reference or callback is retained. Cancellation preserves consistent accepted
 * input; a later advance can resume when the query/request/revision is unchanged.
 * A revision change fails instead of mixing matches from different generations.
 * The byte quantum limits logical source admissions/processing; an underlying
 * provider's cold replay or blocking I/O can still occupy its worker longer.
 * Capacity failures preserve exact budgetDemand; budgetPeak includes retained
 * session/page storage plus the failed operation, excluding source/provider pins
 * and other callers' retained matches. These are capacities, not process RSS.
 */
class DocumentSearchSession {
public:
  /** @param source Retained logical byte source with an existing accounting root.
   * @param utf8Query Nonempty valid UTF-8 literal; matching never folds case or normalizes.
   * @param decoderOptions Original source encoding/error policy; UTF-8 hint is the default.
   * @param options Bounded query, worker and result capacities.
   * @return Cold session with initial NEXT-from-zero request, or typed validation/admission failure.
   * Creation performs no source metadata/read I/O and may run on a worker.
   */
  static std::expected<std::shared_ptr<DocumentSearchSession>, DocumentSearchFailure>
  create(std::shared_ptr<ByteDocumentSource> source, std::string_view utf8Query,
         DocumentDecoderOptions decoderOptions = {.hint = DocumentEncoding::UTF8}, DocumentSearchOptions options = {});

  ~DocumentSearchSession(); ///< Release retained pages/maps before their accounting owners.
  /** @return Retained session/page capacities, excluding independent matches and source/provider pins.
   * Allocation-free and lock-free. The caller must serialize this worker-only
   * observation with start/advance; concurrent UI reads are not supported.
   */
  SourceBudgetUsage retainedUsage() const noexcept;
  /** @param direction NEXT or PREVIOUS; wrapping is not performed by the kernel.
   * @param anchorByte NEXT accepts starts >= anchor; PREVIOUS accepts starts < anchor.
   * @return Success after resetting a request, or typed validation failure.
   * Both directions restart from the real origin. Consecutive NEXT after FOUND
   * should call advance directly to preserve the streaming overlap/KMP state.
   * A new request also clears the previous revision qualification.
   */
  std::expected<void, DocumentSearchFailure> start(DocumentSearchDirection direction, std::uint64_t anchorByte = 0);
  /** @param stop Cooperative cancellation before/after reads and during bounded processing.
   * @return PENDING, FOUND or END with owning progress, or an exact source/decoder/admission cause.
   * NEXT continues after FOUND and includes overlaps. PREVIOUS rescans only the
   * needed prefix, retains the last eligible match, and is terminal after FOUND.
   */
  std::expected<DocumentSearchStep, DocumentSearchFailure> advance(std::stop_token stop = {});

private:
  struct State;
  explicit DocumentSearchSession(std::unique_ptr<State> state);
  std::unique_ptr<State> state_;
};
} // namespace portals
