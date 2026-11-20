// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/document_hex.hpp>

#include <algorithm>
#include <limits>

namespace portals {
namespace {

HexByteClass classify(unsigned value) noexcept {
  if (value == 0)
    return HexByteClass::ZERO;
  if (value == 0x20)
    return HexByteClass::SPACE;
  if (value >= 0x80)
    return HexByteClass::HIGH_BYTE;
  if (value < 0x20 || value == 0x7f)
    return HexByteClass::CONTROL;
  return HexByteClass::PRINTABLE;
}

} // namespace

std::expected<std::size_t, HexError> hexRowWidth(HexRowOptions options) noexcept {
  if (options.bytesPerRow == 0 || options.bytesPerRow > MAX_HEX_ROW_BYTES || options.groupSize == 0 ||
      options.groupSize > MAX_HEX_ROW_BYTES) {
    return std::unexpected(HexError::INVALID_OPTIONS);
  }

  const auto groups = (options.bytesPerRow - 1) / options.groupSize;
  const auto hexWidth = options.bytesPerRow * 3 - 1 + groups;
  return 18 + hexWidth + (options.showAscii ? 4 + options.bytesPerRow : 0);
}

std::expected<HexRowResult, HexError> formatHexRow(std::span<const std::byte> input, std::uint64_t absoluteOffset,
                                                   HexRowOptions options, std::span<char> text,
                                                   std::span<HexByteCell> cells) noexcept {
  const auto width = hexRowWidth(options);
  if (!width)
    return std::unexpected(width.error());

  const auto count = std::min(input.size(), options.bytesPerRow);
  if (count > std::numeric_limits<std::uint64_t>::max() - absoluteOffset)
    return std::unexpected(HexError::OFFSET_OVERFLOW);
  if (text.size() < *width)
    return std::unexpected(HexError::TEXT_TOO_SMALL);
  if (cells.size() < count)
    return std::unexpected(HexError::CELLS_TOO_SMALL);

  const char *digits = options.uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
  std::fill_n(text.begin(), *width, ' ');
  for (std::size_t i = 0; i < 16; ++i) {
    text[15 - i] = digits[(absoluteOffset >> (i * 4)) & 0xf];
  }

  const auto asciiStart = *width - options.bytesPerRow - 1;
  if (options.showAscii) {
    text[asciiStart - 1] = '|';
    text[*width - 1] = '|';
  }

  for (std::size_t i = 0; i < count; ++i) {
    const auto value = std::to_integer<unsigned>(input[i]);
    const auto column = 18 + i * 3 + i / options.groupSize;
    text[column] = digits[value >> 4];
    text[column + 1] = digits[value & 0xf];
    std::optional<std::size_t> asciiColumn;
    if (options.showAscii) {
      asciiColumn = asciiStart + i;
      text[*asciiColumn] = value >= 0x20 && value <= 0x7e ? static_cast<char>(value) : '.';
    }
    cells[i] = {absoluteOffset + i, column, asciiColumn, classify(value)};
  }

  return HexRowResult{count, *width};
}

} // namespace portals
