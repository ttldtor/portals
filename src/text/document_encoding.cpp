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
using LocalStatus = DocumentDecoderLocalStatus;

/// Fixed local proof outcome; a failure offset is deliberately not a guessed boundary.
struct LocalBoundary {
  std::uint64_t offset{};                 ///< Proven candidate start, meaningful only for READY.
  LocalStatus status{LocalStatus::READY}; ///< Boundary proof or missing context.
};

/// Read a byte already proven to lie inside the at-most-eight-byte local span.
std::uint8_t localByte(std::span<const std::byte> bytes, std::uint64_t offset, std::uint64_t position) noexcept {
  return std::to_integer<std::uint8_t>(bytes[static_cast<std::size_t>(position - offset)]);
}

/// Return a complete original UTF-16 unit; the caller proves two bytes are present.
std::uint16_t localWord(std::span<const std::byte> bytes, std::uint64_t offset, std::uint64_t position,
                        DocumentEncoding encoding) noexcept {
  const auto first = localByte(bytes, offset, position);
  const auto second = localByte(bytes, offset, position + 1);

  return encoding == DocumentEncoding::UTF16LE ? std::uint16_t(first | (second << 8))
                                               : std::uint16_t((first << 8) | second);
}

std::uint8_t codeUnitWidth(DocumentEncoding encoding) noexcept {
  if (encoding == DocumentEncoding::UTF16LE || encoding == DocumentEncoding::UTF16BE) {
    return 2;
  }

  if (encoding == DocumentEncoding::UTF32LE || encoding == DocumentEncoding::UTF32BE) {
    return 4;
  }

  return 1;
}

std::uint8_t encodingBomWidth(DocumentEncoding encoding) noexcept {
  if (encoding == DocumentEncoding::UTF8) {
    return 3;
  }

  const auto width = codeUnitWidth(encoding);

  return width == 1 ? 0 : width;
}

LocalBoundary utf8LocalBoundary(std::span<const std::byte> bytes, std::uint64_t offset, std::uint64_t anchor,
                                std::uint64_t contentStart, std::uint64_t originBoundary) noexcept {
  if (anchor == contentStart || anchor == originBoundary || !continuation(localByte(bytes, offset, anchor))) {
    return {anchor};
  }

  constexpr std::uint8_t MAX_LOOKBEHIND = 3;

  for (std::uint8_t distance = 1; distance <= MAX_LOOKBEHIND; ++distance) {
    if (distance > anchor - contentStart) {
      return {0, LocalStatus::NEED_PREFIX};
    }

    const auto position = anchor - distance;

    if (position < offset) {
      return {0, LocalStatus::NEED_PREFIX};
    }

    if (position == contentStart || position == originBoundary || !continuation(localByte(bytes, offset, position))) {
      return {position};
    }
  }

  // A lead before these three continuation bytes cannot also own the anchor:
  // every valid scalar and every malformed maximal subpart has at most four bytes.
  return {anchor};
}

LocalBoundary utf16LocalBoundary(std::span<const std::byte> bytes, std::uint64_t offset, std::uint64_t anchor,
                                 std::uint64_t contentStart, std::uint64_t originBoundary, DocumentEncoding encoding,
                                 bool final) noexcept {
  const auto start = anchor - anchor % 2;
  const auto end = offset + bytes.size();

  if (start < offset) {
    return {0, LocalStatus::NEED_PREFIX};
  }

  if (end - start < 2) {
    return final ? LocalBoundary{start} : LocalBoundary{0, LocalStatus::NEED_SUFFIX};
  }

  const auto current = localWord(bytes, offset, start, encoding);
  const bool lowSurrogate = current >= 0xdc00 && current <= 0xdfff;

  if (!lowSurrogate || start == contentStart || start == originBoundary) {
    return {start};
  }

  if (start < 2 || start - 2 < offset) {
    return {0, LocalStatus::NEED_PREFIX};
  }

  const auto previous = localWord(bytes, offset, start - 2, encoding);

  return {previous >= 0xd800 && previous <= 0xdbff ? start - 2 : start};
}

LocalBoundary localBoundary(std::span<const std::byte> bytes, std::uint64_t offset, std::uint64_t anchor,
                            std::uint64_t contentStart, std::uint64_t originBoundary, DocumentEncoding encoding,
                            bool final) noexcept {
  if (encoding == DocumentEncoding::UTF8) {
    return utf8LocalBoundary(bytes, offset, anchor, contentStart, originBoundary);
  }

  if (encoding == DocumentEncoding::UTF16LE || encoding == DocumentEncoding::UTF16BE) {
    return utf16LocalBoundary(bytes, offset, anchor, contentStart, originBoundary, encoding, final);
  }

  const auto width = codeUnitWidth(encoding);
  const auto start = anchor - anchor % width;

  return start < offset ? LocalBoundary{0, LocalStatus::NEED_PREFIX} : LocalBoundary{start};
}

/// Reuse the streaming decoder's exact scalar/malformed-subpart rules without retaining bytes.
Unit localUnit(std::span<const std::byte> bytes, std::uint64_t offset, std::uint64_t position,
               DocumentEncoding encoding, bool final) noexcept {
  std::array<std::uint8_t, 4> pending{};
  const auto size =
    static_cast<std::uint8_t>(std::min<std::uint64_t>(pending.size(), offset + bytes.size() - position));

  for (std::uint8_t i = 0; i < size; ++i) {
    pending[i] = localByte(bytes, offset, position + i);
  }

  return decode(pending, size, encoding, final);
}
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

DocumentDecoderLocalResult DocumentDecoder::localCheckpoint(const DocumentDecoderCheckpoint &origin,
                                                            std::span<const std::byte> window,
                                                            std::uint64_t windowOffset, std::uint64_t anchorByteOffset,
                                                            bool finalWindow) noexcept {
  DocumentDecoderLocalResult result{};
  const auto fail = [&](LocalStatus status) {
    result.status = status;

    return result;
  };

  // Reuse existing option/encoding validation; an opaque checkpoint is not an
  // authenticated handle, so also reject inconsistent representation metadata.
  if (!restore(origin).checkpoint()) {
    return fail(LocalStatus::INVALID_ORIGIN);
  }

  const auto &options = origin.options_;
  const auto encoding = origin.encoding_;
  const auto bomLength = origin.bomLength_;
  const auto width = codeUnitWidth(encoding);
  const bool explicitMismatch =
    options.explicitEncoding != DocumentEncoding::UNRESOLVED && options.explicitEncoding != encoding;
  const bool invalidBom = bomLength != 0 && bomLength != encodingBomWidth(encoding);
  const bool invalidMismatch =
    origin.bomMismatch_ && (bomLength != 0 || options.explicitEncoding == DocumentEncoding::UNRESOLVED);
  const bool invalidHint =
    options.explicitEncoding == DocumentEncoding::UNRESOLVED && bomLength == 0 && options.hint != encoding;

  if (explicitMismatch || invalidBom || invalidMismatch || invalidHint || origin.nextOffset_ < bomLength ||
      origin.nextOffset_ % width != 0) {
    return fail(LocalStatus::INVALID_ORIGIN);
  }

  if (window.size() > LOCAL_WINDOW_BYTES || window.size() > std::numeric_limits<std::uint64_t>::max() - windowOffset) {
    return fail(LocalStatus::INVALID_RANGE);
  }

  const auto end = windowOffset + window.size();

  if (anchorByteOffset < windowOffset || anchorByteOffset > end ||
      (finalWindow && (end < bomLength || end < origin.nextOffset_))) {
    return fail(LocalStatus::INVALID_RANGE);
  }

  const auto anchor = std::max(anchorByteOffset, std::uint64_t{bomLength});

  if (anchor > end) {
    return fail(LocalStatus::NEED_SUFFIX);
  }

  if (anchor == end) {
    if (finalWindow) {
      result.restartByteOffset = end;

      return fail(LocalStatus::AT_END);
    }

    return fail(LocalStatus::NEED_SUFFIX);
  }

  const auto boundary =
    localBoundary(window, windowOffset, anchor, bomLength, origin.nextOffset_, encoding, finalWindow);

  if (boundary.status != LocalStatus::READY) {
    return fail(boundary.status);
  }

  auto position = boundary.offset;

  for (;;) {
    const auto unit = localUnit(window, windowOffset, position, encoding, finalWindow);

    if (unit.length == 0) {
      return fail(LocalStatus::NEED_SUFFIX);
    }

    if (anchor - position < unit.length) {
      if (unit.malformed && options.policy == DocumentDecodePolicy::STRICT_MODE) {
        result.errorOffset = position;
        result.errorLength = unit.length;

        return fail(LocalStatus::MALFORMED);
      }

      result.restartByteOffset = position;
      result.checkpoint = DocumentDecoderCheckpoint(options, encoding, position, bomLength, origin.bomMismatch_);

      return fail(LocalStatus::READY);
    }

    // UTF-8 search begins at most three bytes before anchor; other encodings
    // already found its containing code unit/pair. Each positive unit advances.
    position += unit.length;
  }
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
