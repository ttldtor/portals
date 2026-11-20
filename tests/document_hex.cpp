// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/document_hex.hpp>

#include <array>
#include <limits>
#include <string_view>

using namespace portals;

TEST_CASE("golden rows and positions") {
  std::array<std::byte, 16> bytes{};
  for (unsigned i = 0; i < bytes.size(); ++i)
    bytes[i] = std::byte(i + 0x20);
  std::array<char, 200> text{};
  std::array<HexByteCell, 16> cells{};
  auto result = formatHexRow(bytes, 0x100000001ULL, {}, text, cells);
  REQUIRE(result);
  CHECK(std::string_view(text.data(), result->columns) ==
        "0000000100000001  20 21 22 23 24 25 26 27  28 29 2A 2B 2C 2D 2E 2F  | !\"#$%&'()*+,-./|");
  CHECK(result->bytesConsumed == 16);
  CHECK(cells[8].hexColumn == 43);
  CHECK(cells[8].byteOffset == 0x100000009ULL);
  CHECK(cells[0].byteClass == HexByteClass::SPACE);
  CHECK(cells[1].byteClass == HexByteClass::PRINTABLE);
  CHECK(cells[0].asciiColumn == 69);

  HexRowOptions options{4, 2, false, true};
  result = formatHexRow(std::span(bytes).first(2), 0, options, text, cells);
  REQUIRE(result);
  CHECK(std::string_view(text.data(), result->columns) == "0000000000000000  20 21         | !  |");
  CHECK(result->bytesConsumed == 2);
  result = formatHexRow({}, 0, options, text, cells);
  REQUIRE(result);
  CHECK(std::string_view(text.data(), result->columns) == "0000000000000000                |    |");
}

TEST_CASE("all byte classifications and safe output") {
  std::array<std::byte, 256> input{};
  for (unsigned i = 0; i < 256; ++i)
    input[i] = std::byte(i);
  std::array<char, 2048> text{};
  std::array<HexByteCell, 256> cells{};
  auto result = formatHexRow(input, 20, {256, 8, false, true}, text, cells);
  REQUIRE(result);
  for (unsigned i = 0; i < 256; ++i) {
    CHECK(cells[i].byteOffset == i + 20);
    REQUIRE(cells[i].asciiColumn);
    CHECK(text[*cells[i].asciiColumn] == (i >= 32 && i <= 126 ? char(i) : '.'));
    auto expected = i == 0                 ? HexByteClass::ZERO
                    : i == 32              ? HexByteClass::SPACE
                    : i >= 128             ? HexByteClass::HIGH_BYTE
                    : (i < 32 || i == 127) ? HexByteClass::CONTROL
                                           : HexByteClass::PRINTABLE;
    CHECK(cells[i].byteClass == expected);
  }
  for (std::size_t i = 0; i < result->columns; ++i)
    CHECK(text[i] >= 32);
  CHECK(text[cells[255].hexColumn] == 'f');
  CHECK(text[cells[255].hexColumn + 1] == 'f');
}

TEST_CASE("width, truncation to a row, and disabled ASCII") {
  HexRowOptions options{3, 1, false, false};
  auto width = hexRowWidth(options);
  REQUIRE(width);
  CHECK(*width == 28);
  std::array<std::byte, 4> input{std::byte{0xab}, std::byte{0xcd}, std::byte{0xef}, std::byte{0}};
  std::array<char, 28> text{};
  std::array<HexByteCell, 3> cells{};
  auto result = formatHexRow(input, 0, options, text, cells);
  REQUIRE(result);
  CHECK(result->bytesConsumed == 3);
  CHECK(result->columns == 28);
  CHECK(std::string_view(text.data(), text.size()) == "0000000000000000  ab  cd  ef");
  CHECK_FALSE(cells[0].asciiColumn.has_value());
}

TEST_CASE("failures preserve outputs") {
  std::array<std::byte, 2> input{std::byte{1}, std::byte{2}};
  std::array<char, 100> text{};
  text.fill('!');
  std::array<HexByteCell, 2> cells{{{42, 42, 42, HexByteClass::SPACE}, {42, 42, 42, HexByteClass::SPACE}}};
  auto checkUnchanged = [&] {
    for (char c : text)
      CHECK(c == '!');
    for (const auto &cell : cells) {
      CHECK(cell.byteOffset == 42);
      CHECK(cell.hexColumn == 42);
      CHECK(cell.asciiColumn == 42);
      CHECK(cell.byteClass == HexByteClass::SPACE);
    }
  };
  auto result = formatHexRow(input, 0, {0, 8, true, true}, text, cells);
  CHECK(result.error() == HexError::INVALID_OPTIONS);
  checkUnchanged();
  CHECK_FALSE(hexRowWidth({1, 0, true, true}));
  CHECK_FALSE(hexRowWidth({MAX_HEX_ROW_BYTES + 1, 1, true, true}));
  CHECK_FALSE(hexRowWidth({1, MAX_HEX_ROW_BYTES + 1, true, true}));
  result = formatHexRow(input, std::numeric_limits<std::uint64_t>::max() - 1, {}, text, cells);
  CHECK(result.error() == HexError::OFFSET_OVERFLOW);
  checkUnchanged();
  result = formatHexRow(input, 0, {}, std::span(text).first(1), cells);
  CHECK(result.error() == HexError::TEXT_TOO_SMALL);
  checkUnchanged();
  result = formatHexRow(input, 0, {}, text, std::span(cells).first(1));
  CHECK(result.error() == HexError::CELLS_TOO_SMALL);
  checkUnchanged();
}
