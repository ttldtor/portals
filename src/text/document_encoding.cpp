// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../../vendor/whatwg-encoding/windows_125x.hpp"

#include <algorithm>
#include <limits>
#include <portals/document_encoding.hpp>

namespace portals {
namespace {
using detail::whatwg::CP1251;
using detail::whatwg::CP1252;

struct Unit {
  char32_t scalar{};
  std::uint8_t length{};
  bool malformed{};
};

bool continuation(std::uint8_t byte) {
  return byte >= 0x80 && byte <= 0xbf;
}

Unit decode(const std::array<std::uint8_t, 4> &b, std::uint8_t n, DocumentEncoding enc, bool final) {
  if (!n)
    return {};
  if (enc == DocumentEncoding::WINDOWS1251 || enc == DocumentEncoding::WINDOWS1252) {
    return {b[0] < 0x80 ? char32_t(b[0])
                        : (enc == DocumentEncoding::WINDOWS1251 ? CP1251[b[0] - 0x80] : CP1252[b[0] - 0x80]),
            1, false};
  }
  if (enc == DocumentEncoding::UTF8) {
    auto first = b[0];
    if (first < 0x80)
      return {first, 1, false};
    int length = first >= 0xc2 && first <= 0xdf   ? 2
                 : first >= 0xe0 && first <= 0xef ? 3
                 : first >= 0xf0 && first <= 0xf4 ? 4
                                                  : 0;
    if (!length)
      return {0xfffd, 1, true};
    for (int i = 1; i < length; ++i) {
      if (i >= n)
        return final ? Unit{0xfffd, n, true} : Unit{};
      auto c = b[i];
      if (!continuation(c) || (i == 1 && ((first == 0xe0 && c < 0xa0) || (first == 0xed && c > 0x9f) ||
                                          (first == 0xf0 && c < 0x90) || (first == 0xf4 && c > 0x8f))))
        return {0xfffd, static_cast<std::uint8_t>(i), true};
    }
    char32_t scalar = first & ((1u << (7 - length)) - 1);
    for (int i = 1; i < length; ++i)
      scalar = (scalar << 6) | (b[i] & 0x3f);
    return {scalar, static_cast<std::uint8_t>(length), false};
  }
  bool little = enc == DocumentEncoding::UTF16LE || enc == DocumentEncoding::UTF32LE;
  bool utf16 = enc == DocumentEncoding::UTF16LE || enc == DocumentEncoding::UTF16BE;
  auto word = [&](int i) -> std::uint16_t {
    return little ? std::uint16_t(b[i] | (b[i + 1] << 8)) : std::uint16_t((b[i] << 8) | b[i + 1]);
  };
  int width = utf16 ? 2 : 4;
  if (n < width)
    return final ? Unit{0xfffd, n, true} : Unit{};
  if (!utf16) {
    std::uint32_t cp =
      little
        ? std::uint32_t(b[0]) | (std::uint32_t(b[1]) << 8) | (std::uint32_t(b[2]) << 16) | (std::uint32_t(b[3]) << 24)
        : (std::uint32_t(b[0]) << 24) | (std::uint32_t(b[1]) << 16) | (std::uint32_t(b[2]) << 8) | b[3];
    return cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ? Unit{0xfffd, 4, true} : Unit{char32_t(cp), 4, false};
  }
  auto a = word(0);
  if (a >= 0xdc00 && a <= 0xdfff)
    return {0xfffd, 2, true};
  if (a < 0xd800 || a > 0xdbff)
    return {a, 2, false};
  if (n < 4)
    return final ? Unit{0xfffd, 2, true} : Unit{};
  auto c = word(2);
  if (c < 0xdc00 || c > 0xdfff)
    return {0xfffd, 2, true};
  return {char32_t(0x10000 + ((a - 0xd800) << 10) + c - 0xdc00), 4, false};
}

struct Bom {
  DocumentEncoding encoding;
  std::array<std::uint8_t, 4> bytes;
  std::uint8_t size;
};

constexpr Bom BOMS[] = {{DocumentEncoding::UTF32LE, {0xff, 0xfe, 0, 0}, 4},
                        {DocumentEncoding::UTF32BE, {0, 0, 0xfe, 0xff}, 4},
                        {DocumentEncoding::UTF8, {0xef, 0xbb, 0xbf, 0}, 3},
                        {DocumentEncoding::UTF16LE, {0xff, 0xfe, 0, 0}, 2},
                        {DocumentEncoding::UTF16BE, {0xfe, 0xff, 0, 0}, 2}};
} // namespace

DocumentDecoder::DocumentDecoder(DocumentDecoderOptions options) noexcept : options_(options) {
}

std::optional<DocumentDecoderCheckpoint> DocumentDecoder::checkpoint() const noexcept {
  auto validChoice = [](DocumentEncoding value) {
    switch (value) {
    case DocumentEncoding::UNRESOLVED:
    case DocumentEncoding::UTF8:
    case DocumentEncoding::UTF16LE:
    case DocumentEncoding::UTF16BE:
    case DocumentEncoding::UTF32LE:
    case DocumentEncoding::UTF32BE:
    case DocumentEncoding::WINDOWS1251:
    case DocumentEncoding::WINDOWS1252:
      return true;
    default:
      return false;
    }
  };

  if (!validChoice(options_.explicitEncoding) || !validChoice(options_.hint)) {
    return std::nullopt;
  }

  if (!selected_ || pendingSize_ != 0 || done_ || options_.workLimit == 0 ||
      (options_.policy != DocumentDecodePolicy::REPLACE && options_.policy != DocumentDecodePolicy::STRICT_MODE)) {
    return std::nullopt;
  }

  switch (encoding_) {
  case DocumentEncoding::UTF8:
  case DocumentEncoding::UTF16LE:
  case DocumentEncoding::UTF16BE:
  case DocumentEncoding::UTF32LE:
  case DocumentEncoding::UTF32BE:
  case DocumentEncoding::WINDOWS1251:
  case DocumentEncoding::WINDOWS1252:
    return DocumentDecoderCheckpoint(options_, encoding_, nextOffset_, bomLength_, bomMismatch_);
  default:
    return std::nullopt;
  }
}

DocumentDecoder DocumentDecoder::restore(const DocumentDecoderCheckpoint &checkpoint) noexcept {
  DocumentDecoder decoder(checkpoint.options_);
  decoder.encoding_ = checkpoint.encoding_;
  decoder.nextOffset_ = checkpoint.nextOffset_;
  decoder.pendingOffset_ = checkpoint.nextOffset_;
  decoder.bomLength_ = checkpoint.bomLength_;
  decoder.bomMismatch_ = checkpoint.bomMismatch_;
  decoder.selected_ = true;

  return decoder;
}

DocumentDecodeResult DocumentDecoder::feed(std::span<const std::byte> input, std::uint64_t offset, bool final,
                                           std::span<DecodedScalar> output) noexcept {
  auto copy = *this;
  auto result = copy.feedImpl(input, offset, final, output);
  if (result.status != DocumentDecodeStatus::MALFORMED && result.status != DocumentDecodeStatus::ENCODING_UNRESOLVED &&
      result.status != DocumentDecodeStatus::INVALID_OFFSET &&
      result.status != DocumentDecodeStatus::UNSUPPORTED_POLICY) {
    *this = copy;
  } else {
    result.inputConsumed = 0;
    result.outputProduced = 0;
  }
  return result;
}

DocumentDecodeResult DocumentDecoder::feedImpl(std::span<const std::byte> input, std::uint64_t offset, bool final,
                                               std::span<DecodedScalar> output) noexcept {
  DocumentDecodeResult result{};
  auto finish = [&](DocumentDecodeStatus status) {
    result.status = status;
    return result;
  };
  if (options_.policy == DocumentDecodePolicy::ESCAPE || options_.workLimit == 0)
    return finish(DocumentDecodeStatus::UNSUPPORTED_POLICY);
  if (offset != nextOffset_ || input.size() > std::numeric_limits<std::uint64_t>::max() - offset || done_)
    return finish(DocumentDecodeStatus::INVALID_OFFSET);
  auto remove = [&](std::uint8_t count) {
    for (std::uint8_t i = count; i < pendingSize_; ++i)
      pending_[i - count] = pending_[i];
    pendingSize_ -= count;
    pendingOffset_ += count;
  };
  auto append = [&]() {
    if (!pendingSize_)
      pendingOffset_ = nextOffset_;
    pending_[pendingSize_++] = std::to_integer<std::uint8_t>(input[result.inputConsumed++]);
    ++nextOffset_;
  };
  while (true) {
    bool eof = final && result.inputConsumed == input.size();
    if (!selected_) {
      const Bom *match = nullptr;
      bool possibleLonger = false;
      for (const auto &bom : BOMS) {
        auto compared = std::min<int>(pendingSize_, bom.size);
        if (std::equal(pending_.begin(), pending_.begin() + compared, bom.bytes.begin())) {
          if (pendingSize_ >= bom.size && !match)
            match = &bom;
          if (pendingSize_ < bom.size)
            possibleLonger = true;
        }
      }
      if (possibleLonger && !eof && result.inputConsumed < input.size() && result.inputConsumed < options_.workLimit) {
        append();
        continue;
      }
      if (possibleLonger && !eof && result.inputConsumed == input.size())
        return finish(DocumentDecodeStatus::NEED_INPUT);
      if (possibleLonger && !eof)
        return finish(DocumentDecodeStatus::WORK_LIMIT);
      encoding_ = options_.explicitEncoding != DocumentEncoding::UNRESOLVED ? options_.explicitEncoding
                  : match                                                   ? match->encoding
                                                                            : options_.hint;
      if (encoding_ == DocumentEncoding::UNRESOLVED)
        return finish(DocumentDecodeStatus::ENCODING_UNRESOLVED);
      if (match) {
        bomMismatch_ = match->encoding != encoding_;
        if (!bomMismatch_) {
          bomLength_ = match->size;
          remove(match->size);
        }
      }
      selected_ = true;
    }
    eof = final && result.inputConsumed == input.size();
    auto unit = decode(pending_, pendingSize_, encoding_, eof);
    if (unit.length) {
      if (unit.malformed && options_.policy == DocumentDecodePolicy::STRICT_MODE) {
        result.errorOffset = pendingOffset_;
        result.errorLength = unit.length;
        return finish(DocumentDecodeStatus::MALFORMED);
      }
      if (result.outputProduced == output.size())
        return finish(DocumentDecodeStatus::OUTPUT_FULL);
      output[result.outputProduced++] = {unit.scalar, pendingOffset_, unit.length, unit.malformed};
      remove(unit.length);
      continue;
    }
    if (eof) {
      done_ = true;
      return finish(DocumentDecodeStatus::DONE);
    }
    if (result.outputProduced == output.size())
      return finish(DocumentDecodeStatus::OUTPUT_FULL);
    if (result.inputConsumed == input.size())
      return finish(DocumentDecodeStatus::NEED_INPUT);
    if (result.inputConsumed == options_.workLimit)
      return finish(DocumentDecodeStatus::WORK_LIMIT);
    append();
  }
}
} // namespace portals
