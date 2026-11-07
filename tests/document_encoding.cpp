// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "fixtures/encoding/windows_125x_expected.hpp"
#include <array>
#include <bit>
#include <doctest.h>
#include <portals/document_encoding.hpp>
#include <span>
#include <type_traits>
#include <vector>

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

TEST_CASE("WHATWG single-byte golden mappings") {
  constexpr auto &EXPECTED_1251 = document_encoding_fixture::EXPECTED1251;
  constexpr auto &EXPECTED_1252 = document_encoding_fixture::EXPECTED1252;
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
                   : (encoding == DocumentEncoding::WINDOWS1251 ? EXPECTED_1251[byte - 128] : EXPECTED_1252[byte - 128]);
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

namespace {

using LocalStatus = DocumentDecoderLocalStatus;

/// Independent raw corpus and annotated original-byte scalar intervals.
struct LocalCorpus {
  DocumentEncoding encoding{};         ///< Expected effective encoding.
  std::vector<std::byte> bytes;        ///< Original source bytes, including any BOM.
  std::vector<DecodedScalar> expected; ///< Independently annotated replacement-mode scalars.
  std::size_t originBytes{};           ///< Real source prefix ending after the first valid scalar.
  DocumentDecoderOptions options{};    ///< Source selection before malformed-policy substitution.
  std::uint8_t bomLength{};            ///< Expected omitted BOM size.
  bool bomMismatch{};                  ///< Expected explicit-choice/BOM conflict.
};

std::vector<std::byte> raw(std::initializer_list<unsigned> values) {
  std::vector<std::byte> result;

  for (const auto value : values) {
    result.push_back(static_cast<std::byte>(value));
  }

  return result;
}

std::vector<LocalCorpus> localCorpora() {
  std::vector<LocalCorpus> result{
    {DocumentEncoding::UTF8,
     raw({0x41, 0xe2, 0x82, 0xac, 0xf0, 0x9f, 0x98, 0x80, 0xe1, 0x80,
          0x42, 0x80, 0xed, 0xa0, 0x80, 0xef, 0xbb, 0xbf, 0xf0, 0x9f}),
     {{U'A', 0, 1, false},
      {0x20ac, 1, 3, false},
      {0x1f600, 4, 4, false},
      {0xfffd, 8, 2, true},
      {U'B', 10, 1, false},
      {0xfffd, 11, 1, true},
      {0xfffd, 12, 1, true},
      {0xfffd, 13, 1, true},
      {0xfffd, 14, 1, true},
      {0xfeff, 15, 3, false},
      {0xfffd, 18, 2, true}},
     1},
    {DocumentEncoding::UTF16LE,
     raw({0x41, 0, 0x3d, 0xd8, 0, 0xde, 0, 0xd8, 0x42, 0, 0, 0xdc, 0xff, 0xfe, 0x3d, 0xd8, 0}),
     {{U'A', 0, 2, false},
      {0x1f600, 2, 4, false},
      {0xfffd, 6, 2, true},
      {U'B', 8, 2, false},
      {0xfffd, 10, 2, true},
      {0xfeff, 12, 2, false},
      {0xfffd, 14, 2, true},
      {0xfffd, 16, 1, true}},
     2},
    {DocumentEncoding::UTF16BE,
     raw({0, 0x41, 0xd8, 0x3d, 0xde, 0, 0xd8, 0, 0, 0x42, 0xdc, 0, 0xfe, 0xff, 0xd8, 0x3d, 0}),
     {{U'A', 0, 2, false},
      {0x1f600, 2, 4, false},
      {0xfffd, 6, 2, true},
      {U'B', 8, 2, false},
      {0xfffd, 10, 2, true},
      {0xfeff, 12, 2, false},
      {0xfffd, 14, 2, true},
      {0xfffd, 16, 1, true}},
     2},
    {DocumentEncoding::UTF32LE,
     raw({0x41, 0, 0, 0, 0, 0xf6, 1, 0, 0, 0xd8, 0, 0, 0, 0, 0x11, 0, 0xff, 0xfe, 0, 0, 0x42, 0, 0}),
     {{U'A', 0, 4, false},
      {0x1f600, 4, 4, false},
      {0xfffd, 8, 4, true},
      {0xfffd, 12, 4, true},
      {0xfeff, 16, 4, false},
      {0xfffd, 20, 3, true}},
     4},
    {DocumentEncoding::UTF32BE,
     raw({0, 0, 0, 0x41, 0, 1, 0xf6, 0, 0, 0, 0xd8, 0, 0, 0x11, 0, 0, 0, 0, 0xfe, 0xff, 0, 0, 0x42}),
     {{U'A', 0, 4, false},
      {0x1f600, 4, 4, false},
      {0xfffd, 8, 4, true},
      {0xfffd, 12, 4, true},
      {0xfeff, 16, 4, false},
      {0xfffd, 20, 3, true}},
     4},
    {DocumentEncoding::WINDOWS1251,
     raw({0x41, 0x98, 0xc0, 0xff}),
     {{U'A', 0, 1, false}, {0x98, 1, 1, false}, {0x410, 2, 1, false}, {0x44f, 3, 1, false}},
     1},
    {DocumentEncoding::WINDOWS1252,
     raw({0x41, 0x81, 0x80, 0xff}),
     {{U'A', 0, 1, false}, {0x81, 1, 1, false}, {0x20ac, 2, 1, false}, {0xff, 3, 1, false}},
     1}};

  for (auto &corpus : result) {
    corpus.options.explicitEncoding = corpus.encoding;
  }

  const std::array boms{raw({0xef, 0xbb, 0xbf}), raw({0xff, 0xfe}), raw({0xfe, 0xff}), raw({0xff, 0xfe, 0, 0}),
                        raw({0, 0, 0xfe, 0xff})};

  for (std::size_t i = 0; i < boms.size(); ++i) {
    auto corpus = result[i];

    corpus.bytes.insert(corpus.bytes.begin(), boms[i].begin(), boms[i].end());
    corpus.bomLength = static_cast<std::uint8_t>(boms[i].size());
    corpus.originBytes += corpus.bomLength;
    corpus.options.explicitEncoding = DocumentEncoding::UNRESOLVED;
    corpus.options.hint = DocumentEncoding::WINDOWS1252;

    for (auto &scalar : corpus.expected) {
      scalar.byteOffset += corpus.bomLength;
    }

    result.push_back(std::move(corpus));
  }

  return result;
}

void checkScalar(const DecodedScalar &actual, const DecodedScalar &expected) {
  CHECK(actual.scalar == expected.scalar);
  CHECK(actual.byteOffset == expected.byteOffset);
  CHECK(actual.byteLength == expected.byteLength);
  CHECK(actual.replacement == expected.replacement);
}

DocumentDecoderCheckpoint realOrigin(const LocalCorpus &corpus, DocumentDecodePolicy policy,
                                     std::size_t workLimit = 65536) {
  auto options = corpus.options;

  options.policy = policy;
  DocumentDecoder decoder(options);
  std::array<DecodedScalar, 16> output{};
  const auto result = decoder.feed(std::span(corpus.bytes).first(corpus.originBytes), 0, false, output);

  REQUIRE(result.status == DocumentDecodeStatus::NEED_INPUT);
  REQUIRE(result.inputConsumed == corpus.originBytes);
  const auto checkpoint = decoder.checkpoint();
  REQUIRE(checkpoint.has_value());

  if (workLimit == options.workLimit) {
    return *checkpoint;
  }

  options.workLimit = workLimit;
  DocumentDecoder bounded(options);
  std::size_t consumed = 0;

  while (consumed < corpus.originBytes) {
    const auto step =
      bounded.feed(std::span(corpus.bytes).first(corpus.originBytes).subspan(consumed), consumed, false, output);

    REQUIRE((step.status == DocumentDecodeStatus::WORK_LIMIT || step.status == DocumentDecodeStatus::NEED_INPUT));
    REQUIRE(step.inputConsumed != 0);
    consumed += step.inputConsumed;
  }

  REQUIRE(bounded.checkpoint().has_value());

  return *bounded.checkpoint();
}

const DecodedScalar *containing(const LocalCorpus &corpus, std::uint64_t anchor) {
  const auto effective = std::max(anchor, std::uint64_t{corpus.bomLength});

  for (const auto &scalar : corpus.expected) {
    if (scalar.byteOffset <= effective && effective - scalar.byteOffset < scalar.byteLength) {
      return &scalar;
    }
  }

  return nullptr;
}

void checkLocalProof(const LocalCorpus &corpus, const DocumentDecoderLocalResult &proof,
                     const DecodedScalar &expected) {
  REQUIRE(proof.status == LocalStatus::READY);
  REQUIRE(proof.checkpoint.has_value());
  CHECK(proof.restartByteOffset == expected.byteOffset);
  auto resumed = DocumentDecoder::restore(*proof.checkpoint);

  CHECK(resumed.encoding() == corpus.encoding);
  CHECK(resumed.bomLength() == corpus.bomLength);
  CHECK(resumed.bomMismatch() == corpus.bomMismatch);
  std::array<DecodedScalar, 1> output{};
  const auto result = resumed.feed(std::span(corpus.bytes).subspan(static_cast<std::size_t>(proof.restartByteOffset)),
                                   proof.restartByteOffset, true, output);

  REQUIRE((result.status == DocumentDecodeStatus::OUTPUT_FULL || result.status == DocumentDecodeStatus::DONE));
  REQUIRE(result.outputProduced == 1);
  checkScalar(output[0], expected);
}

} // namespace

TEST_CASE("local restart canonical windows match independent scalar and replacement goldens") {
  for (const auto &corpus : localCorpora()) {
    DocumentDecoder sequential(corpus.options);
    std::array<DecodedScalar, 64> oracle{};
    const auto decoded = sequential.feed(corpus.bytes, 0, true, oracle);

    REQUIRE(decoded.status == DocumentDecodeStatus::DONE);
    REQUIRE(decoded.outputProduced == corpus.expected.size());

    for (std::size_t i = 0; i < corpus.expected.size(); ++i) {
      checkScalar(oracle[i], corpus.expected[i]);
    }

    for (const auto policy : {DocumentDecodePolicy::REPLACE, DocumentDecodePolicy::STRICT_MODE}) {
      const auto origin = realOrigin(corpus, policy);

      for (std::size_t anchor = 0; anchor <= corpus.bytes.size(); ++anchor) {
        const auto begin = anchor > 3 ? anchor - 3 : 0;
        const auto length = std::min(DocumentDecoder::LOCAL_WINDOW_BYTES, corpus.bytes.size() - begin);
        const bool eof = begin + length == corpus.bytes.size();
        const auto proof =
          DocumentDecoder::localCheckpoint(origin, std::span(corpus.bytes).subspan(begin, length), begin, anchor, eof);
        const auto *expected = containing(corpus, anchor);

        CAPTURE(anchor);
        CAPTURE(begin);
        CAPTURE(corpus.encoding);
        CAPTURE(corpus.bomLength);
        CAPTURE(policy);

        if (!expected) {
          CHECK(proof.status == LocalStatus::AT_END);
          CHECK_FALSE(proof.checkpoint.has_value());
          CHECK(proof.restartByteOffset == corpus.bytes.size());
        } else if (expected->replacement && policy == DocumentDecodePolicy::STRICT_MODE) {
          CHECK(proof.status == LocalStatus::MALFORMED);
          CHECK_FALSE(proof.checkpoint.has_value());
          CHECK(proof.errorOffset == expected->byteOffset);
          CHECK(proof.errorLength == expected->byteLength);
        } else {
          checkLocalProof(corpus, proof, *expected);
        }
      }
    }
  }
}

TEST_CASE("all small local windows either prove the original interval or request context") {
  for (const auto &corpus : localCorpora()) {
    const auto origin = realOrigin(corpus, DocumentDecodePolicy::REPLACE);

    for (std::size_t anchor = 0; anchor <= corpus.bytes.size(); ++anchor) {
      const auto first =
        anchor > DocumentDecoder::LOCAL_WINDOW_BYTES ? anchor - DocumentDecoder::LOCAL_WINDOW_BYTES : 0;

      for (auto begin = first; begin <= anchor; ++begin) {
        const auto last = std::min(corpus.bytes.size(), begin + DocumentDecoder::LOCAL_WINDOW_BYTES);

        for (auto end = anchor; end <= last; ++end) {
          const auto proof = DocumentDecoder::localCheckpoint(
            origin, std::span(corpus.bytes).subspan(begin, end - begin), begin, anchor, end == corpus.bytes.size());
          const auto *expected = containing(corpus, anchor);

          CAPTURE(corpus.encoding);
          CAPTURE(corpus.bomLength);
          CAPTURE(anchor);
          CAPTURE(begin);
          CAPTURE(end);

          if (proof.status == LocalStatus::READY) {
            REQUIRE(expected != nullptr);
            checkLocalProof(corpus, proof, *expected);
          } else if (proof.status == LocalStatus::AT_END) {
            CHECK(expected == nullptr);
            CHECK_FALSE(proof.checkpoint.has_value());
            CHECK(proof.restartByteOffset == corpus.bytes.size());
          } else {
            CHECK((proof.status == LocalStatus::NEED_PREFIX || proof.status == LocalStatus::NEED_SUFFIX));
            CHECK_FALSE(proof.checkpoint.has_value());
          }
        }
      }
    }
  }
}

namespace {

/// Test-only collected scalar suffix; production restart/decoder retains no vector.
struct LocalCapture {
  std::vector<DecodedScalar> scalars; ///< Committed scalar output across bounded calls.
  DocumentDecodeStatus status{};      ///< Last feed outcome.
};

LocalCapture advanceLocalDecoder(DocumentDecoder &decoder, std::span<const std::byte> bytes, std::uint64_t offset,
                                 bool final) {
  LocalCapture result;
  std::size_t consumed = 0;

  for (std::size_t attempt = 0; attempt < bytes.size() * 8 + 64; ++attempt) {
    std::array<DecodedScalar, 1> output{};
    const auto step = decoder.feed(bytes.subspan(consumed), offset + consumed, final, output);

    REQUIRE(step.inputConsumed <= 1);
    consumed += step.inputConsumed;
    result.scalars.insert(result.scalars.end(), output.begin(), output.begin() + step.outputProduced);
    result.status = step.status;

    if (step.status != DocumentDecodeStatus::WORK_LIMIT && step.status != DocumentDecodeStatus::OUTPUT_FULL) {
      return result;
    }

    REQUIRE((step.inputConsumed != 0 || step.outputProduced != 0));
  }

  FAIL("bounded restored decoder did not terminate");

  return result;
}

} // namespace

TEST_CASE("restored local checkpoints preserve one-byte work budget and every suffix split") {
  for (const auto &corpus : localCorpora()) {
    const auto origin = realOrigin(corpus, DocumentDecodePolicy::REPLACE, 1);

    for (std::size_t anchor = 0; anchor < corpus.bytes.size(); ++anchor) {
      const auto begin = anchor > 3 ? anchor - 3 : 0;
      const auto length = std::min(DocumentDecoder::LOCAL_WINDOW_BYTES, corpus.bytes.size() - begin);
      const auto proof = DocumentDecoder::localCheckpoint(origin, std::span(corpus.bytes).subspan(begin, length), begin,
                                                          anchor, begin + length == corpus.bytes.size());

      REQUIRE(proof.status == LocalStatus::READY);
      REQUIRE(proof.checkpoint.has_value());

      const auto suffix = std::span(corpus.bytes).subspan(static_cast<std::size_t>(proof.restartByteOffset));
      const auto expected =
        std::find_if(corpus.expected.begin(), corpus.expected.end(), [&](const DecodedScalar &scalar) {
          return scalar.byteOffset == proof.restartByteOffset;
        });

      REQUIRE(expected != corpus.expected.end());

      for (std::size_t split = 0; split <= suffix.size(); ++split) {
        auto restored = DocumentDecoder::restore(*proof.checkpoint);
        auto first = advanceLocalDecoder(restored, suffix.first(split), proof.restartByteOffset, false);
        auto second = advanceLocalDecoder(restored, suffix.subspan(split), proof.restartByteOffset + split, true);

        REQUIRE(first.status == DocumentDecodeStatus::NEED_INPUT);
        REQUIRE(second.status == DocumentDecodeStatus::DONE);
        first.scalars.insert(first.scalars.end(), second.scalars.begin(), second.scalars.end());
        REQUIRE(first.scalars.size() == static_cast<std::size_t>(corpus.expected.end() - expected));

        for (std::size_t i = 0; i < first.scalars.size(); ++i) {
          checkScalar(first.scalars[i], expected[i]);
        }
      }
    }
  }
}

TEST_CASE("local context requests distinguish missing UTF prefixes suffixes and true EOF") {
  const auto corpora = localCorpora();
  const auto utf8 = realOrigin(corpora[0], DocumentDecodePolicy::REPLACE);

  CHECK(DocumentDecoder::localCheckpoint(utf8, raw({0x82, 0xac}), 2, 2).status == LocalStatus::NEED_PREFIX);
  CHECK(DocumentDecoder::localCheckpoint(utf8, raw({0xe2, 0x82}), 1, 2).status == LocalStatus::NEED_SUFFIX);
  CHECK(DocumentDecoder::localCheckpoint(utf8, {}, 3, 3).status == LocalStatus::NEED_SUFFIX);

  auto truncated = DocumentDecoder::localCheckpoint(utf8, raw({0xe2, 0x82}), 1, 2, true);

  REQUIRE(truncated.status == LocalStatus::READY);
  REQUIRE(truncated.checkpoint.has_value());
  CHECK(truncated.restartByteOffset == 1);
  auto resumed = DocumentDecoder::restore(*truncated.checkpoint);
  std::array<DecodedScalar, 2> output{};
  const auto decoded = resumed.feed(raw({0xe2, 0x82}), 1, true, output);

  REQUIRE(decoded.status == DocumentDecodeStatus::DONE);
  checkScalar(output[0], {0xfffd, 1, 2, true});

  const auto utf16 = realOrigin(corpora[1], DocumentDecodePolicy::REPLACE);
  CHECK(DocumentDecoder::localCheckpoint(utf16, raw({0, 0xde}), 4, 4).status == LocalStatus::NEED_PREFIX);
  CHECK(DocumentDecoder::localCheckpoint(utf16, raw({0x3d}), 2, 2).status == LocalStatus::NEED_SUFFIX);
  CHECK(DocumentDecoder::localCheckpoint(utf16, raw({0xd8, 0, 0xde}), 3, 3).status == LocalStatus::NEED_PREFIX);

  const auto utf32 = realOrigin(corpora[3], DocumentDecodePolicy::REPLACE);
  CHECK(DocumentDecoder::localCheckpoint(utf32, raw({0xf6, 1, 0}), 5, 5).status == LocalStatus::NEED_PREFIX);
  CHECK(DocumentDecoder::localCheckpoint(utf32, raw({0, 0xf6}), 4, 5).status == LocalStatus::NEED_SUFFIX);
}

TEST_CASE("local strict failures are repeatable atomic and do not validate an omitted prefix") {
  LocalCorpus corpus{DocumentEncoding::UTF8, raw({0x41, 0xe1, 0x80, 0x42}), {}, 1, {DocumentEncoding::UTF8}};
  const auto origin = realOrigin(corpus, DocumentDecodePolicy::STRICT_MODE);

  for (int attempt = 0; attempt < 3; ++attempt) {
    const auto error = DocumentDecoder::localCheckpoint(origin, std::span(corpus.bytes).subspan(1), 1, 2, true);

    CHECK(error.status == LocalStatus::MALFORMED);
    CHECK_FALSE(error.checkpoint.has_value());
    CHECK(error.errorOffset == 1);
    CHECK(error.errorLength == 2);
  }

  auto unchanged = DocumentDecoder::restore(origin);
  std::array<DecodedScalar, 8> output{};
  const auto error = unchanged.feed(std::span(corpus.bytes).subspan(1), 1, true, output);

  CHECK(error.status == DocumentDecodeStatus::MALFORMED);
  CHECK(error.inputConsumed == 0);
  CHECK(error.outputProduced == 0);
  CHECK(unchanged.encoding() == DocumentEncoding::UTF8);

  const auto valid = DocumentDecoder::localCheckpoint(origin, std::span(corpus.bytes).last(1), 3, 3, true);

  REQUIRE(valid.status == LocalStatus::READY);
  REQUIRE(valid.checkpoint.has_value());
  auto local = DocumentDecoder::restore(*valid.checkpoint);
  const auto read = local.feed(std::span(corpus.bytes).last(1), 3, true, output);

  REQUIRE(read.status == DocumentDecodeStatus::DONE);
  REQUIRE(read.outputProduced == 1);
  checkScalar(output[0], {U'B', 3, 1, false});
}

TEST_CASE("local EOF never creates continuation checkpoints from odd UTF tails") {
  for (const auto &corpus : localCorpora()) {
    const auto origin = realOrigin(corpus, DocumentDecodePolicy::REPLACE);
    const auto eof = DocumentDecoder::localCheckpoint(origin, {}, corpus.bytes.size(), corpus.bytes.size(), true);

    CHECK(eof.status == LocalStatus::AT_END);
    CHECK(eof.restartByteOffset == corpus.bytes.size());
    CHECK_FALSE(eof.checkpoint.has_value());
  }

  DocumentDecoder empty({DocumentEncoding::UTF8});
  std::array<DecodedScalar, 1> output{};

  CHECK(empty.feed({}, 0, true, output).status == DocumentDecodeStatus::DONE);
  CHECK_FALSE(empty.checkpoint().has_value());

  // Empty or ambiguous BOM-only sources need no continuation seed; existing
  // capture semantics remain intact rather than exposing arbitrary-offset state.
  DocumentDecoder bomOnly({DocumentEncoding::UTF16LE});
  CHECK(bomOnly.feed(raw({0xff, 0xfe}), 0, true, output).status == DocumentDecodeStatus::DONE);
  CHECK_FALSE(bomOnly.checkpoint().has_value());
}

TEST_CASE("local restart preserves original BOM mismatch and never reprobes a middle BOM") {
  LocalCorpus mismatch{DocumentEncoding::WINDOWS1252,
                       raw({0xef, 0xbb, 0xbf, 0x41, 0x42}),
                       {},
                       4,
                       {DocumentEncoding::WINDOWS1252},
                       0,
                       true};
  const auto origin = realOrigin(mismatch, DocumentDecodePolicy::REPLACE);
  const auto proof = DocumentDecoder::localCheckpoint(origin, std::span(mismatch.bytes).last(1), 4, 4, true);

  checkLocalProof(mismatch, proof, {U'B', 4, 1, false});

  LocalCorpus middle{
    DocumentEncoding::UTF8, raw({0x41, 0x42, 0x43, 0xef, 0xbb, 0xbf, 0x44}), {}, 3, {DocumentEncoding::UTF8}};
  const auto middleOrigin = realOrigin(middle, DocumentDecodePolicy::REPLACE);
  const auto middleProof =
    DocumentDecoder::localCheckpoint(middleOrigin, std::span(middle.bytes).subspan(2), 2, 4, true);

  checkLocalProof(middle, middleProof, {0xfeff, 3, 3, false});

  DocumentDecoder withBom;
  std::array<DecodedScalar, 1> output{};

  REQUIRE(withBom.feed(raw({0xef, 0xbb, 0xbf}), 0, false, output).status == DocumentDecodeStatus::NEED_INPUT);
  REQUIRE(withBom.checkpoint().has_value());
  const auto bomOrigin = *withBom.checkpoint();
  const auto end = DocumentDecoder::localCheckpoint(bomOrigin, raw({0xef, 0xbb, 0xbf}), 0, 1, true);

  CHECK(end.status == LocalStatus::AT_END);
  CHECK(end.restartByteOffset == 3);
  CHECK_FALSE(end.checkpoint.has_value());
  CHECK(DocumentDecoder::localCheckpoint(bomOrigin, raw({0xef}), 0, 0).status == LocalStatus::NEED_SUFFIX);
}

TEST_CASE("local restart rejects invalid ranges and unresolved forged representation") {
  static_assert(!std::is_default_constructible_v<DocumentDecoderCheckpoint>);
  static_assert(!std::is_constructible_v<DocumentDecoderCheckpoint, DocumentDecoderOptions, DocumentEncoding,
                                         std::uint64_t, std::uint8_t, bool>);
  static_assert(std::is_trivially_copyable_v<DocumentDecoderCheckpoint>);
  static_assert(sizeof(DocumentDecoderLocalResult) < 256);

  const auto corpus = localCorpora()[0];
  const auto origin = realOrigin(corpus, DocumentDecodePolicy::REPLACE);
  const auto forged =
    std::bit_cast<DocumentDecoderCheckpoint>(std::array<std::byte, sizeof(DocumentDecoderCheckpoint)>{});

  CHECK(DocumentDecoder::localCheckpoint(forged, raw({0x41}), 0, 0).status == LocalStatus::INVALID_ORIGIN);
  CHECK(DocumentDecoder::localCheckpoint(origin, raw({0x41}), 4, 3).status == LocalStatus::INVALID_RANGE);
  CHECK(DocumentDecoder::localCheckpoint(origin, raw({0x41}), 4, 6).status == LocalStatus::INVALID_RANGE);
  CHECK(DocumentDecoder::localCheckpoint(origin, std::array<std::byte, 9>{}, 4, 4).status ==
        LocalStatus::INVALID_RANGE);
  CHECK(DocumentDecoder::localCheckpoint(origin, raw({0x41}), UINT64_MAX, UINT64_MAX).status ==
        LocalStatus::INVALID_RANGE);
  CHECK(DocumentDecoder::localCheckpoint(origin, {}, 0, 0, true).status == LocalStatus::INVALID_RANGE);

  DocumentDecoder unresolved;

  CHECK_FALSE(unresolved.checkpoint().has_value());
  std::array<DecodedScalar, 1> output{};
  CHECK(unresolved.feed(raw({0xef}), 0, false, output).status == DocumentDecodeStatus::NEED_INPUT);
  CHECK_FALSE(unresolved.checkpoint().has_value());
}

TEST_CASE("local boundary arithmetic preserves offsets above four GiB and at uint64 end") {
  const auto corpus = localCorpora()[0];
  const auto origin = realOrigin(corpus, DocumentDecodePolicy::REPLACE);
  constexpr std::uint64_t OFFSET = (std::uint64_t{1} << 32) + 123;
  const auto bytes = raw({0x41, 0xf0, 0x9f, 0x98, 0x80, 0x42});
  const auto proof = DocumentDecoder::localCheckpoint(origin, bytes, OFFSET, OFFSET + 3, true);

  REQUIRE(proof.status == LocalStatus::READY);
  REQUIRE(proof.checkpoint.has_value());
  CHECK(proof.restartByteOffset == OFFSET + 1);
  auto resumed = DocumentDecoder::restore(*proof.checkpoint);
  std::array<DecodedScalar, 2> output{};
  const auto read = resumed.feed(std::span(bytes).subspan(1), OFFSET + 1, true, output);

  REQUIRE(read.status == DocumentDecodeStatus::DONE);
  REQUIRE(read.outputProduced == 2);
  checkScalar(output[0], {0x1f600, OFFSET + 1, 4, false});
  checkScalar(output[1], {U'B', OFFSET + 5, 1, false});

  const auto last = DocumentDecoder::localCheckpoint(origin, raw({0x41}), UINT64_MAX - 1, UINT64_MAX - 1, true);

  REQUIRE(last.status == LocalStatus::READY);
  REQUIRE(last.checkpoint.has_value());
  auto maximum = DocumentDecoder::restore(*last.checkpoint);
  const auto maximumRead = maximum.feed(raw({0x41}), UINT64_MAX - 1, true, output);

  REQUIRE(maximumRead.status == DocumentDecodeStatus::DONE);
  checkScalar(output[0], {U'A', UINT64_MAX - 1, 1, false});
  const auto eof = DocumentDecoder::localCheckpoint(origin, {}, UINT64_MAX, UINT64_MAX, true);

  CHECK(eof.status == LocalStatus::AT_END);
  CHECK_FALSE(eof.checkpoint.has_value());

  constexpr std::uint64_t ALIGNED_OFFSET = (std::uint64_t{1} << 32) + 8;
  const auto corpora = localCorpora();

  for (std::size_t i = 1; i <= 4; ++i) {
    const auto &wide = corpora[i];
    const auto wideOrigin = realOrigin(wide, DocumentDecodePolicy::REPLACE);
    const auto windowBegin = wide.originBytes - 1;
    const auto wideProof =
      DocumentDecoder::localCheckpoint(wideOrigin, std::span(wide.bytes).subspan(windowBegin, 8),
                                       ALIGNED_OFFSET + windowBegin, ALIGNED_OFFSET + wide.originBytes + 3);

    REQUIRE(wideProof.status == LocalStatus::READY);
    REQUIRE(wideProof.checkpoint.has_value());
    CHECK(wideProof.restartByteOffset == ALIGNED_OFFSET + wide.originBytes);
    auto wideDecoder = DocumentDecoder::restore(*wideProof.checkpoint);
    const auto wideRead =
      wideDecoder.feed(std::span(wide.bytes).subspan(wide.originBytes, 4), wideProof.restartByteOffset, false, output);

    REQUIRE(wideRead.status == DocumentDecodeStatus::NEED_INPUT);
    REQUIRE(wideRead.outputProduced == 1);
    checkScalar(output[0], {0x1f600, ALIGNED_OFFSET + wide.originBytes, 4, false});
  }
}

TEST_CASE("UTF8 continuation runs do not invent an earlier scalar outside the local window") {
  LocalCorpus corpus{DocumentEncoding::UTF8, raw({0x41}), {}, 1, {DocumentEncoding::UTF8}};

  corpus.bytes.insert(corpus.bytes.end(), 64, std::byte{0x80});
  const auto origin = realOrigin(corpus, DocumentDecodePolicy::REPLACE);
  const auto proof = DocumentDecoder::localCheckpoint(origin, std::span(corpus.bytes).subspan(30, 8), 30, 33);

  checkLocalProof(corpus, proof, {0xfffd, 33, 1, true});
}

TEST_CASE("UTF8 local proofs match the full decoder for every adjacent byte pair") {
  const auto corpus = localCorpora()[0];
  const auto origin = realOrigin(corpus, DocumentDecodePolicy::REPLACE);

  for (unsigned first = 0; first <= 0xff; ++first) {
    for (unsigned second = 0; second <= 0xff; ++second) {
      const std::array bytes{std::byte{0x41}, static_cast<std::byte>(first), static_cast<std::byte>(second),
                             std::byte{0x5a}};
      DocumentDecoder sequential({DocumentEncoding::UTF8});
      std::array<DecodedScalar, 4> expected{};
      const auto complete = sequential.feed(bytes, 0, true, expected);

      REQUIRE(complete.status == DocumentDecodeStatus::DONE);

      for (std::uint64_t anchor = 1; anchor <= 2; ++anchor) {
        const auto proof = DocumentDecoder::localCheckpoint(origin, bytes, 0, anchor, true);

        REQUIRE(proof.status == LocalStatus::READY);
        REQUIRE(proof.checkpoint.has_value());
        const auto found =
          std::find_if(expected.begin(), expected.begin() + complete.outputProduced, [&](const DecodedScalar &scalar) {
            return scalar.byteOffset <= anchor && anchor - scalar.byteOffset < scalar.byteLength;
          });

        REQUIRE(found != expected.begin() + complete.outputProduced);
        CHECK(proof.restartByteOffset == found->byteOffset);
        auto resumed = DocumentDecoder::restore(*proof.checkpoint);
        std::array<DecodedScalar, 1> selected{};
        const auto result = resumed.feed(std::span(bytes).subspan(static_cast<std::size_t>(proof.restartByteOffset)),
                                         proof.restartByteOffset, true, selected);

        REQUIRE(result.outputProduced == 1);
        checkScalar(selected[0], *found);
      }
    }
  }
}
