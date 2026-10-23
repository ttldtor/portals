// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/style_pss.hpp>

#include <bit>
#include <cstdlib>
#include <string>

namespace allocation_test {
thread_local std::ptrdiff_t remaining = -1;
} // namespace allocation_test

#if defined(__GNUC__)
__attribute__((noinline))
#endif
void *operator new(std::size_t size) {
  if (allocation_test::remaining == 0) {
    throw std::bad_alloc();
  }

  if (allocation_test::remaining > 0) {
    --allocation_test::remaining;
  }

  if (void *memory = std::malloc(size ? size : 1)) {
    return memory;
  }

  throw std::bad_alloc();
}

#if defined(__GNUC__)
__attribute__((noinline))
#endif
void operator delete(void *memory) noexcept {
  std::free(memory);
}

#if defined(__GNUC__)
__attribute__((noinline))
#endif
void operator delete(void *memory, std::size_t) noexcept {
  std::free(memory);
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete[](void *memory) noexcept {
  ::operator delete(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
  ::operator delete(memory);
}

namespace {
using namespace portals;
namespace sc = portals::style::ct;
using namespace std::literals;

constexpr auto DIRECT = sc::pss("Button { foreground: #ff0000; bold: false; }", "fixture.pss");
constexpr auto RICH = sc::pss(
  [] {
    return "Button { foreground: #ff0000; bold: false; }"sv;
  },
  [] {
    return "fixture.pss"sv;
  });
constexpr auto REPEATED = sc::pss(
  [] {
    return "/* one */\nButton#a.hot.hot:focused { bold:true; bold:false; }\n*{min-width:-0;}"sv;
  },
  [] {
    return "duplicates.pss"sv;
  });
constexpr auto TYPED = sc::compactSheet(
  std::array{sc::flatRule(sc::type<"Button">(), sc::fg(sc::rgb(255, 0, 0)), sc::bold(false)),
             sc::flatRule(sc::selector<"Button", "action", "primary">(StyleState::FOCUSED), sc::bg(sc::rgb(1, 2, 3))),
             sc::flatRule(sc::all(), sc::minWidth(std::int32_t{7}))},
  "typed-fixture");

constexpr auto DEFINITIONS = sc::parameters(sc::parameter<"accent">(sc::rgb(3, 4, 5)), sc::parameter<"enabled">(false));
constexpr auto SNAPSHOT = sc::compactSheet(std::array{
  sc::flatRule(DEFINITIONS, sc::id<"action">(), sc::fg(sc::token<"accent">()), sc::bold(sc::token<"enabled">()))});

constexpr auto EMPTY = sc::pss([] {
  return ""sv;
});
constexpr auto BOM = sc::pss([] {
  return "\xef\xbb\xbf*{bold:false;}"sv;
});
constexpr auto ALL_PROPERTIES = sc::pss([] {
  return "Button#action.primary{foreground:#abcdef;background:#ABCDEF;bold:true;italic:false;underline:true;min-width:2147483647;}"sv;
});

static_assert(EMPTY.rules().empty() && EMPTY.declarations().empty());
static_assert(RICH.rules().size() == 1 && RICH.declarations().size() == 2);
static_assert(sizeof(RICH) < sizeof(DIRECT));
static_assert(sizeof(sc::pss("*{}")) < 1024);
static_assert(sizeof(sc::CompactSheet<sc::MAX_CT_PSS_RULES, sc::MAX_CT_PSS_CLASSES, sc::MAX_CT_PSS_DECLARATIONS,
                                      sc::MAX_CT_PSS_TEXT_BYTES>) <= sc::MAX_CT_PSS_DESCRIPTOR_BYTES);
static_assert(std::is_trivially_copyable_v<decltype(RICH)>);
static_assert(!sc::detail::PssSourceProvider<decltype([] {
  return "\0bad";
})>);

std::string repeated(std::string_view text, std::size_t count) {
  std::string result;

  result.reserve(text.size() * count);

  for (std::size_t index = 0; index < count; ++index) {
    result += text;
  }

  return result;
}

void expectError(std::string_view input, PssErrorCode code, PssLocation location, std::string_view subject = {}) {
  const auto result = parsePssDocument(input);

  REQUIRE_FALSE(result);
  CHECK(result.error().code == code);
  CHECK(result.error().location == location);
  CHECK(result.error().subjectView() == subject);
}
} // namespace

TEST_CASE("legacy parser characterization preserves observed v0 bytes and syntax") {
  struct Fixture {
    const char *name;
    std::string bytes;
    bool valid;
  };

  const Fixture fixtures[] = {
    {"duplicates", "*{bold:true;bold:false;min-width:1;min-width:-0;}", true},
    {"compound", "Button#a.hot.hot:focused{foreground:#aBcDEF;}", true},
    {"selector space", "Button .hot{}", false},
    {"selector comment", "Button/*x*/.hot{}", false},
    {"comment invalid utf8", std::string("/*") + std::bit_cast<char>(std::uint8_t{0xff}) + "*/ *{}", true},
    {"comment nul", std::string("/*") + char(0) + "*/ *{}", true},
    {"bom", std::string("\xef\xbb\xbf") + "*{}", false},
    {"plus width", "*{min-width:+0;}", false},
    {"whitespace value", "*{bold:true /*x*/ ;}", true},
    {"embedded value comment", "*{bold:true/*x*/;}", false},
    {"unterminated", "/*", false},
    {"empty", "", true},
    {"state metric", "*:normal{min-width:0;}", false}};

  for (const auto &fixture : fixtures) {
    INFO(fixture.name);
    CHECK(bool(parsePss(fixture.bytes)) == fixture.valid);
  }

  const auto duplicate = parsePss(fixtures[0].bytes);

  REQUIRE(duplicate);
  CHECK(duplicate->rules[0].declarations.bold == false);
  CHECK(duplicate->rules[0].declarations.minWidth == 0);
}

TEST_CASE("runtime and both compile-time paths retain identical complete owning metadata") {
  const auto direct = sc::materializeDocument(DIRECT);
  const auto rich = sc::materializeDocument(RICH);
  const auto runtime = parsePssDocument("Button { foreground: #ff0000; bold: false; }", "fixture.pss");

  REQUIRE(runtime);
  REQUIRE(direct);
  REQUIRE(rich);
  CHECK(*direct == *runtime);
  CHECK(*rich == *runtime);
  CHECK(direct->sheet.rules[0].declarations.bold == false);
  CHECK(direct->originKind == PssOriginKind::PSS_BYTES);
  CHECK(direct->origins[0].selector == PssLocation{0, 1, 1});
  CHECK(direct->declarations[0].location == PssLocation{9, 1, 10});
  CHECK(direct->declarations[1].location == PssLocation{30, 1, 31});
  CHECK(sc::materialize(DIRECT) == runtime->sheet);

  const auto repeatedCt = sc::materializeDocument(REPEATED);
  const auto repeatedRt = parsePssDocument(
    "/* one */\nButton#a.hot.hot:focused { bold:true; bold:false; }\n*{min-width:-0;}", "duplicates.pss");

  REQUIRE(repeatedCt);
  REQUIRE(repeatedRt);
  CHECK(*repeatedCt == *repeatedRt);
  CHECK(repeatedCt->origins[0].declarationCount == 2);
  CHECK(repeatedCt->origins[0].effectiveDeclaration[static_cast<std::size_t>(PssProperty::BOLD)] == 1);
  CHECK(repeatedCt->origins[0].effectiveDeclaration[static_cast<std::size_t>(PssProperty::FOREGROUND)] ==
        NO_PSS_DECLARATION);
  CHECK(repeatedCt->declarations[0].property == PssProperty::BOLD);
  CHECK(repeatedCt->declarations[1].property == PssProperty::BOLD);
  CHECK(repeatedCt->declarations[0].value.boolean);
  CHECK_FALSE(repeatedCt->declarations[1].value.boolean);
  CHECK(repeatedCt->declarations[0].location != repeatedCt->declarations[1].location);
  CHECK(repeatedCt->declarations[2].ruleIndex == 1);
  CHECK(repeatedCt->sheet.rules[1].declarations.minWidth == 0);
}

TEST_CASE("strict UTF8 and BOM admission preserves original coordinates and complete extents") {
  const auto document = sc::materializeDocument(BOM);
  const auto parsed = parsePssDocument("\xef\xbb\xbf*{bold:false;}");

  REQUIRE(document);
  REQUIRE(parsed);
  CHECK(*document == *parsed);
  CHECK(parsed->origins[0].selector == PssLocation{3, 1, 4});
  CHECK(parsed->declarations[0].location == PssLocation{5, 1, 6});
  CHECK(parsePssDocument("/* \xd0\xbf\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82 */\n*{}"));

  expectError("/*\0bad*/ *{}"sv, PssErrorCode::NUL_BYTE, {2, 1, 3});
  expectError("\0bad"sv, PssErrorCode::NUL_BYTE, {0, 1, 1});
  expectError("/*\xc0\x80*/ *{}", PssErrorCode::INVALID_UTF8, {2, 1, 3});
  expectError("\n/*\xed\xa0\x80*/", PssErrorCode::INVALID_UTF8, {3, 2, 3});
  expectError("/*\xf4\x90\x80\x80*/", PssErrorCode::INVALID_UTF8, {2, 1, 3});
  expectError("/*\xf0\x9f*/", PssErrorCode::INVALID_UTF8, {2, 1, 3});
  expectError("\xef\xbb\xbf\xef\xbb\xbf*{}", PssErrorCode::EXPECTED_SELECTOR, {3, 1, 4});
}

TEST_CASE("structured first errors preserve exact coordinates and an owning property subject") {
  expectError("Button {\n unknown: true;\n}", PssErrorCode::UNKNOWN_PROPERTY, {10, 2, 2}, "unknown");
  expectError("Button {\n foreground: banana;\n}", PssErrorCode::INVALID_VALUE, {22, 2, 14}, "foreground");
  expectError("*{min-width:+0;}", PssErrorCode::INVALID_WIDTH, {12, 1, 13}, "min-width");
  expectError("*{min-width:2147483648;}", PssErrorCode::INVALID_WIDTH, {12, 1, 13}, "min-width");
  expectError("*:normal{min-width:0;}", PssErrorCode::INVALID_WIDTH, {19, 1, 20}, "min-width");
  expectError("Button .hot{}", PssErrorCode::EXPECTED_PUNCTUATION, {7, 1, 8});
  expectError("Button#a#b{}", PssErrorCode::DUPLICATE_ID, {9, 1, 10});
  expectError("Button:focused:normal{}", PssErrorCode::DUPLICATE_STATE, {15, 1, 16});
  expectError("Button:unknown{}", PssErrorCode::UNKNOWN_STATE, {7, 1, 8}, "unknown");
  expectError("/*", PssErrorCode::UNTERMINATED_COMMENT, {2, 1, 3});

  const auto error = parsePssDocument(std::string("*{temporary-property:true;}"));

  REQUIRE_FALSE(error);
  CHECK(error.error().subjectView() == "temporary-property");

  const auto oversizedName = std::string(100, 'x');
  const auto truncated = parsePssDocument("*{" + oversizedName + ":true;}");

  REQUIRE_FALSE(truncated);
  CHECK(truncated.error().subjectTruncated);
  CHECK(truncated.error().subjectSize == MAX_PSS_ERROR_SUBJECT_BYTES);
  CHECK(truncated.error().subjectView() == std::string(MAX_PSS_ERROR_SUBJECT_BYTES, 'x'));
}

TEST_CASE("scalar typed array shares the existing cascade without parameter or selector lifetime retention") {
  const auto document = sc::materializeDocument(TYPED);
  const auto expected =
    parsePss("Button{foreground:#ff0000;bold:false;}Button#action.primary:focused{background:#010203;}*{min-width:7;}");

  REQUIRE(document);
  REQUIRE(expected);
  CHECK(document->sheet == *expected);
  CHECK(document->originKind == PssOriginKind::SYNTHETIC_TYPED);
  CHECK(document->sourceName == "typed-fixture");
  CHECK(document->origins[1].selector.line == 2);
  CHECK(document->declarations[2].ruleIndex == 1);

  const auto snapshot = sc::materializeDocument(SNAPSHOT);

  REQUIRE(snapshot);
  CHECK(snapshot->sheet.rules[0].selector.id == "action");
  CHECK(snapshot->sheet.rules[0].declarations.foreground == RgbColor{3, 4, 5});
  CHECK(snapshot->sheet.rules[0].declarations.bold == false);
  CHECK(sc::materialize(sc::sheet(sc::parameters(), sc::rule(sc::className<"primary">(), sc::bg(sc::rgb(1, 2, 3)))))
          .rules.size() == 1);
}

TEST_CASE("flat expression preflight preserves qualified values nested blends and empty rules") {
  using AccentToken = decltype(sc::token<"accent">());
  constexpr auto QUALIFIED = sc::flatRule(DEFINITIONS, sc::all(), sc::foreground<const RgbColor>(sc::rgb(9, 8, 7)),
                                          sc::background<const AccentToken>(sc::token<"accent">()));
  constexpr auto ACCENT = sc::token<"accent">();
  constexpr auto BLEND_TARGET = sc::rgb(7, 8, 9);
  constexpr auto INNER = sc::mix(ACCENT, BLEND_TARGET, 50);
  constexpr auto NESTED = sc::flatRule(DEFINITIONS, sc::all(), sc::fg(sc::mix(INNER, sc::rgb(10, 20, 30), 50)));
  constexpr auto EMPTY_RULE = sc::flatRule(sc::all());
  constexpr auto DESCRIPTION = sc::compactSheet(std::array{QUALIFIED, NESTED, EMPTY_RULE});
  const auto document = sc::materializeDocument(DESCRIPTION);

  REQUIRE(document);
  REQUIRE(document->sheet.rules.size() == 3);
  CHECK(document->sheet.rules[0].declarations.foreground == RgbColor{9, 8, 7});
  CHECK(document->sheet.rules[0].declarations.background == RgbColor{3, 4, 5});
  CHECK(document->sheet.rules[1].declarations.foreground == RgbColor{7, 13, 18});
  CHECK(document->sheet.rules[2].declarations == StyleDeclarations{});
  REQUIRE(document->declarations.size() == 3);
  CHECK(document->declarations[0].value.color == RgbColor{9, 8, 7});
  CHECK(document->declarations[1].value.color == RgbColor{3, 4, 5});
  CHECK(document->declarations[2].value.color == RgbColor{7, 13, 18});
}

TEST_CASE("owning runtime documents and errors outlive caller input and source identity") {
  auto document = [] {
    std::string bytes = "LongWidgetType#owned-id.primary{bold:false;}";
    std::string identity = "a source identity beyond the typical short string capacity";

    return parsePssDocument(bytes, identity);
  }();

  REQUIRE(document);
  CHECK(document->sourceName == "a source identity beyond the typical short string capacity");
  CHECK(document->sheet.rules[0].selector.type == "LongWidgetType");
  CHECK(document->sheet.rules[0].selector.id == "owned-id");
  CHECK(document->sheet.rules[0].selector.classes[0] == "primary");
  CHECK(document->origins[0].declarationCount == 1);
}

TEST_CASE("named CT and runtime budgets reject the correct boundary without borrowed output") {
  const auto ruleBoundary = repeated("*{}", MAX_STYLE_RULES);

  REQUIRE(parsePssDocument(ruleBoundary));
  expectError(ruleBoundary + "*{}", PssErrorCode::RULE_LIMIT, {ruleBoundary.size(), 1, ruleBoundary.size() + 1});

  const auto inputBoundary = std::string(MAX_PSS_BYTES, ' ');

  REQUIRE(parsePssDocument(inputBoundary));
  expectError(inputBoundary + ' ', PssErrorCode::INPUT_LIMIT, {});

  const auto maxName = std::string(MAX_PSS_SOURCE_NAME_BYTES, 'a');

  CHECK(parsePssDocument("*{}", maxName));
  CHECK_FALSE(parsePssDocument("*{}", maxName + 'a'));

  const auto inspection = [](std::string_view source, std::string_view origin = {}) {
    return sc::detail::inspectPss(source, origin);
  };

  CHECK(inspection(repeated("*{}", sc::MAX_CT_PSS_RULES)).valid);
  CHECK(inspection(repeated("*{}", sc::MAX_CT_PSS_RULES + 1)).error.code == PssErrorCode::RULE_LIMIT);
  CHECK(inspection("*" + repeated(".a", sc::MAX_CT_PSS_CLASSES) + "{}").valid);
  CHECK(inspection("*" + repeated(".a", sc::MAX_CT_PSS_CLASSES + 1) + "{}").error.code == PssErrorCode::CLASS_LIMIT);
  CHECK(inspection("*{" + repeated("bold:true;", sc::MAX_CT_PSS_DECLARATIONS) + "}").valid);
  CHECK(inspection("*{" + repeated("bold:true;", sc::MAX_CT_PSS_DECLARATIONS + 1) + "}").error.code ==
        PssErrorCode::DECLARATION_LIMIT);
  CHECK(inspection(std::string(sc::MAX_CT_PSS_BYTES, ' ')).valid);
  CHECK(inspection(std::string(sc::MAX_CT_PSS_BYTES + 1, ' ')).error.code == PssErrorCode::INPUT_LIMIT);
  CHECK(inspection("*{}", std::string(sc::MAX_CT_PSS_SOURCE_NAME_BYTES, 'a')).valid);
  CHECK(inspection("*{}", std::string(sc::MAX_CT_PSS_SOURCE_NAME_BYTES + 1, 'a')).error.code ==
        PssErrorCode::SOURCE_NAME_LIMIT);

  const auto manyNames = std::string(sc::MAX_CT_PSS_BYTES - 2, 'a') + "{}";

  CHECK(inspection(manyNames).valid);
  CHECK(inspection(manyNames, "abc").error.code == PssErrorCode::TEXT_LIMIT);

  const auto metadataOverflow = "*{" + repeated("bold:true;", MAX_PSS_BYTES / 10 - 1) + "}";
  const auto rejected = parsePssDocument(metadataOverflow);

  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().code == PssErrorCode::DOCUMENT_LIMIT);
  CHECK(parsePss(metadataOverflow));
}

TEST_CASE("shared grammar kernel is allocation-free and runtime wrappers catch every ownership failure") {
  allocation_test::remaining = 0;

  const auto admission = sc::detail::inspectPss("Button#a.x{foreground:#123456;bold:false;}", "origin");

  allocation_test::remaining = -1;

  CHECK(admission.valid);
  CHECK(admission.counts.rules == 1);
  CHECK(admission.counts.classes == 1);
  CHECK(admission.counts.declarations == 2);

  const auto run = [](auto operation) {
    bool sawFailure{};
    bool sawSuccess{};

    for (std::ptrdiff_t budget = 0; budget < 256; ++budget) {
      allocation_test::remaining = budget;

      auto result = operation();

      allocation_test::remaining = -1;

      if (result) {
        sawSuccess = true;

        break;
      }

      sawFailure = true;
      CHECK(result.error().cause == TreeError::NO_MEMORY);
    }

    CHECK(sawFailure);
    CHECK(sawSuccess);
  };

  run([] {
    return parsePssDocument("LongWidgetTypeName#long-widget-identity.primary{foreground:#123456;bold:false;}",
                            "a long caller supplied source identity with allocated storage");
  });
  run([] {
    return sc::materializeDocument(ALL_PROPERTIES);
  });
  run([] {
    return parsePss("LongWidgetTypeName#long-widget-identity.primary{foreground:#123456;bold:false;}");
  });
}

TEST_CASE("frame part declarations share runtime literal typed compact and state cascades") {
  static_assert(static_cast<int>(PssProperty::FOREGROUND) == 0);
  static_assert(static_cast<int>(PssProperty::MIN_WIDTH) == 5);
  constexpr auto literal =
    sc::pss("Border{foreground:#010203;frame-line-foreground:#040506;frame-title-bold:false;frame-ends-italic:true;}"
            "Border:focused{frame-title-foreground:#070809;}");
  constexpr auto typed = sc::compactSheet(
    std::array{sc::flatRule(sc::type<"Border">(), sc::fg(sc::rgb(1, 2, 3)), sc::frameLineForeground(sc::rgb(4, 5, 6)),
                            sc::frameTitleBold(false), sc::frameEndsItalic(true)),
               sc::flatRule(sc::type<"Border">(StyleState::FOCUSED), sc::frameTitleForeground(sc::rgb(7, 8, 9)))});
  const auto expected = parsePssDocument(
    "Border{foreground:#010203;frame-line-foreground:#040506;frame-title-bold:false;frame-ends-italic:true;}"
    "Border:focused{frame-title-foreground:#070809;}");
  const auto parsed = sc::materializeDocument(literal);
  const auto materialized = sc::materializeDocument(typed);
  REQUIRE(expected);
  REQUIRE(parsed);
  REQUIRE(materialized);
  CHECK(parsed->sheet == expected->sheet);
  CHECK(materialized->sheet == expected->sheet);
  CHECK(parsed->declarations == expected->declarations);
  const auto resolved = resolveStyle(expected->sheet, StyleTarget{.type = "Border"}, {});
  REQUIRE(resolved.frame[0].line);
  REQUIRE(resolved.frame[0].title);
  REQUIRE(resolved.frame[1].title);
  REQUIRE(resolved.frame[0].ends);
  CHECK(resolved.frame[0].line->foreground == RgbColor{4, 5, 6});
  CHECK(resolved.frame[0].title->foreground == RgbColor{1, 2, 3});
  CHECK_FALSE(resolved.frame[0].title->bold);
  CHECK(resolved.frame[1].title->foreground == RgbColor{7, 8, 9});
  CHECK(resolved.frame[0].ends->italic);
}
