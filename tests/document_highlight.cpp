// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <portals/document_highlight.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace portals;

namespace {

using Role = SemanticRole;
using Status = DocumentHighlightStatus;

struct Corpus {
  std::string text;
  std::vector<Role> expected;

  Corpus &add(Role role, std::string_view part) {
    text += part;
    expected.insert(expected.end(), part.size(), role);

    return *this;
  }
};

std::vector<DecodedScalar> scalars(std::string_view input, std::uint64_t offset = 0, std::uint8_t width = 1) {
  std::vector<DecodedScalar> result;

  for (const auto c : input) {
    result.push_back({static_cast<unsigned char>(c), offset, width, false});
    offset += width;
  }

  return result;
}

struct Capture {
  std::vector<TokenRun> runs;
  Status status{};
  std::size_t consumed{};
};

Capture advance(DocumentHighlighter &lexer, std::span<const DecodedScalar> input, bool eof,
                std::size_t outputCapacity = 64) {
  Capture result;
  std::array<TokenRun, 64> output{};

  for (std::size_t attempts = 0; attempts < input.size() * 8 + 1024; ++attempts) {
    const auto step = lexer.feed(input.subspan(result.consumed), std::span(output).first(outputCapacity), eof);

    result.consumed += step.inputConsumed;
    result.runs.insert(result.runs.end(), output.begin(), output.begin() + step.outputProduced);
    result.status = step.status;

    if (step.status != Status::OUTPUT_FULL && step.status != Status::WORK_LIMIT) {
      return result;
    }

    REQUIRE((step.inputConsumed != 0 || step.outputProduced != 0));
  }

  FAIL("bounded retries failed to finish");

  return result;
}

std::vector<Role> expand(std::span<const TokenRun> runs, std::uint64_t start, std::size_t bytes) {
  std::vector<Role> result;
  std::uint64_t next = start;

  for (const auto &run : runs) {
    REQUIRE(run.byteLength != 0);
    REQUIRE(run.byteOffset == next);
    REQUIRE(run.byteLength <= bytes - result.size());

    result.insert(result.end(), static_cast<std::size_t>(run.byteLength), run.role);
    next += run.byteLength;
  }

  REQUIRE(result.size() == bytes);

  return result;
}

std::vector<Role> highlight(const Corpus &corpus, DocumentLanguage language, std::size_t split,
                            std::size_t capacity = 64, std::size_t workLimit = 4096,
                            DocumentMySqlMode mysqlMode = DocumentMySqlMode::NORMAL,
                            std::uint32_t mysqlVersion = 80400) {
  const auto input = scalars(corpus.text);
  DocumentHighlighter lexer({language, workLimit, 32, 64, mysqlMode, mysqlVersion});
  auto first = advance(lexer, std::span(input).first(split), false, capacity);
  auto second = advance(lexer, std::span(input).subspan(split), true, capacity);

  REQUIRE(first.status == Status::NEED_INPUT);
  REQUIRE(second.status == Status::DONE);
  first.runs.insert(first.runs.end(), second.runs.begin(), second.runs.end());

  return expand(first.runs, 0, corpus.text.size());
}

void everySplit(const Corpus &corpus, DocumentLanguage language,
                DocumentMySqlMode mysqlMode = DocumentMySqlMode::NORMAL, std::uint32_t mysqlVersion = 80400) {
  for (std::size_t split = 0; split <= corpus.text.size(); ++split) {
    CAPTURE(split);
    const auto actual = highlight(corpus, language, split, 64, 4096, mysqlMode, mysqlVersion);

    if (actual != corpus.expected) {
      for (std::size_t i = 0; i < actual.size(); ++i) {
        if (actual[i] != corpus.expected[i]) {
          INFO("first differing byte=" << i << " char=" << corpus.text[i] << " actual=" << static_cast<int>(actual[i])
                                       << " expected=" << static_cast<int>(corpus.expected[i]));
          CHECK(actual[i] == corpus.expected[i]);

          break;
        }
      }
    }

    CHECK(actual == corpus.expected);
    CHECK(highlight(corpus, language, split, 1, 1, mysqlMode, mysqlVersion) == corpus.expected);
  }
}

Corpus sqlGolden() {
  Corpus c;

  c.add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "selective")
    .add(Role::OPERATOR, ",")
    .add(Role::NORMAL, " ")
    .add(Role::NUMBER, "12.5e-3")
    .add(Role::OPERATOR, ",")
    .add(Role::NORMAL, " ")
    .add(Role::STRING, "'a''b\\c'")
    .add(Role::OPERATOR, ",")
    .add(Role::NORMAL, " ")
    .add(Role::STRING, "E'a\\'b'")
    .add(Role::OPERATOR, ",")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "\"a\"\"b\"")
    .add(Role::NORMAL, " ")
    .add(Role::KEYWORD, "FROM")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "t")
    .add(Role::OPERATOR, ";")
    .add(Role::NORMAL, " ")
    .add(Role::COMMENT, "-- SELECT 'x'")
    .add(Role::NORMAL, "\r\n")
    .add(Role::COMMENT, "/* outer\n/* inner */ end */")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "select")
    .add(Role::NORMAL, " ")
    .add(Role::STRING, "$tag$a'$ta$tag$")
    .add(Role::OPERATOR, ",")
    .add(Role::NORMAL, " ")
    .add(Role::STRING, "$$text$$")
    .add(Role::OPERATOR, ";");

  return c;
}

Corpus markdownGolden() {
  Corpus c;

  c.add(Role::NORMAL, "  ")
    .add(Role::HEADING, "## Heading")
    .add(Role::NORMAL, "\ntext ")
    .add(Role::MARKUP, "*")
    .add(Role::NORMAL, "bold")
    .add(Role::MARKUP, "*")
    .add(Role::NORMAL, " ")
    .add(Role::STRING, "``code ` x``")
    .add(Role::NORMAL, " end\n")
    .add(Role::MARKUP, "~~~unknown")
    .add(Role::NORMAL, "\nSELECT 'plain'\n~~\n~~~~x\n")
    .add(Role::MARKUP, "~~~~  ")
    .add(Role::NORMAL, "\n")
    .add(Role::MARKUP, "```sql")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::STRING, "'value'")
    .add(Role::OPERATOR, ";")
    .add(Role::NORMAL, "\n")
    .add(Role::COMMENT, "/* remote\n")
    .add(Role::MARKUP, "``` ")
    .add(Role::NORMAL, "\n")
    .add(Role::HEADING, "# back")
    .add(Role::NORMAL, "\n");

  return c;
}

} // namespace

TEST_CASE("annotated PostgreSQL byte roles are independent of every chunk split") {
  everySplit(sqlGolden(), DocumentLanguage::POSTGRESQL_LEXICAL);
}

TEST_CASE("annotated Markdown source and embedded SQL survive every chunk split") {
  everySplit(markdownGolden(), DocumentLanguage::MARKDOWN_SOURCE);
}

TEST_CASE("COPY data is literal through a standalone terminator") {
  Corpus c;

  c.add(Role::KEYWORD, "COPY")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "t")
    .add(Role::OPERATOR, "(")
    .add(Role::IDENTIFIER, "x")
    .add(Role::OPERATOR, ")")
    .add(Role::NORMAL, " ")
    .add(Role::KEYWORD, "FROM")
    .add(Role::NORMAL, " ")
    .add(Role::KEYWORD, "STDIN")
    .add(Role::OPERATOR, ";")
    .add(Role::NORMAL, "\n")
    .add(Role::STRING, "SELECT '\n\\.x\n\\x\n")
    .add(Role::OPERATOR, "\\.")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::NUMBER, "1")
    .add(Role::OPERATOR, ";");
  everySplit(c, DocumentLanguage::POSTGRESQL_LEXICAL);

  Corpus md;

  md.add(Role::MARKUP, "```sql").add(Role::NORMAL, "\n");
  md.text += c.text;
  md.expected.insert(md.expected.end(), c.expected.begin(), c.expected.end());
  md.add(Role::NORMAL, "\n").add(Role::MARKUP, "```").add(Role::NORMAL, "\n");
  everySplit(md, DocumentLanguage::MARKDOWN_SOURCE);

  Corpus crlf;

  crlf.add(Role::KEYWORD, "COPY")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "t")
    .add(Role::NORMAL, " ")
    .add(Role::KEYWORD, "FROM")
    .add(Role::NORMAL, " ")
    .add(Role::KEYWORD, "STDIN")
    .add(Role::OPERATOR, ";")
    .add(Role::NORMAL, "\r\n")
    .add(Role::STRING, "'/*\r\n")
    .add(Role::OPERATOR, "\\.")
    .add(Role::NORMAL, "\r\n")
    .add(Role::KEYWORD, "SELECT");
  everySplit(crlf, DocumentLanguage::POSTGRESQL_LEXICAL);
}

TEST_CASE("numeric suffix ambiguity and long identifier prefixes stay bounded") {
  Corpus c;

  c.add(Role::NUMBER, "1")
    .add(Role::OPERATOR, ".")
    .add(Role::NORMAL, " ")
    .add(Role::NUMBER, "2")
    .add(Role::IDENTIFIER, "e")
    .add(Role::OPERATOR, "+")
    .add(Role::NORMAL, " ")
    .add(Role::NUMBER, "3.14")
    .add(Role::IDENTIFIER, "efoo")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "SELECTabcdefghijklmnopqrstuvw")
    .add(Role::NORMAL, " ")
    .add(Role::OPERATOR, "$")
    .add(Role::IDENTIFIER, "tag")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "foo$tag$")
    .add(Role::NORMAL, " ")
    .add(Role::STRING, "$$$a$$");
  everySplit(c, DocumentLanguage::POSTGRESQL_LEXICAL);
}

TEST_CASE("Markdown prefixes never promote a midline fence") {
  Corpus c;

  c.add(Role::MARKUP, "~~")
    .add(Role::STRING, "```sql\nSELECT\n```")
    .add(Role::NORMAL, "\n")
    .add(Role::MARKUP, "#######")
    .add(Role::NORMAL, " text\n    ")
    .add(Role::STRING, "```x\ntext\n```")
    .add(Role::NORMAL, "\n")
    .add(Role::HEADING, "#")
    .add(Role::NORMAL, "\n")
    .add(Role::MARKUP, "#")
    .add(Role::NORMAL, "notheading\n")
    .add(Role::MARKUP, "~~~")
    .add(Role::NORMAL, "\n")
    .add(Role::NORMAL, "a\n~~ \n~~~x\n")
    .add(Role::MARKUP, "~~~\t");
  everySplit(c, DocumentLanguage::MARKDOWN_SOURCE);
}

TEST_CASE("all scalar-sized chunks match independent goldens") {
  for (const auto language : {DocumentLanguage::POSTGRESQL_LEXICAL, DocumentLanguage::MARKDOWN_SOURCE}) {
    const auto corpus = language == DocumentLanguage::POSTGRESQL_LEXICAL ? sqlGolden() : markdownGolden();
    const auto input = scalars(corpus.text);
    DocumentHighlighter lexer({language, 1});
    std::vector<TokenRun> runs;

    for (std::size_t i = 0; i < input.size(); ++i) {
      const auto result = advance(lexer, std::span(input).subspan(i, 1), i + 1 == input.size(), 1);

      REQUIRE(result.status == (i + 1 == input.size() ? Status::DONE : Status::NEED_INPUT));
      runs.insert(runs.end(), result.runs.begin(), result.runs.end());
    }

    CHECK(expand(runs, 0, input.size()) == corpus.expected);
  }
}

TEST_CASE("Unicode scalar ranges and replacements preserve their source intervals") {
  const std::array input{DecodedScalar{U'S', 0, 2, false},  DecodedScalar{U'E', 2, 2, false},
                         DecodedScalar{U'L', 4, 2, false},  DecodedScalar{U'E', 6, 2, false},
                         DecodedScalar{U'C', 8, 2, false},  DecodedScalar{U'T', 10, 2, false},
                         DecodedScalar{U' ', 12, 2, false}, DecodedScalar{U'\U0001F680', 14, 4, false},
                         DecodedScalar{U' ', 18, 2, false}, DecodedScalar{U'\uFFFD', 20, 1, true}};
  DocumentHighlighter lexer({DocumentLanguage::POSTGRESQL_LEXICAL, 1});
  const auto result = advance(lexer, input, true, 1);
  const auto roles = expand(result.runs, 0, 21);

  CHECK(roles[0] == Role::KEYWORD);
  CHECK(roles[11] == Role::KEYWORD);
  CHECK(roles[12] == Role::NORMAL);
  CHECK(roles[14] == Role::IDENTIFIER);
  CHECK(roles[17] == Role::IDENTIFIER);
  CHECK(roles[20] == Role::IDENTIFIER);
}

TEST_CASE("remote dollar and single string checkpoints retain lexical role") {
  for (const auto opening : {"$tag$", "'", "E'"}) {
    DocumentHighlighter lexer({DocumentLanguage::POSTGRESQL_LEXICAL, 3});
    const auto prefix = scalars(opening);

    REQUIRE(advance(lexer, prefix, false, 1).status == Status::NEED_INPUT);

    std::uint64_t offset = prefix.size();

    for (int block = 0; block < 64; ++block) {
      const auto input = scalars(std::string(4096, 'x'), offset);

      REQUIRE(advance(lexer, input, false, 1).status == Status::NEED_INPUT);
      offset += input.size();
    }

    DocumentHighlighter restored(lexer.checkpoint());
    const auto suffix = scalars(std::string_view(opening) == "$tag$" ? "$tag$ SELECT 1;" : "' SELECT 1;", offset);
    const auto a = advance(lexer, suffix, true, 1);
    const auto b = advance(restored, suffix, true, 1);

    REQUIRE(a.status == Status::DONE);
    REQUIRE(b.status == Status::DONE);
    CHECK(expand(a.runs, offset, suffix.size()) == expand(b.runs, offset, suffix.size()));
    REQUIRE(!a.runs.empty());
    CHECK(a.runs.front().role == Role::STRING);
  }
}

TEST_CASE("an arbitrarily long potential fence-closing line keeps speculation bounded") {
  for (const auto label : {"sql", "unknown"}) {
    DocumentHighlighter lexer({DocumentLanguage::MARKDOWN_SOURCE, 31});
    const auto prefix = scalars(std::string("```") + label + "\n/*\n``` ");

    REQUIRE(advance(lexer, prefix, false, 1).status == Status::NEED_INPUT);

    std::uint64_t offset = prefix.size();

    for (int block = 0; block < 64; ++block) {
      const auto input = scalars(std::string(4096, ' '), offset);
      const auto result = advance(lexer, input, false, 1);

      REQUIRE(result.status == Status::NEED_INPUT);
      CHECK(result.runs.empty());
      offset += input.size();
    }

    DocumentHighlighter restored(lexer.checkpoint());
    const auto suffix = scalars("x\n*/\n```\n", offset);
    const auto result = advance(restored, suffix, true, 1);

    REQUIRE(result.status == Status::DONE);
    REQUIRE(!result.runs.empty());
    CHECK(result.runs.front().byteOffset == prefix.size() - 4);
    CHECK(result.runs.front().byteLength >= 64 * 4096);
    CHECK(result.runs.front().role == (std::string_view(label) == "sql" ? Role::COMMENT : Role::NORMAL));
  }
}

TEST_CASE("remote checkpoint restores multiline SQL and Markdown state") {
  for (const auto language : {DocumentLanguage::POSTGRESQL_LEXICAL, DocumentLanguage::MARKDOWN_SOURCE}) {
    const auto prefix = language == DocumentLanguage::POSTGRESQL_LEXICAL ? "/* opening\n" : "```sql\n/* opening\n";
    const auto suffix =
      language == DocumentLanguage::POSTGRESQL_LEXICAL ? "closing */ SELECT 1;" : "closing */ SELECT 1;\n```\n";
    DocumentHighlighter lexer({language, 7});
    const auto first = scalars(prefix);

    REQUIRE(advance(lexer, first, false, 1).status == Status::NEED_INPUT);

    std::uint64_t offset = first.size();
    const auto repeated = scalars(std::string(4096, 'x'));

    for (int i = 0; i < 256; ++i) {
      auto block = repeated;

      for (auto &scalar : block) {
        scalar.byteOffset += offset;
      }

      REQUIRE(advance(lexer, block, false, 1).status == Status::NEED_INPUT);
      offset += block.size();
    }

    const auto saved = lexer.checkpoint();
    DocumentHighlighter restored(saved);
    const auto last = scalars(suffix, offset);
    const auto original = advance(lexer, last, true, 1);
    const auto resumed = advance(restored, last, true, 1);

    REQUIRE(original.status == Status::DONE);
    REQUIRE(resumed.status == Status::DONE);
    CHECK(expand(original.runs, offset, last.size()) == expand(resumed.runs, offset, last.size()));
    REQUIRE(!resumed.runs.empty());
    CHECK(resumed.runs.front().role == Role::COMMENT);
  }
}

TEST_CASE("checkpoints preserve pending keyword, dollar delimiter and output queue") {
  for (const auto prefix : {"SEL", "$ta", "'x'", "/*x*", "1e+"}) {
    DocumentHighlighter lexer({DocumentLanguage::POSTGRESQL_LEXICAL, 1});
    const auto input = scalars(prefix);

    REQUIRE(advance(lexer, input, false, 1).status == Status::NEED_INPUT);

    DocumentHighlighter restored(lexer.checkpoint());
    const auto suffix = scalars("ECT tag$body$tag$ 1;", input.size());
    const auto a = advance(lexer, suffix, true, 1);
    const auto b = advance(restored, suffix, true, 1);

    CHECK(a.status == b.status);
    REQUIRE(a.runs.size() == b.runs.size());

    for (std::size_t i = 0; i < a.runs.size(); ++i) {
      CHECK(a.runs[i].byteOffset == b.runs[i].byteOffset);
      CHECK(a.runs[i].byteLength == b.runs[i].byteLength);
      CHECK(a.runs[i].role == b.runs[i].role);
    }
  }

  DocumentHighlighter plain;
  const auto input = scalars("a");
  const auto stalled = plain.feed(input, {}, false);

  REQUIRE(stalled.status == Status::OUTPUT_FULL);
  REQUIRE(stalled.inputConsumed == 1);

  DocumentHighlighter restored(plain.checkpoint());
  const auto result = advance(restored, {}, true, 1);

  REQUIRE(result.status == Status::DONE);
  REQUIRE(result.runs.size() == 1);
  CHECK(result.runs[0].byteOffset == 0);
}

TEST_CASE("arbitrarily long token and line keep owning state constant") {
  static_assert(std::is_trivially_copyable_v<DocumentHighlightCheckpoint>);
  static_assert(std::is_trivially_copyable_v<DocumentHighlighter>);
  static_assert(sizeof(DocumentHighlightCheckpoint) < 2048);
  static_assert(sizeof(DocumentHighlighter) == sizeof(DocumentHighlightCheckpoint));

  for (const auto language :
       {DocumentLanguage::POSTGRESQL_LEXICAL, DocumentLanguage::MARKDOWN_SOURCE, DocumentLanguage::MYSQL_LEXICAL}) {
    DocumentHighlighter lexer({language});
    std::uint64_t offset = 0;

    for (int block = 0; block < 1024; ++block) {
      const auto input = scalars(std::string(4096, 'a'), offset);
      const auto result = advance(lexer, input, false);

      REQUIRE(result.status == Status::NEED_INPUT);
      offset += input.size();
    }

    REQUIRE(advance(lexer, {}, true).status == Status::DONE);
    CHECK(lexer.nextByteOffset() == 4 * 1024 * 1024);
  }
}

TEST_CASE("byte intervals retain wide encoding widths and positions above four GiB") {
  constexpr std::uint64_t START = (std::uint64_t{1} << 32) + 123;
  const auto input = scalars("SELECT 'x';", START, 4);
  DocumentHighlighter lexer({DocumentLanguage::POSTGRESQL_LEXICAL, 2}, START);
  const auto result = advance(lexer, input, true, 1);

  REQUIRE(result.status == Status::DONE);
  const auto roles = expand(result.runs, START, input.size() * 4);

  CHECK(roles.front() == Role::KEYWORD);
  CHECK(roles[7 * 4] == Role::STRING);
  CHECK(lexer.nextByteOffset() == START + input.size() * 4);
}

TEST_CASE("configuration, scalar and offset failures are transactional") {
  for (const auto options : {DocumentHighlighterOptions{DocumentLanguage::PLAIN, 0},
                             DocumentHighlighterOptions{DocumentLanguage::PLAIN, 1, 0},
                             DocumentHighlighterOptions{DocumentLanguage::PLAIN, 1, 65},
                             DocumentHighlighterOptions{DocumentLanguage::PLAIN, 1, 1, 0},
                             DocumentHighlighterOptions{static_cast<DocumentLanguage>(999)}}) {
    DocumentHighlighter lexer(options);
    const auto result = lexer.feed(scalars("x"), {}, true);

    CHECK(result.status == Status::INVALID_CONFIGURATION);
    CHECK(result.inputConsumed == 0);
    CHECK(result.outputProduced == 0);
    CHECK(lexer.nextByteOffset() == 0);
  }

  for (const auto bad : {DecodedScalar{U'x', 1, 1, false}, DecodedScalar{U'x', 0, 0, false},
                         DecodedScalar{0xD800, 0, 1, false}, DecodedScalar{0x110000, 0, 1, false}}) {
    DocumentHighlighter lexer;
    std::array<TokenRun, 4> output{};
    const auto result = lexer.feed(std::span(&bad, 1), output, true);

    CHECK(result.status == Status::INVALID_INPUT);
    CHECK(result.inputConsumed == 0);
    CHECK(lexer.nextByteOffset() == 0);
  }

  const auto offset = std::numeric_limits<std::uint64_t>::max() - 1;
  DocumentHighlighter lexer({}, offset);
  const DecodedScalar bad{U'x', offset, 2, false};
  std::array<TokenRun, 1> output{};
  const auto result = lexer.feed(std::span(&bad, 1), output, true);

  CHECK(result.status == Status::INVALID_INPUT);
  CHECK(lexer.nextByteOffset() == offset);
}

TEST_CASE("delimiter and nesting bounds stop before the failing scalar") {
  for (const auto text : {"$abc", "/* /*", "```"}) {
    const auto language =
      std::string_view(text) == "```" ? DocumentLanguage::MARKDOWN_SOURCE : DocumentLanguage::POSTGRESQL_LEXICAL;
    DocumentHighlighter lexer({language, 4096, 2, 1});
    const auto input = scalars(text);
    const auto result = advance(lexer, input, true, 1);

    CHECK(result.status == Status::RESOURCE_LIMIT);
    CHECK(result.consumed == input.size() - 1);
    CHECK(lexer.nextByteOffset() == input.size() - 1);

    std::array<TokenRun, 4> output{};
    const auto retry = lexer.feed(std::span(input).last(1), output, true);

    CHECK(retry.status == Status::RESOURCE_LIMIT);
    CHECK(retry.inputConsumed == 0);
  }
}

TEST_CASE("unsupported modes are explicit and unknown language is plain") {
  for (const auto text : {"SET ", "DELIMITER ", "B'", "X'", "U&'"}) {
    DocumentHighlighter lexer({DocumentLanguage::POSTGRESQL_LEXICAL});
    const auto result = advance(lexer, scalars(text), true, 1);

    CHECK(result.status == Status::UNSUPPORTED_PROFILE);
  }

  DocumentHighlighter markdown({DocumentLanguage::MARKDOWN_SOURCE});

  CHECK(advance(markdown, scalars("```foo`"), true).status == Status::UNSUPPORTED_PROFILE);
  CHECK(documentLanguageFromName("Sql") == DocumentLanguage::POSTGRESQL_LEXICAL);
  CHECK(documentLanguageFromName("PostgreSQL") == DocumentLanguage::POSTGRESQL_LEXICAL);
  CHECK(documentLanguageFromName("MD") == DocumentLanguage::MARKDOWN_SOURCE);
  CHECK(documentLanguageFromName("unrecognized") == DocumentLanguage::PLAIN);

  Corpus plain;

  plain.add(Role::NORMAL, "SELECT 'x' /*x*/ ```\n");
  everySplit(plain, documentLanguageFromName("unrecognized"));
}

TEST_CASE("Unicode prefix rejection waits for an immediate quote") {
  Corpus ordinary;

  ordinary.add(Role::IDENTIFIER, "u")
    .add(Role::OPERATOR, "&")
    .add(Role::IDENTIFIER, "v")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "U")
    .add(Role::OPERATOR, "&")
    .add(Role::NUMBER, "1")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "u")
    .add(Role::OPERATOR, "&");
  everySplit(ordinary, DocumentLanguage::POSTGRESQL_LEXICAL);

  for (const auto text : {"U&'text'", "U&\"identifier\""}) {
    const auto input = scalars(text);

    for (std::size_t split = 0; split <= input.size(); ++split) {
      DocumentHighlighter lexer({DocumentLanguage::POSTGRESQL_LEXICAL, 1});
      const auto first = advance(lexer, std::span(input).first(split), false, 1);

      if (split <= 2) {
        REQUIRE(first.status == Status::NEED_INPUT);
        CHECK(advance(lexer, std::span(input).subspan(split), true, 1).status == Status::UNSUPPORTED_PROFILE);
      } else {
        CHECK(first.status == Status::UNSUPPORTED_PROFILE);
      }
    }
  }
}

TEST_CASE("newline, EOF, empty output and post-completion contracts are explicit") {
  DocumentHighlighter lexer({DocumentLanguage::POSTGRESQL_LEXICAL, 1});
  const auto input = scalars("SELECT");

  REQUIRE(advance(lexer, input, false, 1).status == Status::NEED_INPUT);

  const auto zero = lexer.feed({}, {}, true);

  CHECK(zero.inputConsumed == 0);
  CHECK(zero.outputProduced == 0);
  CHECK(zero.status == Status::OUTPUT_FULL);
  CHECK(lexer.feed({}, {}, true).status == Status::OUTPUT_FULL);

  const auto finished = advance(lexer, {}, true, 1);

  REQUIRE(finished.status == Status::DONE);
  REQUIRE(finished.runs.size() == 1);
  CHECK(finished.runs[0].role == Role::KEYWORD);
  CHECK(lexer.feed({}, {}, true).status == Status::DONE);
  CHECK(lexer.feed(scalars("x", input.size()), {}, true).status == Status::INVALID_INPUT);
}

TEST_CASE("MySQL annotated quotes comments and version gates survive every split") {
  Corpus c;

  c.add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "`a``b`")
    .add(Role::OPERATOR, ",")
    .add(Role::NORMAL, " ")
    .add(Role::STRING, "\"x\\\"y\"")
    .add(Role::OPERATOR, ",")
    .add(Role::NORMAL, " ")
    .add(Role::STRING, "'a\\'b'")
    .add(Role::MARKUP, ";")
    .add(Role::NORMAL, " ")
    .add(Role::COMMENT, "# comment")
    .add(Role::NORMAL, "\n")
    .add(Role::OPERATOR, "--")
    .add(Role::IDENTIFIER, "x")
    .add(Role::NORMAL, "\n")
    .add(Role::COMMENT, "-- comment")
    .add(Role::NORMAL, "\n")
    .add(Role::COMMENT, "/* ordinary */")
    .add(Role::NORMAL, "\n")
    .add(Role::COMMENT, "/*!40101")
    .add(Role::NORMAL, " ")
    .add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::NUMBER, "1")
    .add(Role::NORMAL, " ")
    .add(Role::COMMENT, "*/")
    .add(Role::MARKUP, ";")
    .add(Role::NORMAL, "\n")
    .add(Role::COMMENT, "/*!99999 SET SQL_MODE='ANSI_QUOTES' */")
    .add(Role::MARKUP, ";")
    .add(Role::NORMAL, "\n")
    .add(Role::COMMENT, "/*+ hint */");
  everySplit(c, DocumentLanguage::MYSQL_LEXICAL);

  Corpus noEscapes;

  noEscapes.add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::STRING, "'a\\'")
    .add(Role::OPERATOR, ",")
    .add(Role::NORMAL, " ")
    .add(Role::STRING, "\"b\\\"")
    .add(Role::OPERATOR, ",")
    .add(Role::NORMAL, " ")
    .add(Role::STRING, "'d''e'")
    .add(Role::MARKUP, ";");
  everySplit(noEscapes, DocumentLanguage::MYSQL_LEXICAL, DocumentMySqlMode::NO_BACKSLASH_ESCAPES);
}

TEST_CASE("MySQL stored routines keep internal semicolons until changed client delimiter") {
  Corpus c;

  c.add(Role::MARKUP, "DELIMITER $$")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "CREATE")
    .add(Role::NORMAL, " ")
    .add(Role::KEYWORD, "PROCEDURE")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "p")
    .add(Role::OPERATOR, "()")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "BEGIN")
    .add(Role::NORMAL, "\n  ")
    .add(Role::KEYWORD, "SET")
    .add(Role::NORMAL, " ")
    .add(Role::OPERATOR, "@")
    .add(Role::IDENTIFIER, "x")
    .add(Role::OPERATOR, "=")
    .add(Role::NUMBER, "1")
    .add(Role::OPERATOR, ";")
    .add(Role::NORMAL, "\n  ")
    .add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::STRING, "'$$'")
    .add(Role::OPERATOR, ";")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "END")
    .add(Role::MARKUP, "$$")
    .add(Role::NORMAL, "\n")
    .add(Role::MARKUP, "delimiter //")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::NUMBER, "1")
    .add(Role::OPERATOR, ";")
    .add(Role::MARKUP, "//")
    .add(Role::NORMAL, "\n")
    .add(Role::MARKUP, "DELIMITER ;")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "CALL")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "p")
    .add(Role::OPERATOR, "()")
    .add(Role::MARKUP, ";");
  everySplit(c, DocumentLanguage::MYSQL_LEXICAL);

  Corpus markdown;

  markdown.add(Role::MARKUP, "```mysql").add(Role::NORMAL, "\n");
  markdown.text += c.text;
  markdown.expected.insert(markdown.expected.end(), c.expected.begin(), c.expected.end());
  markdown.add(Role::NORMAL, "\n").add(Role::MARKUP, "```").add(Role::NORMAL, "\n");
  everySplit(markdown, DocumentLanguage::MARKDOWN_SOURCE);
}

TEST_CASE("MySQL delimiter prefixes overlap and preserve failed punctuation") {
  Corpus c;

  c.add(Role::MARKUP, "DELIMITER !!?")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::NUMBER, "1")
    .add(Role::OPERATOR, "!")
    .add(Role::MARKUP, "!!?")
    .add(Role::NORMAL, "\n")
    .add(Role::MARKUP, "DELIMITER //")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::NUMBER, "2")
    .add(Role::OPERATOR, "/")
    .add(Role::NORMAL, " ")
    .add(Role::OPERATOR, "+")
    .add(Role::NORMAL, " ")
    .add(Role::NUMBER, "3")
    .add(Role::COMMENT, "/*body*/")
    .add(Role::MARKUP, "//")
    .add(Role::NORMAL, "\n")
    .add(Role::MARKUP, "DELIMITER ;");
  everySplit(c, DocumentLanguage::MYSQL_LEXICAL);
}

TEST_CASE("MySQL literal source mode changes and common dump saves restore escapes") {
  Corpus c;

  c.add(Role::COMMENT, "/*!40101")
    .add(Role::NORMAL, " ")
    .add(Role::KEYWORD, "SET")
    .add(Role::NORMAL, " ")
    .add(Role::OPERATOR, "@")
    .add(Role::IDENTIFIER, "OLD_SQL_MODE")
    .add(Role::OPERATOR, "=@@")
    .add(Role::IDENTIFIER, "SQL_MODE")
    .add(Role::OPERATOR, ",")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "SQL_MODE")
    .add(Role::OPERATOR, "=")
    .add(Role::STRING, "'NO_BACKSLASH_ESCAPES,NO_AUTO_VALUE_ON_ZERO'")
    .add(Role::NORMAL, " ")
    .add(Role::COMMENT, "*/")
    .add(Role::MARKUP, ";")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::STRING, "'literal\\'")
    .add(Role::MARKUP, ";")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "SET")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "SQL_MODE")
    .add(Role::OPERATOR, "=@")
    .add(Role::IDENTIFIER, "OLD_SQL_MODE")
    .add(Role::MARKUP, ";")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::STRING, "'escaped\\'quote'")
    .add(Role::MARKUP, ";");
  everySplit(c, DocumentLanguage::MYSQL_LEXICAL);
}

TEST_CASE("MySQL observes mode assignment after an unrelated SET clause") {
  Corpus c;

  c.add(Role::KEYWORD, "SET")
    .add(Role::NORMAL, " ")
    .add(Role::OPERATOR, "@")
    .add(Role::IDENTIFIER, "x")
    .add(Role::OPERATOR, "=")
    .add(Role::NUMBER, "1")
    .add(Role::OPERATOR, ",")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "SQL_MODE")
    .add(Role::OPERATOR, "=")
    .add(Role::STRING, "'NO_BACKSLASH_ESCAPES'")
    .add(Role::MARKUP, ";")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::STRING, "'x\\'")
    .add(Role::MARKUP, ";");
  everySplit(c, DocumentLanguage::MYSQL_LEXICAL);
}

TEST_CASE("MySQL mode changes wait for full client statement and do not execute routine SET") {
  Corpus c;

  c.add(Role::KEYWORD, "SET")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "SESSION")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "SQL_MODE")
    .add(Role::OPERATOR, "=")
    .add(Role::STRING, "'NO_BACKSLASH_ESCAPES'")
    .add(Role::OPERATOR, ",")
    .add(Role::NORMAL, " ")
    .add(Role::OPERATOR, "@")
    .add(Role::IDENTIFIER, "x")
    .add(Role::OPERATOR, "=")
    .add(Role::STRING, "'still\\'escaped'")
    .add(Role::MARKUP, ";")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::STRING, "'now\\'")
    .add(Role::MARKUP, ";")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "SET")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "SQL_MODE")
    .add(Role::OPERATOR, "=")
    .add(Role::STRING, "''")
    .add(Role::MARKUP, ";")
    .add(Role::NORMAL, "\n")
    .add(Role::MARKUP, "DELIMITER //")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "CREATE")
    .add(Role::NORMAL, " ")
    .add(Role::KEYWORD, "PROCEDURE")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "p")
    .add(Role::OPERATOR, "()")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "BEGIN")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "SET")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "SQL_MODE")
    .add(Role::OPERATOR, "=")
    .add(Role::STRING, "'NO_BACKSLASH_ESCAPES'")
    .add(Role::OPERATOR, ";")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::STRING, "'routine\\'quote'")
    .add(Role::OPERATOR, ";")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "END")
    .add(Role::MARKUP, "//")
    .add(Role::NORMAL, "\n")
    .add(Role::MARKUP, "DELIMITER ;")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::STRING, "'outside\\'quote'")
    .add(Role::MARKUP, ";");
  everySplit(c, DocumentLanguage::MYSQL_LEXICAL);
}

TEST_CASE("MySQL scalar-sized chunks and checkpoint roundtrips match annotated roles") {
  Corpus c;

  c.add(Role::MARKUP, "DELIMITER //")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "SET")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "SQL_MODE")
    .add(Role::OPERATOR, "=")
    .add(Role::STRING, "'NO_BACKSLASH_ESCAPES'")
    .add(Role::MARKUP, "//")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "`a``b`")
    .add(Role::OPERATOR, ",")
    .add(Role::NORMAL, " ")
    .add(Role::STRING, "'x\\'")
    .add(Role::MARKUP, "//")
    .add(Role::NORMAL, "\n")
    .add(Role::COMMENT, "/*!40101")
    .add(Role::NORMAL, " ")
    .add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::NUMBER, "1")
    .add(Role::NORMAL, " ")
    .add(Role::COMMENT, "*/")
    .add(Role::MARKUP, "//")
    .add(Role::NORMAL, "\n")
    .add(Role::MARKUP, "DELIMITER ;");
  const auto input = scalars(c.text);
  DocumentHighlighter lexer({DocumentLanguage::MYSQL_LEXICAL, 1});
  std::vector<TokenRun> runs;

  for (std::size_t i = 0; i < input.size(); ++i) {
    lexer = DocumentHighlighter(lexer.checkpoint());
    const auto result = advance(lexer, std::span(input).subspan(i, 1), i + 1 == input.size(), 1);

    REQUIRE(result.status == (i + 1 == input.size() ? Status::DONE : Status::NEED_INPUT));
    runs.insert(runs.end(), result.runs.begin(), result.runs.end());
  }

  CHECK(expand(runs, 0, input.size()) == c.expected);
}

TEST_CASE("MySQL remote checkpoints own comments routine strings mode and directive state") {
  const std::array prefixes{std::string_view{"DELIMITER //\nCREATE PROCEDURE p() BEGIN SELECT '"},
                            std::string_view{"/*!40101 SELECT '"}, std::string_view{"/*!99999 "},
                            std::string_view{"DELIMITER // "},
                            std::string_view{"SET @OLD_SQL_MODE=@@SQL_MODE; SET SQL_MODE='NO_BACKSLASH_ESCAPES'; "}};
  const std::array suffixes{std::string_view{"'; END//\nDELIMITER ;\nSELECT 1;"}, std::string_view{"' */; SELECT 1;"},
                            std::string_view{"*/; SELECT 1;"}, std::string_view{"\nSELECT 1//"},
                            std::string_view{"SET SQL_MODE=@OLD_SQL_MODE; SELECT 'a\\'b';"}};

  for (std::size_t context = 0; context < prefixes.size(); ++context) {
    DocumentHighlighter lexer({DocumentLanguage::MYSQL_LEXICAL, 3});
    const auto prefix = scalars(prefixes[context]);

    REQUIRE(advance(lexer, prefix, false, 1).status == Status::NEED_INPUT);

    std::uint64_t offset = prefix.size();

    for (int block = 0; block < 64; ++block) {
      const auto input = scalars(std::string(4096, context >= 3 ? ' ' : 'x'), offset);

      REQUIRE(advance(lexer, input, false, 1).status == Status::NEED_INPUT);
      offset += input.size();
    }

    DocumentHighlighter restored(lexer.checkpoint());
    const auto suffix = scalars(suffixes[context], offset);
    const auto a = advance(lexer, suffix, true, 1);
    const auto b = advance(restored, suffix, true, 1);

    REQUIRE(a.status == Status::DONE);
    REQUIRE(b.status == Status::DONE);
    CHECK(expand(a.runs, offset, suffix.size()) == expand(b.runs, offset, suffix.size()));
    REQUIRE(!a.runs.empty());
    CHECK(a.runs.front().role == (context <= 1   ? Role::STRING
                                  : context == 2 ? Role::COMMENT
                                  : context == 3 ? Role::NORMAL
                                                 : Role::KEYWORD));
  }
}

TEST_CASE("MySQL version policy selects active SQL without interpreting inactive mode changes") {
  Corpus c;

  c.add(Role::COMMENT, "/*!90000 SET SQL_MODE='ANSI_QUOTES' */")
    .add(Role::MARKUP, ";")
    .add(Role::NORMAL, "\n")
    .add(Role::COMMENT, "/*!1234 SELECT */")
    .add(Role::MARKUP, ";")
    .add(Role::NORMAL, "\n")
    .add(Role::COMMENT, "/*!080400")
    .add(Role::NORMAL, " ")
    .add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::NUMBER, "1")
    .add(Role::NORMAL, " ")
    .add(Role::COMMENT, "*/")
    .add(Role::MARKUP, ";");
  everySplit(c, DocumentLanguage::MYSQL_LEXICAL);

  DocumentHighlighter newer({DocumentLanguage::MYSQL_LEXICAL, 4096, 32, 64, DocumentMySqlMode::NORMAL, 90000});

  CHECK(advance(newer, scalars("/*!90000 SET SQL_MODE='ANSI_QUOTES' */;"), true, 1).status ==
        Status::UNSUPPORTED_PROFILE);
}

TEST_CASE("MySQL unsupported modes directives and resource bounds are explicit") {
  for (const auto text : {"SET SQL_MODE=CONCAT('','');",
                          "SET SQL_MODE=1;",
                          "SET SQL_MODE='' - 1;",
                          "SET SQL_MODE='' / 1;",
                          "SET SQL_MODE=`x`;",
                          "SET SQL_MODE='' -;",
                          "SET SQL_MODE='ANSI_QUOTES';",
                          "SET SQL_MODE='UNKNOWN_MODE';",
                          "SET SQL_MODE=@OLD_SQL_MODE;",
                          "SET GLOBAL SQL_MODE='';",
                          "SET SQL_MODE='' 'NO_BACKSLASH_ESCAPES';",
                          "DELIMITER a",
                          "DELIMITER \\",
                          "DELIMITER $$ x",
                          "DELIMITER\n",
                          "DELIMITER",
                          "/*!1234567 ",
                          "/*!40101X",
                          "/* /*",
                          "/*! SELECT /*",
                          "DELIMITER //\nSELECT 1\nDELIMITER ;"}) {
    CAPTURE(text);
    DocumentHighlighter lexer({DocumentLanguage::MYSQL_LEXICAL, 1});

    CHECK(advance(lexer, scalars(text), true, 1).status == Status::UNSUPPORTED_PROFILE);
  }

  for (const auto text : {"DELIMITER !!!", "SET SQL_MODE='XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX"}) {
    DocumentHighlighter lexer({DocumentLanguage::MYSQL_LEXICAL, 1, 2});

    CHECK(advance(lexer, scalars(text), true, 1).status == Status::RESOURCE_LIMIT);
  }

  for (const auto options :
       {DocumentHighlighterOptions{DocumentLanguage::MYSQL_LEXICAL, 1, 32, 64, static_cast<DocumentMySqlMode>(999)},
        DocumentHighlighterOptions{DocumentLanguage::MYSQL_LEXICAL, 1, 32, 64, DocumentMySqlMode::NORMAL, 0},
        DocumentHighlighterOptions{DocumentLanguage::MYSQL_LEXICAL, 1, 32, 64, DocumentMySqlMode::NORMAL, 1000000}}) {
    DocumentHighlighter lexer(options);
    const auto result = lexer.feed(scalars("SELECT"), {}, true);

    CHECK(result.status == Status::INVALID_CONFIGURATION);
    CHECK(result.inputConsumed == 0);
    CHECK(result.outputProduced == 0);
  }

  CHECK(documentLanguageFromName("MySQL") == DocumentLanguage::MYSQL_LEXICAL);
}

TEST_CASE("MySQL overlapping dashes retain the possible comment suffix") {
  Corpus c;

  c.add(Role::OPERATOR, "-")
    .add(Role::COMMENT, "-- comment")
    .add(Role::NORMAL, "\n")
    .add(Role::OPERATOR, "---")
    .add(Role::COMMENT, "--\tcomment")
    .add(Role::NORMAL, "\r\n")
    .add(Role::OPERATOR, "---")
    .add(Role::IDENTIFIER, "x")
    .add(Role::NORMAL, "\n")
    .add(Role::OPERATOR, "--");
  everySplit(c, DocumentLanguage::MYSQL_LEXICAL);
}

TEST_CASE("MySQL long overlapping delimiter prefix retains fixed widths and high source offsets") {
  const std::string delimiter = std::string(63, '!') + '?';
  const auto text = std::string("DELIMITER ") + delimiter + "\nSELECT 1" + std::string(64, '!') + '?';
  constexpr std::uint64_t START = (std::uint64_t{1} << 33) + 321;
  const auto input = scalars(text, START, 4);
  DocumentHighlighter lexer({DocumentLanguage::MYSQL_LEXICAL, 1, 64}, START);
  const auto result = advance(lexer, input, true, 1);
  const auto roles = expand(result.runs, START, input.size() * 4);
  const auto overlap = text.size() - 65;

  REQUIRE(result.status == Status::DONE);
  CHECK(roles[overlap * 4] == Role::OPERATOR);
  CHECK(roles[overlap * 4 + 3] == Role::OPERATOR);
  CHECK(roles[(overlap + 1) * 4] == Role::MARKUP);
  CHECK(roles.back() == Role::MARKUP);
}

TEST_CASE("MySQL does not inherit PostgreSQL COPY and STDIN keywords") {
  Corpus c;

  c.add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "copy")
    .add(Role::OPERATOR, ",")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "stdin")
    .add(Role::OPERATOR, ",")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "delimiter")
    .add(Role::MARKUP, ";");
  everySplit(c, DocumentLanguage::MYSQL_LEXICAL);
}

TEST_CASE("MySQL directive overflow is a retry-stable transactional error") {
  DocumentHighlighter lexer({DocumentLanguage::MYSQL_LEXICAL, 1, 2});
  const auto prefix = scalars("DELIMITER !!");

  REQUIRE(advance(lexer, prefix, false, 1).status == Status::NEED_INPUT);

  DocumentHighlighter restored(lexer.checkpoint());
  const auto suffix = scalars("!", prefix.size());
  std::array<TokenRun, 8> output{};

  for (auto *scanner : {&lexer, &restored}) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      const auto result = scanner->feed(suffix, output, true);

      CHECK(result.status == Status::RESOURCE_LIMIT);
      CHECK(result.inputConsumed == 0);
      CHECK(result.outputProduced == 0);
      CHECK(result.errorOffset == prefix.size());
      CHECK(scanner->nextByteOffset() == prefix.size());
    }
  }
}

TEST_CASE("MySQL review quoted mode targets are explicitly unsupported") {
  for (const auto text :
       {"SET `SQL_MODE`='NO_BACKSLASH_ESCAPES'; SELECT 'x\\';", "SET SESSION `SQL_MODE`='NO_BACKSLASH_ESCAPES';",
        "SET @@SESSION.`SQL_MODE`='NO_BACKSLASH_ESCAPES';"}) {
    const auto input = scalars(text);
    const auto targetOffset = std::string_view(text).find('`');

    for (std::size_t split = 0; split <= input.size(); ++split) {
      DocumentHighlighter lexer({DocumentLanguage::MYSQL_LEXICAL, 1});
      const auto first = advance(lexer, std::span(input).first(split), false, 1);

      if (first.status == Status::UNSUPPORTED_PROFILE) {
        CHECK(first.consumed == targetOffset);
        CHECK(lexer.nextByteOffset() == targetOffset);

        continue;
      }

      REQUIRE(first.status == Status::NEED_INPUT);
      const auto second = advance(lexer, std::span(input).subspan(split), true, 1);

      CHECK(second.status == Status::UNSUPPORTED_PROFILE);
      CHECK(first.consumed + second.consumed == targetOffset);
      CHECK(lexer.nextByteOffset() == targetOffset);
    }

    DocumentHighlighter lexer({DocumentLanguage::MYSQL_LEXICAL, 1});

    REQUIRE(advance(lexer, std::span(input).first(targetOffset), false, 1).status == Status::NEED_INPUT);

    DocumentHighlighter restored(lexer.checkpoint());
    std::array<TokenRun, 8> output{};

    for (auto *scanner : {&lexer, &restored}) {
      for (int attempt = 0; attempt < 2; ++attempt) {
        const auto result = scanner->feed(std::span(input).subspan(targetOffset, 1), output, false);

        CHECK(result.status == Status::UNSUPPORTED_PROFILE);
        CHECK(result.inputConsumed == 0);
        CHECK(result.outputProduced == 0);
        CHECK(result.errorOffset == targetOffset);
        CHECK(scanner->nextByteOffset() == targetOffset);
      }
    }
  }
}

TEST_CASE("MySQL review variable and quoted SET names cannot change source mode") {
  Corpus c;

  c.add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::OPERATOR, "@")
    .add(Role::IDENTIFIER, "set")
    .add(Role::OPERATOR, ",")
    .add(Role::NORMAL, " ")
    .add(Role::OPERATOR, "@@")
    .add(Role::IDENTIFIER, "SQL_MODE")
    .add(Role::OPERATOR, "=")
    .add(Role::STRING, "'NO_BACKSLASH_ESCAPES'")
    .add(Role::MARKUP, ";")
    .add(Role::NORMAL, " ")
    .add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::STRING, "'a\\'b'")
    .add(Role::MARKUP, ";")
    .add(Role::NORMAL, "\n")
    .add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::IDENTIFIER, "`set`")
    .add(Role::OPERATOR, ",")
    .add(Role::NORMAL, " ")
    .add(Role::OPERATOR, "@@")
    .add(Role::IDENTIFIER, "SQL_MODE")
    .add(Role::OPERATOR, "=")
    .add(Role::STRING, "'NO_BACKSLASH_ESCAPES'")
    .add(Role::MARKUP, ";")
    .add(Role::NORMAL, " ")
    .add(Role::KEYWORD, "SELECT")
    .add(Role::NORMAL, " ")
    .add(Role::STRING, "'a\\'b'")
    .add(Role::MARKUP, ";");
  everySplit(c, DocumentLanguage::MYSQL_LEXICAL);

  const auto input = scalars(c.text);
  const auto variable = c.text.find('@');

  for (const auto boundary : {variable + 1, variable + 3, c.text.find("@@SQL_MODE") + 2}) {
    DocumentHighlighter lexer({DocumentLanguage::MYSQL_LEXICAL, 1});
    auto prefix = advance(lexer, std::span(input).first(boundary), false, 1);

    REQUIRE(prefix.status == Status::NEED_INPUT);

    DocumentHighlighter restored(lexer.checkpoint());
    const auto suffix = advance(restored, std::span(input).subspan(boundary), true, 1);

    REQUIRE(suffix.status == Status::DONE);
    prefix.runs.insert(prefix.runs.end(), suffix.runs.begin(), suffix.runs.end());
    CHECK(expand(prefix.runs, 0, input.size()) == c.expected);
  }
}
