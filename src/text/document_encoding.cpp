// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
// Mapping data: WHATWG Encoding indexes dated 2024-09-18, identifiers
// 7592ef921679ba168b00a9e9afa3b4eebd67bf13dc7e84c4b6e120de856826e0
// and e56d49d9176e9a412283cf29ac9bd613f5620462f2a080a84eceaf974cfa18b7.
// https://encoding.spec.whatwg.org/ ; https://github.com/whatwg/encoding/blob/main/LICENSE
// Copyright (c) WHATWG (Apple, Google, Mozilla, Microsoft).
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#include <algorithm>
#include <limits>
#include <portals/document_encoding.hpp>

namespace portals {
namespace {
constexpr char32_t CP1251[] = {
  0x0402, 0x0403, 0x201a, 0x0453, 0x201e, 0x2026, 0x2020, 0x2021, 0x20ac, 0x2030, 0x0409, 0x2039, 0x040a,
  0x040c, 0x040b, 0x040f, 0x0452, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x0098, 0x2122,
  0x0459, 0x203a, 0x045a, 0x045c, 0x045b, 0x045f, 0x00a0, 0x040e, 0x045e, 0x0408, 0x00a4, 0x0490, 0x00a6,
  0x00a7, 0x0401, 0x00a9, 0x0404, 0x00ab, 0x00ac, 0x00ad, 0x00ae, 0x0407, 0x00b0, 0x00b1, 0x0406, 0x0456,
  0x0491, 0x00b5, 0x00b6, 0x00b7, 0x0451, 0x2116, 0x0454, 0x00bb, 0x0458, 0x0405, 0x0455, 0x0457, 0x0410,
  0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, 0x0418, 0x0419, 0x041a, 0x041b, 0x041c, 0x041d,
  0x041e, 0x041f, 0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429, 0x042a,
  0x042b, 0x042c, 0x042d, 0x042e, 0x042f, 0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,
  0x0438, 0x0439, 0x043a, 0x043b, 0x043c, 0x043d, 0x043e, 0x043f, 0x0440, 0x0441, 0x0442, 0x0443, 0x0444,
  0x0445, 0x0446, 0x0447, 0x0448, 0x0449, 0x044a, 0x044b, 0x044c, 0x044d, 0x044e, 0x044f};
constexpr char32_t CP1252[] = {
  0x20ac, 0x0081, 0x201a, 0x0192, 0x201e, 0x2026, 0x2020, 0x2021, 0x02c6, 0x2030, 0x0160, 0x2039, 0x0152,
  0x008d, 0x017d, 0x008f, 0x0090, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x02dc, 0x2122,
  0x0161, 0x203a, 0x0153, 0x009d, 0x017e, 0x0178, 0x00a0, 0x00a1, 0x00a2, 0x00a3, 0x00a4, 0x00a5, 0x00a6,
  0x00a7, 0x00a8, 0x00a9, 0x00aa, 0x00ab, 0x00ac, 0x00ad, 0x00ae, 0x00af, 0x00b0, 0x00b1, 0x00b2, 0x00b3,
  0x00b4, 0x00b5, 0x00b6, 0x00b7, 0x00b8, 0x00b9, 0x00ba, 0x00bb, 0x00bc, 0x00bd, 0x00be, 0x00bf, 0x00c0,
  0x00c1, 0x00c2, 0x00c3, 0x00c4, 0x00c5, 0x00c6, 0x00c7, 0x00c8, 0x00c9, 0x00ca, 0x00cb, 0x00cc, 0x00cd,
  0x00ce, 0x00cf, 0x00d0, 0x00d1, 0x00d2, 0x00d3, 0x00d4, 0x00d5, 0x00d6, 0x00d7, 0x00d8, 0x00d9, 0x00da,
  0x00db, 0x00dc, 0x00dd, 0x00de, 0x00df, 0x00e0, 0x00e1, 0x00e2, 0x00e3, 0x00e4, 0x00e5, 0x00e6, 0x00e7,
  0x00e8, 0x00e9, 0x00ea, 0x00eb, 0x00ec, 0x00ed, 0x00ee, 0x00ef, 0x00f0, 0x00f1, 0x00f2, 0x00f3, 0x00f4,
  0x00f5, 0x00f6, 0x00f7, 0x00f8, 0x00f9, 0x00fa, 0x00fb, 0x00fc, 0x00fd, 0x00fe, 0x00ff};

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
