// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace portals {

/** @brief Terminal policy for printable East Asian Ambiguous scalars. */
enum class AmbiguousWidth {
  /** @brief Use one cell. */
  narrow,
  /** @brief Use two cells. */
  wide
};

/** @brief Terminal policy for recognized emoji clusters, including single emoji. */
enum class EmojiWidth {
  /** @brief Use one cell. */
  one = 1,
  /** @brief Use two cells. */
  two = 2
};

/**
 * @brief Owning deterministic width policy; no font or terminal matching promise.
 *
 * Include the complete value in measurement/layout/cell-buffer cache keys.
 * A policy change requires rebuilding text and invalidating affected geometry
 * and paint. Independent values have no thread affinity.
 */
struct WidthPolicy {
  /** @brief Width of printable Ambiguous scalars. */
  AmbiguousWidth ambiguous{AmbiguousWidth::narrow};
  /** @brief Width of recognized emoji clusters. */
  EmojiWidth emoji{EmojiWidth::two};
  /** @brief Caller-owned terminal capability revision, included in equality. */
  std::uint64_t revision{};
  /** @brief Compare the full cache-key policy. */
  bool operator==(const WidthPolicy &) const noexcept = default;
};

/** @brief Owning limits checked without silently truncating input or clusters. */
struct TextLimits {
  /** @brief Maximum input bytes; also bounded by PTRDIFF_MAX. */
  std::size_t max_input_bytes{1024 * 1024};
  /** @brief Maximum bytes in one extended grapheme cluster. */
  std::size_t max_cluster_bytes{64 * 1024};
  /** @brief Maximum cluster records. */
  std::size_t max_graphemes{1024 * 1024};
  /** @brief Maximum measured cells; additionally bounded by INT32_MAX. */
  std::size_t max_cells{2 * 1024 * 1024};
};

/** @brief Observable text construction/cursor failure. */
enum class TextErrorKind {
  /** @brief Invalid UTF-8, including overlong, surrogate or truncated input. */
  invalid_utf8,
  /** @brief Input length exceeds its configured/representation limit. */
  input_limit,
  /** @brief One whole cluster exceeds its byte limit. */
  cluster_limit,
  /** @brief The number of clusters exceeds its limit. */
  grapheme_limit,
  /** @brief Cell accumulation exceeds its configured/int32_t limit. */
  cell_limit,
  /** @brief A width policy enum value is not recognized. */
  invalid_policy,
  /** @brief A cursor offset is outside the text or inside a cluster. */
  not_boundary
};

/** @brief Owning error value, independent of the failed input's lifetime. */
struct TextError {
  /** @brief Failure category. */
  TextErrorKind kind{};
  /**
   * @brief Byte offset of an invalid sequence, rejected cluster or cursor.
   * Input-limit and invalid-policy errors use zero.
   */
  std::size_t byte_offset{};
  /** @brief Compare category and byte offset. */
  bool operator==(const TextError &) const noexcept = default;
};

/** @brief How a cell renderer must display a complete logical cluster. */
enum class TextDisplay {
  /** @brief Original cluster bytes may be displayed under the selected policy. */
  printable,
  /** @brief Display ASCII '?' in one cell; never emit the original bytes. */
  unsupported
};

/** @brief Owning metadata for a half-open UTF-8 byte range. */
struct Grapheme {
  /** @brief Inclusive byte offset in GraphemeText::bytes(). */
  std::size_t byte_begin{};
  /** @brief Exclusive byte offset in GraphemeText::bytes(). */
  std::size_t byte_end{};
  /** @brief One or two cells, including a one-cell unsupported placeholder. */
  std::uint8_t cells{};
  /** @brief Renderer disposition; preserves the original logical bytes. */
  TextDisplay display{};
};

/** @brief Owning metadata for the maximal whole-cluster prefix in a cell budget. */
struct TextClip {
  /** @brief Exclusive byte end at an extended grapheme boundary. */
  std::size_t byte_end{};
  /** @brief Number of included clusters. */
  std::size_t graphemes{};
  /** @brief Measured cells in the prefix. */
  std::int32_t cells{};
};

/**
 * @brief Owning validated UTF-8 and immutable extended grapheme measurements.
 *
 * Unicode 18.0.0 boundaries are independent of display support: controls,
 * standalone marks and unsupported widths remain whole logical clusters.
 * No I/O, callbacks or mutable global state. Construction/copy can allocate and
 * throw std::bad_alloc or std::length_error; semantic failures use TextError.
 * Any thread may own a value; concurrent reads of one unchanged value are safe.
 * Assignment/move/destruction require caller synchronization and invalidate all
 * borrowed views. Moving resets a distinct source to empty; self-move assignment is a no-op. Policy is captured at
 * construction; replacing a value under a new policy requires layout/paint
 * invalidation by the caller. This component does not own terminal cells.
 */
class GraphemeText {
public:
  /** @brief Copy bytes and metadata into independent storage; may allocate. */
  GraphemeText(const GraphemeText &) = default;
  /** @brief Replace storage with an independent copy; invalidates borrowed views. */
  GraphemeText &operator=(const GraphemeText &other);
  /** @brief Transfer storage and reset the distinct source to empty; invalidates source views. */
  GraphemeText(GraphemeText &&other) noexcept;
  /** @brief Transfer storage; invalidates both objects' views and empties a distinct source. Self-move is a no-op. */
  GraphemeText &operator=(GraphemeText &&other) noexcept;

  /**
   * @brief Validate, segment and measure a single logical UTF-8 buffer.
   * @param bytes Borrowed only for this synchronous call; copied on success.
   * @param policy Owning policy value, retained on success.
   * @param limits Owning resource caps; zero caps permit only empty input.
   * @return An independent text value or TextError; never a partial result.
   * @note Any thread; allocations can throw. No input repair or normalization.
   */
  [[nodiscard]] static std::expected<GraphemeText, TextError> from(std::string_view bytes, WidthPolicy policy = {},
                                                                   TextLimits limits = {});
  /** @brief Borrow original bytes until this object's assignment/move/destruction. */
  [[nodiscard]] std::string_view bytes() const noexcept;
  /** @brief Borrow metadata until this object's assignment/move/destruction. */
  [[nodiscard]] std::span<const Grapheme> graphemes() const noexcept;
  /** @brief Return cached int32_t-compatible total cells; no allocation or errors. */
  [[nodiscard]] std::int32_t measure() const noexcept;
  /** @brief Return the owning captured cache-key policy; no errors. */
  [[nodiscard]] WidthPolicy policy() const noexcept;
  /**
   * @brief Advance a byte-boundary cursor by one cluster; end stays at end.
   * @param byte_offset Valid boundary including zero and byte length.
   * @return Next boundary or not_boundary with the supplied offset.
   * @note Any thread; no mutation, allocation, exceptions or invalidation.
   */
  [[nodiscard]] std::expected<std::size_t, TextError> next_cursor(std::size_t byte_offset) const noexcept;
  /**
   * @brief Retreat a byte-boundary cursor by one cluster; zero stays at zero.
   * @param byte_offset Valid boundary including zero and byte length.
   * @return Previous boundary or not_boundary with the supplied offset.
   * @note Any thread; no mutation, allocation, exceptions or invalidation.
   */
  [[nodiscard]] std::expected<std::size_t, TextError> previous_cursor(std::size_t byte_offset) const noexcept;
  /**
   * @brief Return the maximal prefix fitting a cell budget, stopping before a wide cluster that cannot fit.
   * @param max_cells Available cells; negative values normalize to zero.
   * @return Independent metadata at a whole-cluster boundary, with no partial glyph.
   * @note Any thread; no mutation, allocation, exceptions or invalidation.
   */
  [[nodiscard]] TextClip clip_prefix(std::int32_t max_cells) const noexcept;

private:
  GraphemeText() = default;
  std::string bytes_;
  std::vector<Grapheme> graphemes_;
  WidthPolicy policy_;
  std::int32_t cells_{};
};

} // namespace portals
