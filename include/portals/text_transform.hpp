// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/widget.hpp>

namespace portals {
/** @brief Owning text-analysis limits; words are Unicode-whitespace-delimited tokens, not UAX #29 words. */
struct TextSplitOptions {
  TextLimits limits{};         ///< Whole input validation caps, including bytes, clusters, graphemes and cells.
  std::size_t maxTokens{4096}; ///< Positive token cap; exceeding it rejects the complete output.
};

namespace detail {
struct TextSegmentsData;
/// @cond INTERNAL
std::optional<TextError> textSplitLimit(const GraphemeText &input, const TextLimits &limits) noexcept;
/// @endcond
} // namespace detail

/** @brief One immutable whole-grapheme byte range retaining its original owning input. */
class TextSegment {
public:
  /** @brief Release a shared input owner; rvalue transfer uses the nonallocating shared-owner copy. */
  ~TextSegment() = default;
  /** @return Borrowed token bytes, valid until this segment is assigned or destroyed. */
  std::string_view bytes() const noexcept;
  /** @return Width policy of the retained immutable input. */
  WidthPolicy policy() const noexcept;
  /** @return Owning segmented token under its input policy; allocation failures propagate. */
  GraphemeText text() const;
  /** @param other Owning token to compare.
   * @return Equal token bytes and width policy, independently of original offsets/storage.
   */
  bool operator==(const TextSegment &other) const noexcept;

private:
  friend class TextSegments;

  TextSegment(std::shared_ptr<const detail::TextSegmentsData> owner, std::size_t first, std::size_t last)
      : owner_(std::move(owner)), first_(first), last_(last) {
  }

  std::shared_ptr<const detail::TextSegmentsData> owner_;
  std::size_t first_{};
  std::size_t last_{};
};

/** @brief Immutable bounded token collection; copies retain one input and its metadata without copying text. */
class TextSegments {
public:
  /** @brief Release one immutable collection owner; rvalue transfer retains a valid shared-owner source. */
  ~TextSegments() = default;
  /** @return Number of whole tokens, distinct from UTF-8 byte/scalar count. */
  std::size_t size() const noexcept;
  /** @param index Zero-based token ordinal.
   * @return Independently owner-retaining segment, or INVALID_ARGUMENT for an out-of-range ordinal.
   */
  std::expected<TextSegment, TreeError> at(std::size_t index) const;
  /** @param other Immutable ordered collection to compare.
   * @return Equal token bytes and policy; delimiter offsets in original inputs are not compared.
   */
  bool operator==(const TextSegments &other) const noexcept;

private:
  friend std::expected<TextSegments, TreeError> splitText(const GraphemeText &, bool, TextSplitOptions);

  explicit TextSegments(std::shared_ptr<const detail::TextSegmentsData> owner) : owner_(std::move(owner)) {
  }

  std::shared_ptr<const detail::TextSegmentsData> owner_;
};

/** @brief Split on whole graphemes consisting only of scalars in Unicode White_Space.
 * @param input Validated owning input borrowed only during the call; output retains an independent copy.
 * @param options Input bounds and positive token cap.
 * @return Owning tokens, INVALID_ARGUMENT for zero token cap, NODE_LIMIT for resource caps, NO_MEMORY or
 * CALLBACK_FAILED. Punctuation, apostrophes, CJK and emoji are preserved. This is not Unicode word-break or linguistic
 * analysis.
 */
std::expected<TextSegments, TreeError> words(const GraphemeText &input, TextSplitOptions options = {});

/** @brief Return one token for every existing extended grapheme, including whitespace.
 * @param input Validated owning input borrowed only during the call.
 * @param options Whole input bounds and positive token cap.
 * @return Independent immutable input owner and bounded ranges, or the same errors as words.
 */
std::expected<TextSegments, TreeError> graphemes(const GraphemeText &input, TextSplitOptions options = {});
} // namespace portals
