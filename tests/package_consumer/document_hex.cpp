// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <array>
#include <portals/document_hex.hpp>

int main() {
  std::array input{std::byte{0}, std::byte{0x41}, std::byte{0xff}};
  std::array<char, 128> text{};
  std::array<portals::HexByteCell, 3> cells{};
  const auto result = portals::formatHexRow(input, 0x100000010ULL, {}, text, cells);
  return result && result->bytesConsumed == 3 && cells[2].byteOffset == 0x100000012ULL ? 0 : 1;
}
