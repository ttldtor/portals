// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

namespace portals {

/** Maximum amount of input processed in one row, not a document size limit. */
inline constexpr std::size_t MAX_HEX_ROW_BYTES = 4096;

/** Semantic classification for a source byte. */
enum class HexByteClass {
  ZERO,      ///< Zero byte.
  SPACE,     ///< ASCII space.
  PRINTABLE, ///< ASCII graphic character.
  CONTROL,   ///< Nonzero ASCII control byte, including DEL.
  HIGH_BYTE  ///< Byte at or above 0x80.
};

/** Failure before any output is written. */
enum class HexError {
  INVALID_OPTIONS, ///< A row or group size is outside the supported range.
  OFFSET_OVERFLOW, ///< The offset after consumed input cannot fit in uint64_t.
  TEXT_TOO_SMALL,  ///< The text output cannot hold a complete row.
  CELLS_TOO_SMALL  ///< The cell output cannot hold all consumed bytes.
};

/** Formatting parameters for a fixed-width row. */
struct HexRowOptions {
  std::size_t bytesPerRow = 16; ///< Number of byte slots, from 1 through MAX_HEX_ROW_BYTES.
  std::size_t groupSize = 8;    ///< Slots per group, from 1 through MAX_HEX_ROW_BYTES.
  bool uppercase = true;        ///< Use uppercase hexadecimal digits when true.
  bool showAscii = true;        ///< Append a safe ASCII column when true.
};

/** Display positions and semantic class of one source byte. */
struct HexByteCell {
  std::uint64_t byteOffset;               ///< Absolute source byte offset.
  std::size_t hexColumn;                  ///< Zero-based column of the first hex digit.
  std::optional<std::size_t> asciiColumn; ///< Zero-based ASCII column, absent when disabled.
  HexByteClass byteClass;                 ///< Semantic class for presentation.
};

/** Counts describing output written into caller-owned buffers. */
struct HexRowResult {
  std::size_t bytesConsumed; ///< Number of input bytes represented.
  std::size_t columns;       ///< Number of characters written, without a terminator.
};

/** Calculate the complete width of a row for layout and auto-fit decisions.
 * @param options Row formatting parameters.
 * @return Required character count or INVALID_OPTIONS.
 */
[[nodiscard]] std::expected<std::size_t, HexError> hexRowWidth(HexRowOptions options) noexcept;

/** Format at most one row, padding absent byte slots with spaces.
 * Input, text, and cells must not overlap. Input is not retained or modified;
 * output remains in caller-owned buffers and is valid for their lifetime. Every
 * output character is printable ASCII. No terminator is appended. The caller
 * must account for the output capacity before calling. Errors write nothing.
 * @param input Source bytes; excess beyond one row remains unconsumed.
 * @param absoluteOffset Absolute offset of the first source byte.
 * @param options Row formatting parameters.
 * @param text Caller-owned text output with at least hexRowWidth(options) elements.
 * @param cells Caller-owned output with at least the consumed byte count elements.
 * @return Counts of consumed bytes and written columns, or a typed error.
 */
[[nodiscard]] std::expected<HexRowResult, HexError> formatHexRow(std::span<const std::byte> input,
                                                                 std::uint64_t absoluteOffset, HexRowOptions options,
                                                                 std::span<char> text,
                                                                 std::span<HexByteCell> cells) noexcept;

} // namespace portals
