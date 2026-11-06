// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <array>
#include <doctest.h>
#include <portals/document_encoding.hpp>
#include <span>

using namespace portals;

TEST_CASE("BOM and scalar survive every input split") {
  std::array<std::byte, 7> bytes{std::byte{0xef}, std::byte{0xbb}, std::byte{0xbf}, std::byte{0xf0},
                                 std::byte{0x9f}, std::byte{0x98}, std::byte{0x80}};
  for (std::size_t split = 0; split <= bytes.size(); ++split) {
    DocumentDecoder decoder;
    std::array<DecodedScalar, 8> out{};
    auto a = decoder.feed(std::span(bytes).first(split), 0, false, out);
    REQUIRE(a.status == DocumentDecodeStatus::NEED_INPUT);
    auto b = decoder.feed(std::span(bytes).subspan(split), split, true, std::span(out).subspan(a.outputProduced));
    REQUIRE(b.status == DocumentDecodeStatus::DONE);
    REQUIRE(a.outputProduced + b.outputProduced == 1);
    CHECK(out[0].scalar == 0x1f600);
    CHECK(out[0].byteOffset == 3);
    CHECK(out[0].byteLength == 4);
    CHECK(decoder.bomLength() == 3);
  }
}

TEST_CASE("UTF-8 maximal subparts survive splits") {
  std::array<std::byte, 3> bytes{std::byte{0xe1}, std::byte{0x80}, std::byte{0x41}};
  for (std::size_t split = 0; split <= bytes.size(); ++split) {
    DocumentDecoder decoder({DocumentEncoding::UTF8});
    std::array<DecodedScalar, 4> out{};
    auto a = decoder.feed(std::span(bytes).first(split), 0, false, out);
    auto b = decoder.feed(std::span(bytes).subspan(split), split, true, std::span(out).subspan(a.outputProduced));
    REQUIRE(b.status == DocumentDecodeStatus::DONE);
    REQUIRE(a.outputProduced + b.outputProduced == 2);
    CHECK(out[0].replacement);
    CHECK(out[0].byteOffset == 0);
    CHECK(out[0].byteLength == 2);
    CHECK(out[1].scalar == U'A');
  }
}

TEST_CASE("strict errors roll back the entire call") {
  DocumentDecoder decoder({DocumentEncoding::UTF8, DocumentEncoding::UNRESOLVED, DocumentDecodePolicy::STRICT_MODE});
  std::array<std::byte, 2> bad{std::byte{0x41}, std::byte{0xff}};
  std::array<DecodedScalar, 4> out{};
  auto first = decoder.feed(bad, 0, true, out);
  CHECK(first.status == DocumentDecodeStatus::MALFORMED);
  CHECK(first.inputConsumed == 0);
  CHECK(first.outputProduced == 0);
  CHECK(first.errorOffset == 1);
  auto retry = decoder.feed(std::span(bad).first(1), 0, true, out);
  CHECK(retry.status == DocumentDecodeStatus::DONE);
  CHECK(retry.outputProduced == 1);
}

TEST_CASE("output full retains data and zero capacity makes no lost progress") {
  DocumentDecoder decoder({DocumentEncoding::UTF8});
  std::array<std::byte, 2> bytes{std::byte{0x41}, std::byte{0x42}};
  std::array<DecodedScalar, 1> out{};
  auto zero = decoder.feed(bytes, 0, true, {});
  CHECK(zero.status == DocumentDecodeStatus::OUTPUT_FULL);
  auto a = decoder.feed(std::span(bytes).subspan(zero.inputConsumed), zero.inputConsumed, true, out);
  CHECK(a.status == DocumentDecodeStatus::OUTPUT_FULL);
  CHECK(out[0].scalar == U'A');
  auto used = zero.inputConsumed + a.inputConsumed;
  auto b = decoder.feed(std::span(bytes).subspan(used), used, true, out);
  CHECK(b.status == DocumentDecodeStatus::DONE);
  CHECK(out[0].scalar == U'B');
}

TEST_CASE("BOM precedence, unresolved and explicit mismatch") {
  std::array<std::byte, 4> bytes{std::byte{0xff}, std::byte{0xfe}, std::byte{0x00}, std::byte{0x00}};
  std::array<DecodedScalar, 8> out{};
  DocumentDecoder autoDecoder;
  auto a = autoDecoder.feed(bytes, 0, true, out);
  CHECK(a.status == DocumentDecodeStatus::DONE);
  CHECK(autoDecoder.encoding() == DocumentEncoding::UTF32LE);
  CHECK(autoDecoder.bomLength() == 4);
  DocumentDecoder mismatch({DocumentEncoding::WINDOWS1252});
  auto b = mismatch.feed(bytes, 0, true, out);
  CHECK(b.status == DocumentDecodeStatus::DONE);
  CHECK(mismatch.bomMismatch());
  CHECK(b.outputProduced == 4);
  DocumentDecoder unknown;
  auto c = unknown.feed(std::span(bytes).subspan(2, 1), 0, true, out);
  CHECK(c.status == DocumentDecodeStatus::ENCODING_UNRESOLVED);
  CHECK(c.inputConsumed == 0);
}

TEST_CASE("unsupported policy and offset fail without advancing") {
  DocumentDecoder decoder({DocumentEncoding::UTF8, DocumentEncoding::UNRESOLVED, DocumentDecodePolicy::ESCAPE});
  std::array<std::byte, 1> bytes{std::byte{0x41}};
  std::array<DecodedScalar, 1> out{};
  auto a = decoder.feed(bytes, 0, true, out);
  CHECK(a.status == DocumentDecodeStatus::UNSUPPORTED_POLICY);
  CHECK(a.inputConsumed == 0);
  DocumentDecoder other({DocumentEncoding::UTF8});
  auto b = other.feed(bytes, UINT64_MAX, true, out);
  CHECK(b.status == DocumentDecodeStatus::INVALID_OFFSET);
  auto c = other.feed(bytes, 0, true, out);
  CHECK(c.status == DocumentDecodeStatus::DONE);
}

TEST_CASE("UTF-16 and UTF-32 scalars survive every split") {
  struct Example {
    DocumentEncoding encoding;
    std::array<std::byte, 4> bytes;
  };

  const std::array examples{
    Example{DocumentEncoding::UTF16LE, {std::byte{0x3d}, std::byte{0xd8}, std::byte{0x00}, std::byte{0xde}}},
    Example{DocumentEncoding::UTF16BE, {std::byte{0xd8}, std::byte{0x3d}, std::byte{0xde}, std::byte{0x00}}},
    Example{DocumentEncoding::UTF32LE, {std::byte{0x00}, std::byte{0xf6}, std::byte{0x01}, std::byte{0x00}}},
    Example{DocumentEncoding::UTF32BE, {std::byte{0x00}, std::byte{0x01}, std::byte{0xf6}, std::byte{0x00}}}};
  for (auto example : examples) {
    for (std::size_t split = 0; split <= example.bytes.size(); ++split) {
      DocumentDecoder decoder({example.encoding});
      std::array<DecodedScalar, 4> out{};
      auto a = decoder.feed(std::span(example.bytes).first(split), 0, false, out);
      REQUIRE(a.status == DocumentDecodeStatus::NEED_INPUT);
      auto b =
        decoder.feed(std::span(example.bytes).subspan(split), split, true, std::span(out).subspan(a.outputProduced));
      REQUIRE(b.status == DocumentDecodeStatus::DONE);
      REQUIRE(a.outputProduced + b.outputProduced == 1);
      CHECK(out[0].scalar == 0x1f600);
      CHECK(out[0].byteOffset == 0);
      CHECK(out[0].byteLength == 4);
    }
  }
}

TEST_CASE("work budget yields and resumes") {
  DocumentDecoderOptions options;
  options.explicitEncoding = DocumentEncoding::UTF8;
  options.workLimit = 1;
  DocumentDecoder decoder(options);
  std::array<std::byte, 2> bytes{std::byte{0x41}, std::byte{0x42}};
  std::array<DecodedScalar, 4> out{};
  auto a = decoder.feed(bytes, 0, true, out);
  CHECK(a.status == DocumentDecodeStatus::WORK_LIMIT);
  CHECK(a.inputConsumed == 1);
  auto b = decoder.feed(std::span(bytes).subspan(1), 1, true, std::span(out).subspan(a.outputProduced));
  CHECK(b.status == DocumentDecodeStatus::DONE);
  CHECK(out[0].scalar == U'A');
  CHECK(out[1].scalar == U'B');
}

// Independent golden indexes from WHATWG Encoding, dated 2024-09-18.
// Identifiers: 7592ef921679ba168b00a9e9afa3b4eebd67bf13dc7e84c4b6e120de856826e0
// and e56d49d9176e9a412283cf29ac9bd613f5620462f2a080a84eceaf974cfa18b7.
// Copyright WHATWG (Apple, Google, Mozilla, Microsoft), BSD-3-Clause.
// Full license notice is in src/text/document_encoding.cpp.
TEST_CASE("WHATWG single-byte golden mappings") {
  constexpr std::array<char32_t, 128> EXPECTED1251{
    0x0402, 0x0403, 0x201A, 0x0453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x0409, 0x2039, 0x040A,
    0x040C, 0x040B, 0x040F, 0x0452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x0098, 0x2122,
    0x0459, 0x203A, 0x045A, 0x045C, 0x045B, 0x045F, 0x00A0, 0x040E, 0x045E, 0x0408, 0x00A4, 0x0490, 0x00A6,
    0x00A7, 0x0401, 0x00A9, 0x0404, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x0407, 0x00B0, 0x00B1, 0x0406, 0x0456,
    0x0491, 0x00B5, 0x00B6, 0x00B7, 0x0451, 0x2116, 0x0454, 0x00BB, 0x0458, 0x0405, 0x0455, 0x0457, 0x0410,
    0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417, 0x0418, 0x0419, 0x041A, 0x041B, 0x041C, 0x041D,
    0x041E, 0x041F, 0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427, 0x0428, 0x0429, 0x042A,
    0x042B, 0x042C, 0x042D, 0x042E, 0x042F, 0x0430, 0x0431, 0x0432, 0x0433, 0x0434, 0x0435, 0x0436, 0x0437,
    0x0438, 0x0439, 0x043A, 0x043B, 0x043C, 0x043D, 0x043E, 0x043F, 0x0440, 0x0441, 0x0442, 0x0443, 0x0444,
    0x0445, 0x0446, 0x0447, 0x0448, 0x0449, 0x044A, 0x044B, 0x044C, 0x044D, 0x044E, 0x044F};
  constexpr std::array<char32_t, 128> EXPECTED1252{
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152,
    0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122,
    0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178, 0x00A0, 0x00A1, 0x00A2, 0x00A3, 0x00A4, 0x00A5, 0x00A6,
    0x00A7, 0x00A8, 0x00A9, 0x00AA, 0x00AB, 0x00AC, 0x00AD, 0x00AE, 0x00AF, 0x00B0, 0x00B1, 0x00B2, 0x00B3,
    0x00B4, 0x00B5, 0x00B6, 0x00B7, 0x00B8, 0x00B9, 0x00BA, 0x00BB, 0x00BC, 0x00BD, 0x00BE, 0x00BF, 0x00C0,
    0x00C1, 0x00C2, 0x00C3, 0x00C4, 0x00C5, 0x00C6, 0x00C7, 0x00C8, 0x00C9, 0x00CA, 0x00CB, 0x00CC, 0x00CD,
    0x00CE, 0x00CF, 0x00D0, 0x00D1, 0x00D2, 0x00D3, 0x00D4, 0x00D5, 0x00D6, 0x00D7, 0x00D8, 0x00D9, 0x00DA,
    0x00DB, 0x00DC, 0x00DD, 0x00DE, 0x00DF, 0x00E0, 0x00E1, 0x00E2, 0x00E3, 0x00E4, 0x00E5, 0x00E6, 0x00E7,
    0x00E8, 0x00E9, 0x00EA, 0x00EB, 0x00EC, 0x00ED, 0x00EE, 0x00EF, 0x00F0, 0x00F1, 0x00F2, 0x00F3, 0x00F4,
    0x00F5, 0x00F6, 0x00F7, 0x00F8, 0x00F9, 0x00FA, 0x00FB, 0x00FC, 0x00FD, 0x00FE, 0x00FF};
  for (auto encoding : {DocumentEncoding::WINDOWS1251, DocumentEncoding::WINDOWS1252}) {
    for (unsigned byte = 0; byte < 256; ++byte) {
      DocumentDecoder decoder({encoding});
      std::array input{std::byte(byte)};
      std::array<DecodedScalar, 2> output{};
      auto result = decoder.feed(input, 0, true, output);
      REQUIRE(result.status == DocumentDecodeStatus::DONE);
      REQUIRE(result.outputProduced == 1);
      auto expected =
        byte < 128 ? char32_t(byte)
                   : (encoding == DocumentEncoding::WINDOWS1251 ? EXPECTED1251[byte - 128] : EXPECTED1252[byte - 128]);
      CHECK(output[0].scalar == expected);
      CHECK(output[0].byteOffset == 0);
      CHECK(output[0].byteLength == 1);
      CHECK_FALSE(output[0].replacement);
    }
  }
}

TEST_CASE("malformed UTF maximal subparts are partition independent") {
  struct Example {
    DocumentEncoding encoding;
    std::array<std::byte, 4> bytes;
    std::size_t size;
    std::array<unsigned, 4> lengths;
    std::size_t count;
  };

  const std::array examples{
    Example{
      DocumentEncoding::UTF8, {std::byte{0xe0}, std::byte{0x80}, std::byte{0x80}, std::byte{}}, 3, {1, 1, 1, 0}, 3},
    Example{
      DocumentEncoding::UTF8, {std::byte{0xed}, std::byte{0xa0}, std::byte{0x80}, std::byte{}}, 3, {1, 1, 1, 0}, 3},
    Example{
      DocumentEncoding::UTF8, {std::byte{0xf0}, std::byte{0x9f}, std::byte{0x98}, std::byte{}}, 3, {3, 0, 0, 0}, 1},
    Example{DocumentEncoding::UTF8, {std::byte{0xc0}, std::byte{0x80}, std::byte{}, std::byte{}}, 2, {1, 1, 0, 0}, 2},
    Example{DocumentEncoding::UTF16LE,
            {std::byte{0x00}, std::byte{0xd8}, std::byte{0x41}, std::byte{0x00}},
            4,
            {2, 2, 0, 0},
            2},
    Example{DocumentEncoding::UTF16BE,
            {std::byte{0xd8}, std::byte{0x00}, std::byte{0x00}, std::byte{0x41}},
            4,
            {2, 2, 0, 0},
            2},
    Example{
      DocumentEncoding::UTF16LE, {std::byte{0x00}, std::byte{0xd8}, std::byte{0x41}, std::byte{}}, 3, {2, 1, 0, 0}, 2},
    Example{
      DocumentEncoding::UTF16BE, {std::byte{0xdc}, std::byte{0x00}, std::byte{}, std::byte{}}, 2, {2, 0, 0, 0}, 1},
    Example{DocumentEncoding::UTF32LE,
            {std::byte{0x00}, std::byte{0xd8}, std::byte{0x00}, std::byte{0x00}},
            4,
            {4, 0, 0, 0},
            1},
    Example{DocumentEncoding::UTF32BE,
            {std::byte{0x00}, std::byte{0x11}, std::byte{0x00}, std::byte{0x00}},
            4,
            {4, 0, 0, 0},
            1},
    Example{
      DocumentEncoding::UTF32BE, {std::byte{0x00}, std::byte{0x01}, std::byte{0xf6}, std::byte{}}, 3, {3, 0, 0, 0}, 1}};
  for (auto example : examples) {
    for (std::size_t split = 0; split <= example.size; ++split) {
      DocumentDecoder decoder({example.encoding});
      std::array<DecodedScalar, 8> output{};
      auto input = std::span(example.bytes).first(example.size);
      auto a = decoder.feed(input.first(split), 0, false, output);
      REQUIRE(a.status == DocumentDecodeStatus::NEED_INPUT);
      auto b = decoder.feed(input.subspan(split), split, true, std::span(output).subspan(a.outputProduced));
      REQUIRE(b.status == DocumentDecodeStatus::DONE);
      REQUIRE(a.outputProduced + b.outputProduced == example.count);
      std::uint64_t offset = 0;
      for (std::size_t i = 0; i < example.count; ++i) {
        CHECK(output[i].byteOffset == offset);
        CHECK(output[i].byteLength == example.lengths[i]);
        offset += example.lengths[i];
      }
      CHECK(output[0].replacement);
    }
  }
}

TEST_CASE("strict truncated UTF rolls back at every split") {
  struct Example {
    DocumentEncoding encoding;
    std::array<std::byte, 4> bytes;
    unsigned size;
  };

  const std::array examples{
    Example{DocumentEncoding::UTF8, {std::byte{0xf0}, std::byte{0x9f}, std::byte{0x98}, std::byte{}}, 3},
    Example{DocumentEncoding::UTF16LE, {std::byte{0x00}, std::byte{0xd8}, std::byte{}, std::byte{}}, 2},
    Example{DocumentEncoding::UTF16BE, {std::byte{0xd8}, std::byte{0x00}, std::byte{}, std::byte{}}, 2},
    Example{DocumentEncoding::UTF32LE, {std::byte{0x00}, std::byte{0xf6}, std::byte{0x01}, std::byte{}}, 3},
    Example{DocumentEncoding::UTF32BE, {std::byte{0x00}, std::byte{0x01}, std::byte{0xf6}, std::byte{}}, 3}};
  for (auto example : examples) {
    for (std::size_t split = 0; split <= example.size; ++split) {
      DocumentDecoder decoder({example.encoding, DocumentEncoding::UNRESOLVED, DocumentDecodePolicy::STRICT_MODE});
      auto input = std::span(example.bytes).first(example.size);
      std::array<DecodedScalar, 4> output{};
      auto a = decoder.feed(input.first(split), 0, false, output);
      REQUIRE(a.status == DocumentDecodeStatus::NEED_INPUT);
      auto b = decoder.feed(input.subspan(split), split, true, output);
      CHECK(b.status == DocumentDecodeStatus::MALFORMED);
      CHECK(b.inputConsumed == 0);
      CHECK(b.outputProduced == 0);
      CHECK(b.errorOffset == 0);
      CHECK(b.errorLength == example.size);
      auto again = decoder.feed(input.subspan(split), split, true, output);
      CHECK(again.status == DocumentDecodeStatus::MALFORMED);
      CHECK(again.errorOffset == 0);
    }
  }
}

TEST_CASE("noncharacters are preserved as scalars") {
  DocumentDecoder decoder({DocumentEncoding::UTF8});
  std::array bytes{std::byte{0xef}, std::byte{0xb7}, std::byte{0x90}};
  std::array<DecodedScalar, 1> output{};
  auto result = decoder.feed(bytes, 0, true, output);
  CHECK(result.status == DocumentDecodeStatus::DONE);
  CHECK(output[0].scalar == 0xfdd0);
  CHECK_FALSE(output[0].replacement);
}

TEST_CASE("checkpoint resumes each encoding at a decoded boundary") {
  struct Example {
    DocumentEncoding encoding;
    std::array<std::byte, 8> bytes;
    unsigned unit;
  };

  const std::array examples{
    Example{DocumentEncoding::UTF8, {std::byte{0x41}, std::byte{0x42}}, 1},
    Example{DocumentEncoding::UTF16LE, {std::byte{0x41}, std::byte{0}, std::byte{0x42}, std::byte{0}}, 2},
    Example{DocumentEncoding::UTF16BE, {std::byte{0}, std::byte{0x41}, std::byte{0}, std::byte{0x42}}, 2},
    Example{DocumentEncoding::UTF32LE,
            {std::byte{0x41}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0x42}, std::byte{0}, std::byte{0},
             std::byte{0}},
            4},
    Example{DocumentEncoding::UTF32BE,
            {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0x41}, std::byte{0}, std::byte{0}, std::byte{0},
             std::byte{0x42}},
            4},
    Example{DocumentEncoding::WINDOWS1251, {std::byte{0x41}, std::byte{0x42}}, 1},
    Example{DocumentEncoding::WINDOWS1252, {std::byte{0x41}, std::byte{0x42}}, 1}};
  for (auto example : examples) {
    DocumentDecoder decoder({example.encoding});
    CHECK_FALSE(decoder.checkpoint().has_value());
    std::array<DecodedScalar, 4> out{};
    auto data = std::span(example.bytes).first(example.unit * 2);
    auto first = decoder.feed(data.first(example.unit), 0, false, out);
    REQUIRE(first.status == DocumentDecodeStatus::NEED_INPUT);
    auto snapshot = decoder.checkpoint();
    REQUIRE(snapshot.has_value());
    auto restored = DocumentDecoder::restore(*snapshot);
    std::array<DecodedScalar, 4> resumed{};
    auto a = decoder.feed(data.subspan(example.unit), example.unit, true, out);
    auto wrong = restored.feed(data.subspan(example.unit), 0, true, resumed);
    CHECK(wrong.status == DocumentDecodeStatus::INVALID_OFFSET);
    auto b = restored.feed(data.subspan(example.unit), example.unit, true, resumed);
    REQUIRE(a.status == DocumentDecodeStatus::DONE);
    REQUIRE(b.status == DocumentDecodeStatus::DONE);
    CHECK(a.outputProduced == b.outputProduced);
    CHECK(out[0].scalar == resumed[0].scalar);
    CHECK(out[0].byteOffset == resumed[0].byteOffset);
    CHECK(out[0].byteLength == resumed[0].byteLength);
    CHECK_FALSE(decoder.checkpoint().has_value());
  }
}

TEST_CASE("checkpoint rejects pending and restored stream does not probe BOM") {
  DocumentDecoder decoder({DocumentEncoding::UTF8});
  std::array<DecodedScalar, 4> out{};
  std::array first{std::byte{0x41}};
  auto a = decoder.feed(first, 0, false, out);
  REQUIRE(a.status == DocumentDecodeStatus::NEED_INPUT);
  auto snapshot = decoder.checkpoint();
  REQUIRE(snapshot.has_value());
  std::array bom{std::byte{0xef}, std::byte{0xbb}, std::byte{0xbf}};
  auto pending = decoder.feed(std::span(bom).first(1), 1, false, out);
  CHECK(pending.status == DocumentDecodeStatus::NEED_INPUT);
  CHECK_FALSE(decoder.checkpoint().has_value());
  auto resumed = DocumentDecoder::restore(*snapshot);
  auto b = resumed.feed(bom, 1, true, out);
  CHECK(b.status == DocumentDecodeStatus::DONE);
  CHECK(b.outputProduced == 1);
  CHECK(out[0].scalar == 0xfeff);
  CHECK(out[0].byteOffset == 1);
  CHECK(resumed.bomLength() == 0);
}

TEST_CASE("checkpoint preserves initial BOM metadata and rejects invalid options") {
  std::array bomAndA{std::byte{0xef}, std::byte{0xbb}, std::byte{0xbf}, std::byte{0x41}};
  std::array<DecodedScalar, 8> output{};
  DocumentDecoder decoder;
  auto a = decoder.feed(bomAndA, 0, false, output);
  REQUIRE(a.status == DocumentDecodeStatus::NEED_INPUT);
  auto checkpoint = decoder.checkpoint();
  REQUIRE(checkpoint.has_value());
  auto restored = DocumentDecoder::restore(*checkpoint);
  CHECK(restored.bomLength() == 3);
  CHECK(restored.encoding() == DocumentEncoding::UTF8);
  CHECK_FALSE(restored.bomMismatch());
  DocumentDecoderOptions invalid;
  invalid.explicitEncoding = DocumentEncoding::UTF8;
  invalid.hint = static_cast<DocumentEncoding>(99);
  DocumentDecoder malformed(invalid);
  auto b = malformed.feed(std::span(bomAndA).last(1), 0, false, output);
  REQUIRE(b.status == DocumentDecodeStatus::NEED_INPUT);
  CHECK_FALSE(malformed.checkpoint().has_value());
}

TEST_CASE("non-BOM prefix is emitted without waiting for further input") {
  DocumentDecoder decoder({DocumentEncoding::UTF8});
  std::array bytes{std::byte{0x41}};
  std::array<DecodedScalar, 2> output{};
  auto result = decoder.feed(bytes, 0, false, output);
  CHECK(result.status == DocumentDecodeStatus::NEED_INPUT);
  CHECK(result.inputConsumed == 1);
  REQUIRE(result.outputProduced == 1);
  CHECK(output[0].scalar == U'A');
  CHECK(decoder.checkpoint().has_value());
}

TEST_CASE("BOM selection waits only for ambiguous prefixes") {
  std::array<DecodedScalar, 4> output{};
  DocumentDecoder bigEndian;
  std::array utf16be{std::byte{0xfe}, std::byte{0xff}};
  auto a = bigEndian.feed(utf16be, 0, false, output);
  CHECK(a.status == DocumentDecodeStatus::NEED_INPUT);
  CHECK(a.inputConsumed == 2);
  CHECK(bigEndian.encoding() == DocumentEncoding::UTF16BE);
  CHECK(bigEndian.bomLength() == 2);
  CHECK(bigEndian.checkpoint().has_value());

  DocumentDecoder littleEndian;
  std::array prefix{std::byte{0xff}, std::byte{0xfe}};
  auto b = littleEndian.feed(prefix, 0, false, output);
  CHECK(b.status == DocumentDecodeStatus::NEED_INPUT);
  CHECK(littleEndian.encoding() == DocumentEncoding::UNRESOLVED);
  CHECK_FALSE(littleEndian.checkpoint().has_value());
  std::array next{std::byte{0x41}};
  auto c = littleEndian.feed(next, 2, false, output);
  CHECK(c.status == DocumentDecodeStatus::NEED_INPUT);
  CHECK(littleEndian.encoding() == DocumentEncoding::UTF16LE);
  CHECK(littleEndian.bomLength() == 2);
  CHECK_FALSE(littleEndian.checkpoint().has_value());

  DocumentDecoder utf8;
  std::array utf8bom{std::byte{0xef}, std::byte{0xbb}, std::byte{0xbf}};
  auto d = utf8.feed(utf8bom, 0, false, output);
  CHECK(d.status == DocumentDecodeStatus::NEED_INPUT);
  CHECK(utf8.encoding() == DocumentEncoding::UTF8);
  CHECK(utf8.bomLength() == 3);
  CHECK(utf8.checkpoint().has_value());
}

TEST_CASE("exactly full output reports output full after producing the scalar") {
  DocumentDecoder decoder({DocumentEncoding::UTF8});
  std::array input{std::byte{0x41}};
  std::array<DecodedScalar, 1> output{};
  auto result = decoder.feed(input, 0, false, output);
  CHECK(result.status == DocumentDecodeStatus::OUTPUT_FULL);
  CHECK(result.inputConsumed == 1);
  REQUIRE(result.outputProduced == 1);
  CHECK(output[0].scalar == U'A');
  CHECK(decoder.checkpoint().has_value());
}
