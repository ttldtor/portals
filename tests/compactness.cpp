// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "compactness.hpp"

#include <array>
#include <filesystem>
#include <stdexcept>

using namespace portals::compactness;

namespace {

std::filesystem::path fixture(std::string_view name) {
  return std::filesystem::path(PORTALS_COMPACTNESS_FIXTURES) / name;
}

Footprint measureFixture(std::string_view name) {
  const std::array paths = {fixture(name)};

  return measureFiles(paths);
}

} // namespace

TEST_CASE("whitespace changes lines, not lexical token count") {
  const auto before = measureFixture("before.cpp");
  const auto spaced = measureFixture("spaced.cpp");

  // helper: 9 tokens; main: 11 tokens, with calls and punctuation counted separately.
  CHECK(before.metrics.lexicalTokensApprox == 20);
  CHECK(before.metrics.lexicalTokensApprox == spaced.metrics.lexicalTokensApprox);
  CHECK(before.metrics.codeLines == 2);
  CHECK(spaced.metrics.codeLines == 6);
  CHECK(before.metrics.maxBraceNesting == 1);
}

TEST_CASE("moving code to an explicit helper preserves full token footprint") {
  const auto before = measureFixture("before.cpp");
  const auto entryOnly = measureFixture("after.cpp");
  const std::array afterFiles = {fixture("after.cpp"), fixture("helper.hpp")};
  const auto after = measureFiles(afterFiles);

  CHECK(entryOnly.metrics.lexicalTokensApprox < before.metrics.lexicalTokensApprox);
  CHECK(after.metrics.files == 2);
  CHECK(after.metrics.lexicalTokensApprox == before.metrics.lexicalTokensApprox);
  CHECK(after.metrics.maxBraceNesting == before.metrics.maxBraceNesting);
}

TEST_CASE("canonical path deduplication and sorting ignore input order") {
  const std::array paths = {fixture("helper.hpp"), fixture("before.cpp"), fixture(".") / "before.cpp"};
  const std::array reordered = {fixture("before.cpp"), fixture("helper.hpp")};
  const auto first = measureFiles(paths);
  const auto second = measureFiles(reordered);

  CHECK(first.metrics.files == 2);
  CHECK(first.paths == second.paths);
  CHECK(renderReport(first, first, "json") == renderReport(second, second, "json"));
}

TEST_CASE("comments do not count as code or split literal contents") {
  const auto result = measureSource("// ignored { \"\n/* ignored\n} */\nconst char *s = \"// /* { }\"; // tail\n");

  CHECK(result.lexicalTokensApprox == 7);
  CHECK(result.codeLines == 1);
  CHECK(result.stringLiteralBytes == 11);
  CHECK(result.commentBytes > 0);
  CHECK(result.maxBraceNesting == 0);
  CHECK(measureSource("/* comment */ \n // line\n").codeLines == 0);
  CHECK(measureSource("a/**/b").lexicalTokensApprox == 2);
}

TEST_CASE("literal fixture keeps header, comment, raw body, and macro braces separate") {
  const auto result = measureFixture("literals.cpp");

  CHECK(result.metrics.preprocessorDirectives == 2);
  CHECK(result.metrics.preprocessorTokens == 8);
  CHECK(result.metrics.commentBytes == 20);
  CHECK(result.metrics.maxBraceNesting == 0);
  CHECK(result.metrics.stringLiteralBytes > 0);
}

TEST_CASE("raw, UTF-8, escaped and character literals are single tokens") {
  const auto raw = measureSource("u8R\"tag(// { /* }\nПривет\\\n世界)tag\"_suffix");
  const auto escaped = measureSource("u8\"a\\\"b\"_text '\\'' U'X' L\"text\"");

  CHECK(raw.lexicalTokensApprox == 1);
  CHECK(raw.codeLines == 3);
  CHECK(raw.commentBytes == 0);
  CHECK(raw.maxBraceNesting == 0);
  CHECK(raw.stringLiteralBytes == raw.sourceBytes);
  CHECK(escaped.lexicalTokensApprox == 4);
  CHECK(escaped.stringLiteralBytes == 20);
}

TEST_CASE("raw terminators are searched in physical text, with splices restored") {
  const std::string source = "R\"x(first )x\\\n\" still raw\n)x\";";
  const auto result = measureSource(source);

  CHECK(result.lexicalTokensApprox == 2);
  CHECK(result.codeLines == 3);
  CHECK(result.stringLiteralBytes == source.size() - 1);
}

TEST_CASE("splices join identifiers, comments and ordinary literal spellings") {
  const auto identifiers = measureSource("ident\\\nifier + value");
  const auto comments = measureSource("/\\\n/ hidden\\\nmore\nvalue");
  const auto ordinary = measureSource("\"a\\\r\nb\"");
  const auto whitespaceSplice = measureSource("ident\\ \t\r\nifier");

  CHECK(identifiers.lexicalTokensApprox == 3);
  CHECK(identifiers.codeLines == 2);
  CHECK(comments.lexicalTokensApprox == 1);
  CHECK(comments.codeLines == 1);
  CHECK(ordinary.lexicalTokensApprox == 1);
  CHECK(ordinary.stringLiteralBytes == 6);
  CHECK(ordinary.codeLines == 2);
  CHECK(whitespaceSplice.lexicalTokensApprox == 1);
}

TEST_CASE("BOM and CRLF normalization preserve token and physical line measurements") {
  const auto lf = measureSource("int x;\n\"é\";\n");
  const auto crlf = measureSource("\xEF\xBB\xBFint x;\r\n\"é\";\r\n");
  const auto cr = measureSource("int x;\r\"é\";\r");

  CHECK(crlf.lexicalTokensApprox == lf.lexicalTokensApprox);
  CHECK(crlf.codeLines == lf.codeLines);
  CHECK(cr.codeLines == lf.codeLines);
  CHECK(crlf.sourceBytes == lf.sourceBytes + 5);
  CHECK(crlf.maxLineBytes == 6);
  CHECK(lf.stringLiteralBytes == 4);
}

TEST_CASE("directives include introducers, header names, and continuation tokens") {
  const auto result = measureSource("/* lead */ #include \"file.hpp\"\n#define X { \\\n }\nint f() { return X; }\n");
  const auto digraph = measureSource("%:include <a/b.hpp>\n<% <% %> %>\n");

  CHECK(result.preprocessorDirectives == 2);
  CHECK(result.preprocessorTokens == 8);
  CHECK(result.stringLiteralBytes == 0);
  CHECK(result.maxBraceNesting == 1);
  CHECK(result.codeLines == 4);
  CHECK(digraph.preprocessorDirectives == 1);
  CHECK(digraph.preprocessorTokens == 3);
  CHECK(digraph.lexicalTokensApprox == 7);
  CHECK(digraph.maxBraceNesting == 2);
  CHECK(measureSource("x # y").preprocessorDirectives == 0);
}

TEST_CASE("block-comment internal newlines preserve directive and line-start state") {
  const auto macro = measureSource("#define OPEN /* comment\n*/ {\nOPEN\n}\n");
  const auto include = measureSource("#include /* multiline\n*/ <header.hpp>\n");
  const auto ordinary = measureSource("x /* multiline\n*/ # y\n");

  CHECK(macro.preprocessorDirectives == 1);
  CHECK(macro.preprocessorTokens == 4);
  CHECK(macro.maxBraceNesting == 0);
  CHECK(include.preprocessorDirectives == 1);
  CHECK(include.preprocessorTokens == 3);
  CHECK(include.stringLiteralBytes == 0);
  CHECK(ordinary.preprocessorDirectives == 0);
  CHECK(ordinary.preprocessorTokens == 0);
}

TEST_CASE("raw token spelling counts internal blank physical lines, excluding external blanks") {
  const auto result = measureSource("auto text = R\"(first\n\nthird)\";\n\n");

  CHECK(result.codeLines == 3);
  CHECK(result.lexicalTokensApprox == 5);
}

TEST_CASE("numbers and maximal-munch operators are preprocessing tokens") {
  const auto result = measureSource(".5 0x1.fp+2 1'000 3.14e-2 42_km >>= <=> ->* %:%: ...");

  CHECK(result.lexicalTokensApprox == 10);
  CHECK(measureSource("foo::bar").lexicalTokensApprox == 3);
}

TEST_CASE("brace nesting is lexical and resets per file") {
  CHECK(measureSource("} { { } } \"{\" /* { */").maxBraceNesting == 2);
  CHECK(measureSource("#define M {{{\n{ }\n").maxBraceNesting == 1);
}

TEST_CASE("malformed and unsupported sources fail instead of returning measurements") {
  const std::array invalid = {std::string_view("/* open"),
                              std::string_view("\"open"),
                              std::string_view("'"),
                              std::string_view("''"),
                              std::string_view("\"line\nbreak\""),
                              std::string_view("R\"bad delimiter(x)bad delimiter\""),
                              std::string_view("R\"0123456789abcdefg(x)0123456789abcdefg\""),
                              std::string_view("R\"x(no terminator"),
                              std::string_view("#include <open\n"),
                              std::string_view("#include \"\""),
                              std::string_view("Привет"),
                              std::string_view("\\u0041"),
                              std::string_view("<::"),
                              std::string_view("`")};

  for (const auto source : invalid) {
    INFO(source);
    CHECK_THROWS_AS(measureSource(source), std::runtime_error);
  }

  CHECK_THROWS_AS(measureSource(std::string_view("a\0b", 3)), std::runtime_error);
  CHECK_THROWS_AS(measureSource("\xC0\xAF"), std::runtime_error);
  CHECK_THROWS_AS(measureSource("\xED\xA0\x80"), std::runtime_error);
  CHECK_THROWS_AS(measureSource("\xF4\x90\x80\x80"), std::runtime_error);
  CHECK_THROWS_AS(measureSource("\xC2"), std::runtime_error);
  CHECK_THROWS_AS(measureFixture("malformed.cpp"), std::runtime_error);
}

TEST_CASE("missing paths, directories, and empty footprints fail") {
  const std::array missing = {fixture("does-not-exist.cpp")};
  const std::array directory = {fixture(".")};

  CHECK_THROWS(measureFiles(missing));
  CHECK_THROWS_AS(measureFiles(directory), std::runtime_error);
  CHECK_THROWS_AS(measureFiles({}), std::runtime_error);
}

TEST_CASE("empty baseline percentages are absent and explicit scopes stay honest") {
  const auto empty = measureFixture("empty.cpp");
  const auto after = measureFixture("after.cpp");
  const auto json = renderReport(empty, after, "json");
  const auto markdown = renderReport(empty, after, "markdown");

  CHECK(empty.metrics.lexicalTokensApprox == 0);
  CHECK(empty.metrics.codeLines == 0);
  CHECK(json.find("\"lexical_tokens_approx\": {\"before\": 0, \"after\": 11, \"delta\": 11, \"percent\": null}") !=
        std::string::npos);
  CHECK(json.find("NOT_MEASURED") != std::string::npos);
  CHECK(json.find("NOT_CHECKED") != std::string::npos);
  CHECK(markdown.find("N/A") != std::string::npos);
  CHECK(renderReport(after, empty, "json").find("\"delta\": -11, \"percent\": -100.000000") != std::string::npos);
  CHECK_THROWS_AS(renderReport(empty, after, "xml"), std::runtime_error);
}

TEST_CASE("JSON path escaping is deterministic and Markdown paths cannot inject structure") {
  Footprint before;

  before.paths.emplace_back("a\"b\n.md");

  const auto report = renderReport(before, before, "json");

  CHECK(report.find("a\\\"b\\u000a.md") != std::string::npos);
  CHECK(renderReport(before, before, "markdown").find("a\\\"b\\u000a.md") != std::string::npos);
}

TEST_CASE("CLI accepts explicit repeated file options and rejects argument mistakes") {
  const std::array valid = {std::string_view("--before"),   std::string_view("a.cpp"),   std::string_view("--before"),
                            std::string_view("helper.hpp"), std::string_view("--after"), std::string_view("b.cpp"),
                            std::string_view("--format"),   std::string_view("markdown")};
  const auto options = parseArguments(valid);
  const std::array help = {std::string_view("--help")};
  const std::array missingValue = {std::string_view("--before"), std::string_view("--after")};
  const std::array missingSide = {std::string_view("--before"), std::string_view("a.cpp")};
  const std::array badFormat = {std::string_view("--format"), std::string_view("xml")};
  const std::array unknown = {std::string_view("--unknown")};
  const std::array duplicateFormat = {std::string_view("--format"), std::string_view("json"),
                                      std::string_view("--format"), std::string_view("json")};

  CHECK(options.before.size() == 2);
  CHECK(options.after.size() == 1);
  CHECK(options.format == "markdown");
  CHECK(parseArguments(help).help);
  CHECK_THROWS_AS(parseArguments({}), std::runtime_error);
  CHECK_THROWS_AS(parseArguments(missingValue), std::runtime_error);
  CHECK_THROWS_AS(parseArguments(missingSide), std::runtime_error);
  CHECK_THROWS_AS(parseArguments(badFormat), std::runtime_error);
  CHECK_THROWS_AS(parseArguments(unknown), std::runtime_error);
  CHECK_THROWS_AS(parseArguments(duplicateFormat), std::runtime_error);
  CHECK(helpText().find("--before FILE") != std::string_view::npos);
}
