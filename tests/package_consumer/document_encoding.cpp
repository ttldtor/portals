// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/document_encoding.hpp>

#include <array>
#include <cstddef>

int main() {
  using namespace portals;
  DocumentDecoder decoder({DocumentEncoding::UTF8});
  std::array first{std::byte{0x41}};
  std::array<DecodedScalar, 2> output{};
  auto prefix = decoder.feed(first, 0, false, output);
  if (prefix.status != DocumentDecodeStatus::NEED_INPUT || prefix.outputProduced != 1 || output[0].scalar != U'A') {
    return 1;
  }
  auto checkpoint = decoder.checkpoint();
  if (!checkpoint) {
    return 2;
  }
  auto resumed = DocumentDecoder::restore(*checkpoint);
  std::array second{std::byte{0x42}};
  auto result = resumed.feed(second, 1, true, output);
  return result.status == DocumentDecodeStatus::DONE && result.outputProduced == 1 && output[0].scalar == U'B' &&
             output[0].byteOffset == 1
           ? 0
           : 3;
}
