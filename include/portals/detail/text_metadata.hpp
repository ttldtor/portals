// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/text.hpp>

namespace portals::detail {
/** @brief Numeric Unicode metadata; never retains source bytes or decoded scalar strings. */
struct TextMetadata {
  std::vector<Grapheme> graphemes;     ///< Whole-cluster byte boundaries and terminal widths.
  std::vector<std::size_t> wordStarts; ///< Whole-cluster starts of alphanumeric/mark runs.
  std::int32_t cells{};                ///< Checked total terminal cells.
  bool lineSeparator{};                ///< Input contained a separator excluded by one-line editors.
};

/** @brief Canonical metadata scanner into caller-owned fresh containers.
 * @param bytes Source borrowed only during this call; never copied or retained.
 * @param policy Terminal-width interpretation shared by all text consumers.
 * @param limits Whole-value and cluster limits; no truncation.
 * @param graphemes Fresh empty output owned by the caller; discard partial metadata after failure.
 * @param wordStarts Optional fresh empty word-boundary output; null avoids word classification/storage.
 * @param lineSeparator Optional initially false flag, set for one-line-excluded separators.
 * @return Checked total cell width or original text failure; container allocation can throw.
 */
std::expected<std::int32_t, TextError> scanTextMetadataInto(std::string_view bytes, WidthPolicy policy,
                                                            TextLimits limits, std::vector<Grapheme> &graphemes,
                                                            std::vector<std::size_t> *wordStarts = nullptr,
                                                            bool *lineSeparator = nullptr);

/** @brief Validate and segment borrowed UTF-8 without copying its bytes.
 * @param bytes Source borrowed synchronously; no pointer or scalar sequence is retained.
 * @param policy Terminal width interpretation shared with GraphemeText.
 * @param limits Whole-input and whole-cluster bounds; no truncation.
 * @param collectWordStarts Whether to retain additional numeric navigation boundaries.
 * @return Complete numeric metadata or the original Unicode/bounds failure; allocation may throw.
 */
std::expected<TextMetadata, TextError> scanTextMetadata(std::string_view bytes, WidthPolicy policy, TextLimits limits,
                                                        bool collectWordStarts = false);
} // namespace portals::detail
