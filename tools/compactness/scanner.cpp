// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "compactness.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <stdexcept>

namespace portals::compactness {
namespace {

bool isLetter(char byte) {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || byte == '_';
}

bool isDigit(char byte) {
  return byte >= '0' && byte <= '9';
}

bool isIdentifierByte(char byte) {
  return isLetter(byte) || isDigit(byte);
}

bool isHorizontalSpace(char byte) {
  return byte == ' ' || byte == '\t' || byte == '\v' || byte == '\f';
}

void validateUtf8(std::string_view source) {
  for (std::size_t index = 0; index < source.size();) {
    const auto first = static_cast<unsigned char>(source[index++]);

    if (first == 0) {
      throw std::runtime_error("NUL byte is unsupported");
    }

    if (first < 0x80) {
      continue;
    }

    const unsigned length = first >= 0xC2 && first <= 0xDF   ? 2
                            : first >= 0xE0 && first <= 0xEF ? 3
                            : first >= 0xF0 && first <= 0xF4 ? 4
                                                             : 0;

    if (length == 0 || source.size() - index < length - 1) {
      throw std::runtime_error("invalid UTF-8 input");
    }

    std::uint32_t value = first & (0x7FU >> length);

    for (unsigned continuation = 1; continuation < length; ++continuation) {
      const auto byte = static_cast<unsigned char>(source[index++]);

      if ((byte & 0xC0U) != 0x80U) {
        throw std::runtime_error("invalid UTF-8 continuation");
      }

      value = (value << 6U) | (byte & 0x3FU);
    }

    if ((length == 3 && value < 0x800) || (length == 4 && value < 0x10000) || value > 0x10FFFF ||
        (value >= 0xD800 && value <= 0xDFFF)) {
      throw std::runtime_error("invalid UTF-8 code point");
    }
  }
}

// Phase-2 splices need physical offsets for line measurements and raw-string restoration.
struct Source {
  std::string physical;
  std::string logical;
  std::vector<std::size_t> offsets;
  std::vector<std::size_t> lines;
  std::vector<bool> code;
  std::uint64_t maxLineBytes{};

  explicit Source(std::string_view input) {
    validateUtf8(input);

    if (input.starts_with("\xEF\xBB\xBF")) {
      input.remove_prefix(3);
    }

    for (std::size_t index = 0; index < input.size(); ++index) {
      const char byte = input[index];

      if (byte == '\r') {
        physical.push_back('\n');

        if (index + 1 < input.size() && input[index + 1] == '\n') {
          ++index;
        }
      } else {
        physical.push_back(byte);
      }
    }

    std::size_t line = 0;
    std::uint64_t lineBytes = 0;

    for (const char byte : physical) {
      lines.push_back(line);

      if (byte == '\n') {
        maxLineBytes = std::max(maxLineBytes, lineBytes);
        lineBytes = 0;
        ++line;
      } else {
        ++lineBytes;
      }
    }

    maxLineBytes = std::max(maxLineBytes, lineBytes);
    code.resize(line + 1);

    for (std::size_t index = 0; index < physical.size(); ++index) {
      if (physical[index] == '\\') {
        std::size_t next = index + 1;

        while (next < physical.size() && isHorizontalSpace(physical[next])) {
          ++next;
        }

        if (next < physical.size() && physical[next] == '\n') {
          index = next;

          continue;
        }
      }

      logical.push_back(physical[index]);
      offsets.push_back(index);
    }

    offsets.push_back(physical.size());
  }

  void markCode(std::size_t begin, std::size_t end) {
    const auto physicalEnd = end > begin ? offsets[end - 1] + 1 : offsets[begin];

    for (auto offset = offsets[begin]; offset < physicalEnd; ++offset) {
      code[lines[offset]] = true;
    }
  }

  std::uint64_t spellingBytes(std::size_t begin, std::size_t end) const {
    return end > begin ? offsets[end - 1] + 1 - offsets[begin] : 0;
  }
};

struct Literal {
  std::size_t quote{};
  bool raw{};
  bool string{};
};

std::optional<Literal> findLiteral(std::string_view text, std::size_t begin) {
  constexpr std::array PREFIXES = {std::string_view("u8R\""), std::string_view("uR\""), std::string_view("UR\""),
                                   std::string_view("LR\""),  std::string_view("R\""),  std::string_view("u8\""),
                                   std::string_view("u\""),   std::string_view("U\""),  std::string_view("L\""),
                                   std::string_view("\""),    std::string_view("u8'"),  std::string_view("u'"),
                                   std::string_view("U'"),    std::string_view("L'"),   std::string_view("'")};

  for (const auto prefix : PREFIXES) {
    if (text.substr(begin).starts_with(prefix)) {
      return Literal{begin + prefix.size() - 1, prefix.find('R') != std::string_view::npos, prefix.back() == '"'};
    }
  }

  return std::nullopt;
}

std::size_t scanRaw(const Source &source, const Literal &literal) {
  constexpr std::size_t MAX_RAW_DELIMITER_BYTES = 16;
  const auto quote = source.offsets[literal.quote];
  const auto open = source.physical.find('(', quote + 1);

  if (open == std::string::npos || open - quote - 1 > MAX_RAW_DELIMITER_BYTES) {
    throw std::runtime_error("invalid raw string delimiter");
  }

  const auto delimiter = source.physical.substr(quote + 1, open - quote - 1);

  for (const unsigned char byte : delimiter) {
    if (byte <= ' ' || byte >= 0x7F || byte == '(' || byte == ')' || byte == '\\') {
      throw std::runtime_error("invalid raw string delimiter");
    }
  }

  const auto close = source.physical.find(")" + delimiter + "\"", open + 1);

  if (close == std::string::npos) {
    throw std::runtime_error("unterminated raw string literal");
  }

  const auto physicalEnd = close + delimiter.size() + 2;

  return static_cast<std::size_t>(std::lower_bound(source.offsets.begin(), source.offsets.end(), physicalEnd) -
                                  source.offsets.begin());
}

std::size_t scanLiteral(const Source &source, const Literal &literal) {
  std::size_t end = literal.quote + 1;

  if (literal.raw) {
    end = scanRaw(source, literal);
  } else {
    const char quote = source.logical[literal.quote];
    bool closed = false;

    while (end < source.logical.size()) {
      const char byte = source.logical[end++];

      if (byte == '\n') {
        throw std::runtime_error("newline in ordinary literal");
      }

      if (byte == quote) {
        closed = true;

        break;
      }

      if (byte == '\\') {
        if (end == source.logical.size()) {
          break;
        }

        ++end;
      }
    }

    if (!closed || (!literal.string && end == literal.quote + 2)) {
      throw std::runtime_error("unterminated or empty character literal");
    }
  }

  if (end < source.logical.size() && isLetter(source.logical[end])) {
    ++end;

    while (end < source.logical.size() && isIdentifierByte(source.logical[end])) {
      ++end;
    }
  }

  return end;
}

std::size_t scanNumber(std::string_view text, std::size_t begin) {
  std::size_t end = begin + 1;

  while (end < text.size()) {
    const char byte = text[end];
    const char previous = text[end - 1];

    if (isIdentifierByte(byte) || byte == '.' ||
        ((byte == '+' || byte == '-') && (previous == 'e' || previous == 'E' || previous == 'p' || previous == 'P'))) {
      ++end;
    } else if (byte == '\'' && end + 1 < text.size() && isIdentifierByte(text[end + 1])) {
      end += 2;
    } else {
      break;
    }
  }

  return end;
}

std::string_view scanPunctuation(std::string_view text) {
  // Ordering implements maximal munch. The standard's <:: exception is deliberately rejected.
  constexpr std::array PUNCTUATORS = {
    std::string_view("%:%:"), std::string_view("<<="), std::string_view(">>="), std::string_view("<=>"),
    std::string_view("..."),  std::string_view("->*"), std::string_view("##"),  std::string_view("::"),
    std::string_view(".*"),   std::string_view("->"),  std::string_view("++"),  std::string_view("--"),
    std::string_view("<<"),   std::string_view(">>"),  std::string_view("<="),  std::string_view(">="),
    std::string_view("=="),   std::string_view("!="),  std::string_view("&&"),  std::string_view("||"),
    std::string_view("*="),   std::string_view("/="),  std::string_view("%="),  std::string_view("+="),
    std::string_view("-="),   std::string_view("&="),  std::string_view("^="),  std::string_view("|="),
    std::string_view("<:"),   std::string_view(":>"),  std::string_view("<%"),  std::string_view("%>"),
    std::string_view("%:")};

  if (text.starts_with("<::")) {
    throw std::runtime_error("unsupported <:: lexical corner case");
  }

  for (const auto punctuation : PUNCTUATORS) {
    if (text.starts_with(punctuation)) {
      return punctuation;
    }
  }

  if (std::string_view("{}[]();:?~!+-*/%^&|=<>.,#").find(text.front()) != std::string_view::npos) {
    return text.substr(0, 1);
  }

  throw std::runtime_error("unsupported source byte outside comments/literals (Unicode/UCN identifiers unsupported)");
}

void mergeMetrics(Metrics &total, const Metrics &file) {
  total.files += file.files;
  total.sourceBytes += file.sourceBytes;
  total.lexicalTokensApprox += file.lexicalTokensApprox;
  total.codeLines += file.codeLines;
  total.preprocessorDirectives += file.preprocessorDirectives;
  total.preprocessorTokens += file.preprocessorTokens;
  total.commentBytes += file.commentBytes;
  total.stringLiteralBytes += file.stringLiteralBytes;
  total.maxLineBytes = std::max(total.maxLineBytes, file.maxLineBytes);
  total.maxBraceNesting = std::max(total.maxBraceNesting, file.maxBraceNesting);
}

} // namespace

Metrics measureSource(std::string_view source) {
  Source input(source);
  const std::string_view text = input.logical;
  Metrics metrics;
  std::size_t cursor = 0;
  std::uint64_t braceDepth = 0;
  bool lineStart = true;
  bool directive = false;
  bool directiveName = false;
  bool headerExpected = false;

  metrics.files = 1;
  metrics.sourceBytes = source.size();
  metrics.maxLineBytes = input.maxLineBytes;

  while (cursor < text.size()) {
    const char byte = text[cursor];

    if (byte == '\n') {
      lineStart = true;
      directive = false;
      directiveName = false;
      headerExpected = false;
      ++cursor;

      continue;
    }

    if (isHorizontalSpace(byte)) {
      ++cursor;

      continue;
    }

    const auto begin = cursor;

    if (text.substr(cursor).starts_with("//") || text.substr(cursor).starts_with("/*")) {
      const bool block = text[cursor + 1] == '*';
      const auto close = block ? text.find("*/", cursor + 2) : text.find('\n', cursor + 2);

      if (block && close == std::string_view::npos) {
        throw std::runtime_error("unterminated block comment");
      }

      cursor = close == std::string_view::npos ? text.size() : close + (block ? 2 : 0);
      metrics.commentBytes += input.spellingBytes(begin, cursor);

      // A block comment acts as whitespace; its physical newlines do not end a directive.
      // The terminating newline of a line comment is handled by the main loop.
      continue;
    }

    std::string_view punctuation;

    if (headerExpected && (byte == '<' || byte == '"')) {
      const char closeByte = byte == '<' ? '>' : '"';

      ++cursor;

      while (cursor < text.size() && text[cursor] != closeByte && text[cursor] != '\n') {
        ++cursor;
      }

      if (cursor == text.size() || text[cursor] != closeByte || cursor == begin + 1) {
        throw std::runtime_error("malformed include header name");
      }

      ++cursor;
      headerExpected = false;
    } else if (const auto literal = findLiteral(text, cursor)) {
      cursor = scanLiteral(input, *literal);

      if (literal->string) {
        metrics.stringLiteralBytes += input.spellingBytes(begin, cursor);
      }

      headerExpected = false;
    } else if (isLetter(byte)) {
      ++cursor;

      while (cursor < text.size() && isIdentifierByte(text[cursor])) {
        ++cursor;
      }

      if (directiveName) {
        const auto name = text.substr(begin, cursor - begin);

        headerExpected = name == "include" || name == "include_next";
        directiveName = false;
      } else {
        headerExpected = false;
      }
    } else if (isDigit(byte) || (byte == '.' && cursor + 1 < text.size() && isDigit(text[cursor + 1]))) {
      cursor = scanNumber(text, cursor);
      headerExpected = false;
    } else {
      punctuation = scanPunctuation(text.substr(cursor));
      cursor += punctuation.size();
      headerExpected = false;
    }

    if (lineStart && (punctuation == "#" || punctuation == "%:")) {
      directive = true;
      directiveName = true;
      ++metrics.preprocessorDirectives;
    }

    ++metrics.lexicalTokensApprox;
    input.markCode(begin, cursor);
    lineStart = false;

    if (directive) {
      ++metrics.preprocessorTokens;
    } else if (punctuation == "{" || punctuation == "<%") {
      ++braceDepth;
      metrics.maxBraceNesting = std::max(metrics.maxBraceNesting, braceDepth);
    } else if ((punctuation == "}" || punctuation == "%>") && braceDepth > 0) {
      --braceDepth;
    }
  }

  metrics.codeLines = static_cast<std::uint64_t>(std::count(input.code.begin(), input.code.end(), true));

  return metrics;
}

Footprint measureFiles(std::span<const std::filesystem::path> paths) {
  if (paths.empty()) {
    throw std::runtime_error("a footprint must contain at least one explicit file");
  }

  std::set<std::filesystem::path> unique;

  for (const auto &path : paths) {
    unique.insert(std::filesystem::canonical(path));
  }

  Footprint result;

  for (const auto &path : unique) {
    try {
      if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error("input is not a regular file");
      }

      std::ifstream file(path, std::ios::binary);

      if (!file) {
        throw std::runtime_error("cannot open input file");
      }

      const std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};

      if (file.bad()) {
        throw std::runtime_error("cannot read input file");
      }

      mergeMetrics(result.metrics, measureSource(bytes));
      result.paths.push_back(path);
    } catch (const std::exception &error) {
      throw std::runtime_error(path.string() + ": " + error.what());
    }
  }

  return result;
}

} // namespace portals::compactness
