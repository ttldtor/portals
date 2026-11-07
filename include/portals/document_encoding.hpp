// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace portals {

/// Resolved byte encoding, or an unresolved selection.
enum class DocumentEncoding {
  UNRESOLVED,  ///< No encoding has been selected.
  UTF8,        ///< UTF-8.
  UTF16LE,     ///< UTF-16, little-endian.
  UTF16BE,     ///< UTF-16, big-endian.
  UTF32LE,     ///< UTF-32, little-endian.
  UTF32BE,     ///< UTF-32, big-endian.
  WINDOWS1251, ///< WHATWG Windows-1251.
  WINDOWS1252  ///< WHATWG Windows-1252.
};

/// Handling of malformed input; ESCAPE is reserved and unsupported.
enum class DocumentDecodePolicy {
  REPLACE,     ///< Emit a tagged replacement scalar for each malformed subpart.
  STRICT_MODE, ///< Fail transactionally on malformed input.
  ESCAPE       ///< Reserved; currently unsupported.
};

/// Completion, backpressure, or typed failure of a feed call.
enum class DocumentDecodeStatus {
  NEED_INPUT,          ///< More input is needed to continue.
  OUTPUT_FULL,         ///< More output capacity is needed to continue.
  WORK_LIMIT,          ///< The input admission budget was reached.
  DONE,                ///< Confirmed end of stream was processed.
  MALFORMED,           ///< Strict decoding encountered malformed input.
  ENCODING_UNRESOLVED, ///< Neither explicit choice, BOM nor hint selected an encoding.
  INVALID_OFFSET,      ///< Input is noncontiguous, overflows, or follows completion.
  UNSUPPORTED_POLICY   ///< Escape policy or zero work limit is unsupported.
};

/** @brief Decoded Unicode scalar with its exact interval in the original byte stream. */
struct DecodedScalar {
  char32_t scalar{};          ///< Unicode scalar value.
  std::uint64_t byteOffset{}; ///< Start of the original byte interval.
  std::uint8_t byteLength{};  ///< Length of the original byte interval.
  bool replacement{};         ///< True when emitted for malformed input.
};

/** @brief Committed feed progress or a transactional failure with its byte location. */
struct DocumentDecodeResult {
  std::size_t inputConsumed{};   ///< Input bytes committed by this call.
  std::size_t outputProduced{};  ///< Output scalars committed by this call.
  DocumentDecodeStatus status{}; ///< Outcome of the call.
  std::uint64_t errorOffset{};   ///< Start of the malformed interval when applicable.
  std::uint8_t errorLength{};    ///< Length of the malformed interval when applicable.
};

/** @brief Encoding selection, malformed-input policy and per-call work bound. */
struct DocumentDecoderOptions {
  DocumentEncoding explicitEncoding{DocumentEncoding::UNRESOLVED}; ///< Explicit choice, if any.
  DocumentEncoding hint{DocumentEncoding::UNRESOLVED};             ///< Fallback when no explicit choice or BOM exists.
  DocumentDecodePolicy policy{DocumentDecodePolicy::REPLACE};      ///< Malformed-input policy.
  std::size_t workLimit{65536}; ///< Maximum input bytes admitted per call; must be nonzero.
};

class DocumentDecoder;

/** Opaque decoder state captured at a proven scalar boundary.
 * The caller must bind this value to a source revision: a checkpoint alone
 * cannot establish that source bytes are unchanged.
 */
class DocumentDecoderCheckpoint {
  friend class DocumentDecoder;

  DocumentDecoderCheckpoint(DocumentDecoderOptions options, DocumentEncoding encoding, std::uint64_t nextOffset,
                            std::uint8_t bomLength, bool bomMismatch) noexcept
      : options_(options), encoding_(encoding), nextOffset_(nextOffset), bomLength_(bomLength),
        bomMismatch_(bomMismatch) {
  }

  DocumentDecoderOptions options_;
  DocumentEncoding encoding_;
  std::uint64_t nextOffset_;
  std::uint8_t bomLength_;
  bool bomMismatch_;
};

/// Result of proving a decoded-scalar boundary from a fixed local original-byte window.
enum class DocumentDecoderLocalStatus {
  READY,         ///< The containing valid/replacement scalar begins at restartByteOffset; checkpoint is present.
  AT_END,        ///< Confirmed EOF has no containing scalar; restartByteOffset is EOF and no checkpoint is supplied.
  NEED_PREFIX,   ///< More original bytes before the supplied local window are required to prove the boundary.
  NEED_SUFFIX,   ///< More original bytes or confirmed EOF are required to resolve the selected scalar.
  MALFORMED,     ///< STRICT_MODE rejects the selected original-byte subpart; no checkpoint is supplied.
  INVALID_RANGE, ///< Window exceeds the fixed size, overflows, contradicts BOM extent or excludes the requested anchor.
  INVALID_ORIGIN ///< Origin metadata is unresolved, unsupported or inconsistent with a real decoder boundary.
};

/// Owning local proof outcome; no input view or source owner is retained.
struct DocumentDecoderLocalResult {
  DocumentDecoderLocalStatus status{
    DocumentDecoderLocalStatus::NEED_SUFFIX}; ///< Unproven default, then proof/context/error.
  std::optional<DocumentDecoderCheckpoint>
    checkpoint{}; ///< Present only for READY; caller qualifies the source revision.
  std::uint64_t
    restartByteOffset{}; ///< Separate scalar-start projection for READY, or EOF for AT_END; never edits the anchor.
  std::uint64_t errorOffset{}; ///< First byte of the selected malformed subpart for MALFORMED.
  std::uint8_t errorLength{};  ///< Original-byte length of that malformed subpart, not a character count.
};

/**
 * Incrementally decode a single contiguous stream beginning at byte zero.
 *
 * The caller owns input and output and may discard them on return. The decoder
 * retains at most four input bytes and is not safe for concurrent access.
 * Offsets must equal the starting offset plus inputConsumed from prior successful calls.
 * The starting offset is zero for a new decoder or the checkpoint offset on restore.
 * final denotes confirmed EOF, not temporary absence of bytes. A matching BOM
 * at byte zero is omitted and reported by bomLength(); a conflicting BOM is
 * retained as input and reported by bomMismatch(). The explicit choice takes
 * precedence over a BOM, followed by the hint. No selection is an error.
 *
 * NEED_INPUT requests another chunk; OUTPUT_FULL requests output space;
 * WORK_LIMIT requests another call with the unconsumed suffix; DONE marks EOF.
 * These statuses commit state and report consumed/produced counts. MALFORMED,
 * ENCODING_UNRESOLVED, INVALID_OFFSET and UNSUPPORTED_POLICY leave state
 * unchanged, return zero counts and invalidate output written during that call.
 * STRICT_MODE reports the first malformed span; REPLACE emits U+FFFD tagged as
 * replacement. ESCAPE and a zero work limit are unsupported. A zero-capacity
 * output may return OUTPUT_FULL after buffering a bounded BOM prefix.
 *
 * Each scalar retains its original byte interval. Noncharacters are preserved;
 * valid controls are not sanitized and require safe rendering separately.
 * localCheckpoint() proves a bounded cold restart boundary; it does not certify
 * the omitted prefix's validity. Grapheme, line and cell mapping remain separate.
 */
class DocumentDecoder {
public:
  static constexpr std::size_t LOCAL_WINDOW_BYTES = 8; ///< Maximum original bytes admitted by localCheckpoint().

  /// Create a new stream decoder. A zero work limit or ESCAPE is unsupported.
  /// @param options Encoding selection, error policy and input work limit.
  explicit DocumentDecoder(DocumentDecoderOptions options = {}) noexcept;
  /// Resume a previously captured boundary without probing for another BOM.
  /// @param checkpoint Boundary state bound by the caller to the source revision.
  /// @return Decoder restored to the captured boundary.
  static DocumentDecoder restore(const DocumentDecoderCheckpoint &checkpoint) noexcept;
  /** Prove the scalar/subpart containing an anchor without traversing its omitted prefix.
   * @param origin Real opaque checkpoint after original encoding/BOM resolution, qualified
   * by the caller against the same immutable source revision and decoding policy.
   * @param window At most LOCAL_WINDOW_BYTES original bytes, borrowed only for this call.
   * @param windowOffset Absolute original-byte offset of the local window, which may be unaligned.
   * @param anchorByteOffset Exact user anchor within the inclusive window interval. It stays
   * unchanged; the returned scalar-start projection can precede it, or follow it inside an omitted BOM.
   * @param finalWindow True only when window end is proven source EOF, never temporary absence.
   * @return Owning proof/error result. Only READY supplies a real checkpoint for restore();
   * AT_END supplies none, so odd EOF tails cannot become continuation alignment after append.
   * Missing local context returns NEED_PREFIX/NEED_SUFFIX. STRICT_MODE validates the selected
   * subpart and resumed reads, not the omitted whole prefix. The origin is never mutated and
   * effective encoding, original BOM decision, policy and work limit are preserved.
   */
  static DocumentDecoderLocalResult localCheckpoint(const DocumentDecoderCheckpoint &origin,
                                                    std::span<const std::byte> window, std::uint64_t windowOffset,
                                                    std::uint64_t anchorByteOffset, bool finalWindow = false) noexcept;
  /// Capture only after selection, with no pending bytes, before EOF.
  /// @return Opaque boundary state, or an empty optional when capture is unavailable.
  std::optional<DocumentDecoderCheckpoint> checkpoint() const noexcept;
  /// Decode a bounded contiguous input chunk into caller-owned scalar records.
  /// @param input Borrowed bytes contiguous with the committed input.
  /// @param offset Absolute offset of the first input byte.
  /// @param final True only when the end of input is confirmed.
  /// @param output Caller-owned storage for decoded scalars.
  /// @return Committed counts and status, or a typed failure with error location.
  DocumentDecodeResult feed(std::span<const std::byte> input, std::uint64_t offset, bool final,
                            std::span<DecodedScalar> output) noexcept;

  /// @return Selected encoding, or UNRESOLVED before selection.
  DocumentEncoding encoding() const noexcept {
    return encoding_;
  }

  /// @return Size of a matching BOM omitted at the original stream start.
  std::uint8_t bomLength() const noexcept {
    return bomLength_;
  }

  /// @return True if an explicit choice conflicted with the original BOM.
  bool bomMismatch() const noexcept {
    return bomMismatch_;
  }

private:
  DocumentDecodeResult feedImpl(std::span<const std::byte>, std::uint64_t, bool, std::span<DecodedScalar>) noexcept;
  DocumentDecoderOptions options_;
  DocumentEncoding encoding_{DocumentEncoding::UNRESOLVED};
  std::array<std::uint8_t, 4> pending_{};
  std::uint64_t pendingOffset_{};
  std::uint64_t nextOffset_{};
  std::uint8_t pendingSize_{};
  std::uint8_t bomLength_{};
  bool selected_{};
  bool bomMismatch_{};
  bool done_{};
};
} // namespace portals
