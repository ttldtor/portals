// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/list_text.hpp>
#include <portals/model.hpp>

#include <tuple>

namespace portals {
/** @brief Search/provider errors, separate from scheduler and UI delivery errors. */
enum class SearchError {
  INVALID_ARGUMENT, ///< Invalid configuration or malformed source identity.
  INVALID_TEXT,     ///< Invalid UTF-8 or unsupported text policy.
  NOT_READY,        ///< Source snapshot has no ready immutable rows.
  LIMIT_EXCEEDED,   ///< A finite byte, row, grapheme, frontier or work budget was exceeded.
  STALE_SOURCE,     ///< The exact source/policy key no longer matches.
  CANCELLED,        ///< Cooperative cancellation was observed.
  NO_MEMORY,        ///< Allocation failed within the declared budget.
  CALLBACK_FAILED,  ///< Projection/read callback threw.
  UNAVAILABLE       ///< Borrowed executor does not offer worker-owned cache storage.
};
/** @brief Indexed term-prefix strategy; fuzzy edit distance is over normalized graphemes. */
enum class SearchStrategy {
  PREFIX,      ///< Exact normalized prefix of the complete label or a whitespace token.
  FUZZY_PREFIX ///< Also allow one insertion/deletion/substitution from three query graphemes.
};

/** @brief Configurable finite source, index, query and result budgets. */
struct SearchOptions {
  std::size_t maxSourceRows{100000};                     ///< Maximum immutable source rows.
  std::size_t maxProjectedBytes{33554432};               ///< Maximum original projected UTF-8 bytes per index.
  std::size_t maxRowBytes{4096};                         ///< Maximum bytes in one suggestion label.
  std::size_t maxRowGraphemes{1024};                     ///< Maximum source graphemes per label.
  std::size_t maxQueryBytes{4096};                       ///< Maximum owning query bytes, independently editor-limited.
  std::size_t maxQueryGraphemes{64};                     ///< Maximum normalized query graphemes.
  std::size_t maxTermsPerRow{64};                        ///< Complete label plus nonempty whitespace tokens.
  std::size_t maxIndexBytes{268435456};                  ///< Resident/transient resident allocation-capacity ceiling.
  std::size_t maxBuildScratchBytes{268435456};           ///< Independent build scratch ceiling.
  std::size_t topK{32};                                  ///< Exact output count bound, configurable from one to256.
  std::size_t maxHighlightRangesPerItem{64};             ///< Maximum merged whole-grapheme highlight ranges.
  std::size_t maxQuerySteps{2000000};                    ///< Grapheme DP cells, heap pushes/pops and postings examined.
  std::size_t maxQueryScratchBytes{8388608};             ///< Query frontier/DP/dedup allocation-capacity ceiling.
  SearchStrategy strategy{SearchStrategy::FUZZY_PREFIX}; ///< Prefix or one-edit prefix matching.
};

/** @brief Half-open byte range in the original label, always on complete grapheme boundaries. */
struct SearchHighlight {
  std::size_t begin{};                                               ///< Inclusive original UTF-8 byte offset.
  std::size_t end{};                                                 ///< Exclusive original UTF-8 byte offset.
  /** @return True when both original-label byte offsets are equal. */
  bool operator==(const SearchHighlight &) const noexcept = default;
};

/** @brief Deterministic rank. Smaller tuple sorts first; source ordinal resolves equal scores. */
struct SearchRank {
  std::uint8_t edits{};          ///< Zero or one normalized-grapheme edit.
  bool token{};                  ///< Full-label terms precede token-only matches.
  std::size_t unmatched{};       ///< Remaining normalized term graphemes.
  std::uint64_t sourceOrdinal{}; ///< Stable order in this exact source pin.
  RowId row;                     ///< Final tie break includes row generation.

  /** @return Lexicographic deterministic rank tuple. */
  auto tuple() const noexcept {
    return std::tie(edits, token, unmatched, sourceOrdinal, row.value, row.generation);
  }
};

/** @brief Immutable copied top-K item. It never owns the source corpus or worker index. */
struct SearchItem {
  SearchItem() = default;                   ///< Construct an empty item inside a checked allocation boundary.
  SearchItem(const SearchItem &) = default; ///< Copy only bounded presentation and highlight data.
  /** @return Reference to this item after copying bounded output. */
  SearchItem &operator=(const SearchItem &) = default;

  /** @brief Transfer output through a potentially throwing empty-container construction.
   * MSVC checked iterators allocate a proxy even when moving an empty vector.
   * @param other Bounded item whose presentation and highlights are transferred.
   */
  SearchItem(SearchItem &&other) : presentation(std::move(other.presentation)), highlights(0), rank(other.rank) {
    highlights.swap(other.highlights);
  }

  /** @brief Transfer bounded output without allocating another container proxy.
   * @param other Bounded item whose presentation and highlights are transferred.
   * @return Reference to this item after the transfer.
   */
  SearchItem &operator=(SearchItem &&other) noexcept {
    presentation = std::move(other.presentation);
    highlights.swap(other.highlights);
    rank = other.rank;
    return *this;
  }

  std::shared_ptr<const detail::PreparedListText> presentation;              ///< Bounded prevalidated worker display.
  std::vector<SearchHighlight> highlights = std::vector<SearchHighlight>(0); ///< Whole original grapheme ranges.
  SearchRank rank;                                                           ///< Exact deterministic provider rank.

  /** @return Original owning label text; borrow remains valid while this item's presentation lives. */
  const GraphemeText &text() const noexcept {
    return presentation->text;
  }
};

/** @brief Exact source identity independent of a result model's publication revision. */
struct SearchSourceIdentity {
  std::weak_ptr<const void> owner;       ///< Control-block identity without retaining arbitrary source payload.
  const void *pin{};                     ///< Exact source snapshot address, compared together with owner identity.
  std::uint64_t revision{};              ///< Domain source revision.
  std::uint64_t session{};               ///< Provider/projection instance; nonzero and never recycled.
  std::uint64_t normalizationVersion{1}; ///< Canonical-compose/full-casefold mapping algorithm version.
  WidthPolicy width;                     ///< Complete text measurement policy.

  /** @param other Exact identity to compare, including owner control block.
   * @return True only for the same immutable source and projection/normalization policy.
   */
  bool operator==(const SearchSourceIdentity &other) const noexcept {
    return !owner.owner_before(other.owner) && !other.owner.owner_before(owner) && pin == other.pin &&
           revision == other.revision && session == other.session &&
           normalizationVersion == other.normalizationVersion && width == other.width;
  }
};

/** @brief Immutable bounded search value, distinct from QueryPage<T> and source row count. */
struct SearchResult {
  SearchSourceIdentity source;              ///< Exact weak source identity; does not own the index.
  std::string query = std::string(0, '\0'); ///< Owning original query bytes.
  std::uint64_t generation{};               ///< Latest UI query intent, not a source/result revision.
  std::vector<ModelRow<SearchItem>> rows = std::vector<ModelRow<SearchItem>>(0); ///< Ranked deduplicated top-K.
  bool hasMore{};           ///< One further distinct result was proven.
  std::size_t querySteps{}; ///< Counted worker query work, independent of UI/frame time.
};
} // namespace portals
