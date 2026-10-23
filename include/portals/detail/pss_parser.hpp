// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/pss_data.hpp>

namespace portals::detail {
/** @brief Admission policy; both modes share the complete PSS v0 grammar. */
enum class PssPolicy {
  LEGACY,     ///< Preserve parsePss byte admission, including arbitrary comment bytes and rejected BOM.
  STRICT_UTF8 ///< Validate all UTF-8 bytes and NUL, then accept one leading BOM.
};

/** @brief Borrowed byte range used only within a synchronous parser invocation. */
struct PssSpan {
  std::size_t offset{}; ///< Start in the original source.
  std::size_t size{};   ///< Number of bytes.
};

/** @brief Small validated scalar payload; avoids constructing six optional fields per parsed occurrence. */
using PssValue = PssScalarValue;

/** @brief Static string views avoid repeated C-string length evaluation in the constexpr hot path. */
inline constexpr std::array<std::string_view, PSS_PROPERTY_COUNT> PSS_PROPERTY_NAMES{"foreground",
                                                                                     "background",
                                                                                     "bold",
                                                                                     "italic",
                                                                                     "underline",
                                                                                     "min-width",
                                                                                     "frame-line-foreground",
                                                                                     "frame-line-background",
                                                                                     "frame-line-bold",
                                                                                     "frame-line-italic",
                                                                                     "frame-line-underline",
                                                                                     "frame-title-foreground",
                                                                                     "frame-title-background",
                                                                                     "frame-title-bold",
                                                                                     "frame-title-italic",
                                                                                     "frame-title-underline",
                                                                                     "frame-ends-foreground",
                                                                                     "frame-ends-background",
                                                                                     "frame-ends-bold",
                                                                                     "frame-ends-italic",
                                                                                     "frame-ends-underline"};
/** @brief Pseudo-state names in the existing StyleState order after ANY. */
inline constexpr std::array<std::string_view, 6> PSS_STATE_NAMES{"normal",   "focused", "disabled",
                                                                 "selected", "header",  "caret"};

/** @param code Specific error category.
 * @param location Original source coordinates.
 * @param message Static diagnostic string.
 * @param subject Borrowed bytes copied into a bounded owning ASCII subject.
 * @return Allocation-free owning diagnostic.
 */
constexpr PssError pssError(PssErrorCode code, PssLocation location, std::string_view message,
                            std::string_view subject = {}) {
  PssError result;

  result.code = code;
  result.location = location;
  result.message = message;
  result.subjectSize = std::min(subject.size(), result.subject.size());
  result.subjectTruncated = subject.size() > result.subject.size();

  for (std::size_t index = 0; index < result.subjectSize; ++index) {
    const auto value = static_cast<unsigned char>(subject[index]);

    result.subject[index] = value >= 32 && value < 127 ? subject[index] : '?';
  }

  return result;
}

/** @param location Source position updated for one byte.
 * @param value Consumed byte; only LF starts another line.
 */
constexpr void pssAdvance(PssLocation &location, char value) {
  ++location.offset;

  if (value == '\n') {
    ++location.line;
    location.column = 1;
  } else {
    ++location.column;
  }
}

/** @param source Borrowed bytes validated without allocation.
 * @return Success or the coordinates of the first invalid sequence or NUL.
 * Strict decoding rejects overlong encodings, surrogate code points, and values above U+10FFFF.
 */
constexpr std::expected<void, PssError> validatePssUtf8(std::string_view source) {
  PssLocation location;

  while (location.offset < source.size()) {
    const auto first = static_cast<unsigned char>(source[location.offset]);

    if (first == 0) {
      return std::unexpected(pssError(PssErrorCode::NUL_BYTE, location, "PSS input contains NUL"));
    }

    if (first < 0x80) {
      pssAdvance(location, source[location.offset]);

      continue;
    }

    const auto begin = location;
    std::size_t count{};
    unsigned codepoint{};
    unsigned minimum{};

    if (first >= 0xc2 && first <= 0xdf) {
      count = 2;
      codepoint = first & 0x1f;
      minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      count = 3;
      codepoint = first & 0x0f;
      minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      count = 4;
      codepoint = first & 0x07;
      minimum = 0x10000;
    } else {
      return std::unexpected(pssError(PssErrorCode::INVALID_UTF8, begin, "Invalid UTF-8 in PSS input"));
    }

    if (count > source.size() - location.offset) {
      return std::unexpected(pssError(PssErrorCode::INVALID_UTF8, begin, "Invalid UTF-8 in PSS input"));
    }

    for (std::size_t index = 1; index < count; ++index) {
      const auto value = static_cast<unsigned char>(source[location.offset + index]);

      if (value < 0x80 || value > 0xbf) {
        return std::unexpected(pssError(PssErrorCode::INVALID_UTF8, begin, "Invalid UTF-8 in PSS input"));
      }

      codepoint = (codepoint << 6) | (value & 0x3f);
    }

    if (codepoint < minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
      return std::unexpected(pssError(PssErrorCode::INVALID_UTF8, begin, "Invalid UTF-8 in PSS input"));
    }

    for (std::size_t index = 0; index < count; ++index) {
      pssAdvance(location, source[location.offset]);
    }
  }

  return {};
}

/** @param value Byte tested against the v0 ASCII identifier-start grammar.
 * @return Whether it starts an identifier.
 */
constexpr bool pssLetter(char value) {
  return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
}

/** @param value Byte tested against ASCII decimal digits.
 * @return Whether it is a decimal digit.
 */
constexpr bool pssDigit(char value) {
  return value >= '0' && value <= '9';
}

/** @param value Byte tested against v0 whitespace.
 * @return Whether it separates syntax outside selector suffixes and values.
 */
constexpr bool pssSpace(char value) {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

/** @param name Candidate whole ASCII name.
 * @return Whether it follows the exact PSS v0 identifier grammar.
 */
constexpr bool isPssIdentifier(std::string_view name) {
  if (name.empty() || !pssLetter(name.front())) {
    return false;
  }

  for (const auto value : name.substr(1)) {
    if (!pssLetter(value) && !pssDigit(value) && value != '-') {
      return false;
    }
  }

  return true;
}

/** @brief Shared allocation-free grammar kernel; sink calls are synchronous and may throw.
 * @tparam Sink Supplies rule/selector/declaration callbacks returning expected<void,PssError>.
 * Runtime sinks may allocate; their wrapper catches allocation failure around the entire operation.
 * No source view or callback is retained after parse() returns.
 */
template <class Sink> class PssParser {
public:
  /** @param source Borrowed bytes retained only for this parser invocation.
   * @param sink Borrowed synchronous destination; receives original source spans.
   * @param policy Legacy byte compatibility or strict UTF-8 admission.
   * @param maxBytes Input byte admission budget.
   * @param maxRules Source-order rule admission budget.
   */
  constexpr PssParser(std::string_view source, Sink &sink, PssPolicy policy, std::size_t maxBytes = MAX_PSS_BYTES,
                      std::size_t maxRules = MAX_STYLE_RULES)
      : source_(source), sourceData_(source.data()), sourceSize_(source.size()), sink_(sink), policy_(policy),
        maxBytes_(maxBytes), maxRules_(maxRules) {
  }

  /** @return Grammar/admission result; sink exceptions propagate to the owning runtime wrapper. */
  constexpr std::expected<void, PssError> parse() {
    if (source_.size() > maxBytes_) {
      return std::unexpected(pssError(PssErrorCode::INPUT_LIMIT, {}, "PSS source exceeds byte budget"));
    }

    if (policy_ == PssPolicy::STRICT_UTF8) {
      auto valid = validatePssUtf8(source_);

      if (!valid) {
        return valid;
      }

      if (source_.starts_with("\xef\xbb\xbf")) {
        advance();
        advance();
        advance();
      }
    }

    if (!skipSpace()) {
      return failure();
    }

    while (location_.offset < sourceSize_) {
      if (rules_ == maxRules_) {
        fail(PssErrorCode::RULE_LIMIT, "Too many style rules");

        return failure();
      }

      if (!admit(sink_.beginRule(location_)) || !parseSelector() || !expect('{')) {
        return failure();
      }

      while (peek() != '}') {
        if (!parseDeclaration()) {
          return failure();
        }
      }

      if (!expect('}') || !admit(sink_.endRule())) {
        return failure();
      }

      ++rules_;
    }

    return {};
  }

private:
  constexpr char peek() const {
    return location_.offset < sourceSize_ ? sourceData_[location_.offset] : '\0';
  }

  constexpr bool nextIs(char first, char second) const {
    return location_.offset < sourceSize_ && sourceSize_ - location_.offset >= 2 &&
           sourceData_[location_.offset] == first && sourceData_[location_.offset + 1] == second;
  }

  constexpr std::string_view view(PssSpan span) const {
    return source_.substr(span.offset, span.size);
  }

  constexpr void advance() {
    pssAdvance(location_, peek());
  }

  constexpr bool fail(PssErrorCode code, std::string_view message, std::string_view subject = {}) {
    error_ = pssError(code, location_, message, subject);

    return false;
  }

  constexpr bool failAt(PssErrorCode code, PssLocation location, std::string_view message,
                        std::string_view subject = {}) {
    error_ = pssError(code, location, message, subject);

    return false;
  }

  constexpr std::expected<void, PssError> failure() const {
    return std::unexpected(error_);
  }

  constexpr bool admit(std::expected<void, PssError> result) {
    if (!result) {
      error_ = result.error();

      return false;
    }

    return true;
  }

  constexpr bool skipSpace() {
    for (;;) {
      while (pssSpace(peek())) {
        advance();
      }

      if (!nextIs('/', '*')) {
        return true;
      }

      advance();
      advance();

      while (!nextIs('*', '/')) {
        if (location_.offset == sourceSize_) {
          return fail(PssErrorCode::UNTERMINATED_COMMENT, "Unterminated comment");
        }

        advance();
      }

      advance();
      advance();
    }
  }

  constexpr bool expect(char expected) {
    if (peek() != expected) {
      return fail(PssErrorCode::EXPECTED_PUNCTUATION, "Expected punctuation");
    }

    advance();

    return skipSpace();
  }

  constexpr std::optional<PssSpan> identifier() {
    if (!pssLetter(peek())) {
      fail(PssErrorCode::EXPECTED_IDENTIFIER, "Expected ASCII identifier");

      return {};
    }

    const auto begin = location_.offset;

    for (;;) {
      const auto current = peek();

      if (!pssLetter(current) && !pssDigit(current) && current != '-') {
        break;
      }

      advance();
    }

    return PssSpan{begin, location_.offset - begin};
  }

  constexpr bool parseState() {
    const auto begin = location_;
    const auto name = identifier();

    if (!name) {
      return false;
    }

    for (std::size_t index = 0; index < PSS_STATE_NAMES.size(); ++index) {
      if (view(*name) == PSS_STATE_NAMES[index]) {
        state_ = static_cast<StyleState>(index + 1);

        return admit(sink_.state(state_));
      }
    }

    return failAt(PssErrorCode::UNKNOWN_STATE, begin, "Unknown presentation state", view(*name));
  }

  constexpr bool parseSelector() {
    bool present{};
    bool hasId{};

    state_ = StyleState::ANY;

    if (peek() == '*') {
      advance();
      present = true;
    } else if (pssLetter(peek())) {
      const auto begin = location_;
      const auto name = identifier();

      if (!admit(sink_.type(*name, begin))) {
        return false;
      }

      present = true;
    }

    for (;;) {
      const auto marker = peek();

      if (marker != '.' && marker != '#' && marker != ':') {
        break;
      }

      advance();
      present = true;

      if (marker == ':') {
        if (state_ != StyleState::ANY) {
          return fail(PssErrorCode::DUPLICATE_STATE, "Duplicate selector state");
        }

        if (!parseState()) {
          return false;
        }
      } else {
        if (marker == '#' && hasId) {
          return fail(PssErrorCode::DUPLICATE_ID, "Duplicate selector id");
        }

        const auto begin = location_;
        const auto name = identifier();

        if (!name) {
          return false;
        }

        if (marker == '.') {
          if (!admit(sink_.className(*name, begin))) {
            return false;
          }
        } else {
          hasId = true;

          if (!admit(sink_.id(*name, begin))) {
            return false;
          }
        }
      }
    }

    if (!present) {
      return fail(PssErrorCode::EXPECTED_SELECTOR, "Expected selector");
    }

    return skipSpace();
  }

  constexpr std::string_view value() {
    const auto begin = location_.offset;

    for (;;) {
      const auto current = peek();

      if (current == ';' || current == '}' || current == '\0' || pssSpace(current)) {
        break;
      }

      advance();
    }

    return source_.substr(begin, location_.offset - begin);
  }

  static constexpr std::optional<RgbColor> parseColor(std::string_view text) {
    if (text.size() != 7 || text.front() != '#') {
      return {};
    }

    unsigned number{};

    for (const auto value : text.substr(1)) {
      unsigned part{};

      if (pssDigit(value)) {
        part = static_cast<unsigned>(value - '0');
      } else if (value >= 'a' && value <= 'f') {
        part = static_cast<unsigned>(value - 'a' + 10);
      } else if (value >= 'A' && value <= 'F') {
        part = static_cast<unsigned>(value - 'A' + 10);
      } else {
        return {};
      }

      number = number * 16 + part;
    }

    return RgbColor{static_cast<std::uint8_t>(number >> 16), static_cast<std::uint8_t>(number >> 8),
                    static_cast<std::uint8_t>(number)};
  }

  static constexpr std::optional<std::int32_t> parseWidth(std::string_view text) {
    const bool negative = text.starts_with('-');

    if (negative) {
      text.remove_prefix(1);
    }

    if (text.empty()) {
      return {};
    }

    std::int32_t result{};
    constexpr auto MAX_WIDTH = std::numeric_limits<std::int32_t>::max();

    for (const auto value : text) {
      if (!pssDigit(value)) {
        return {};
      }

      const auto part = value - '0';

      if (result > (MAX_WIDTH - part) / 10) {
        return {};
      }

      result = result * 10 + part;
    }

    if (negative && result != 0) {
      return {};
    }

    return result;
  }

  constexpr bool parseDeclaration() {
    const auto propertyLocation = location_;
    const auto name = identifier();

    if (!name || !skipSpace() || !expect(':')) {
      return false;
    }

    const auto valueLocation = location_;
    const auto text = value();
    const auto propertyName = view(*name);
    std::size_t index{};

    while (index < PSS_PROPERTY_NAMES.size() && propertyName != PSS_PROPERTY_NAMES[index]) {
      ++index;
    }

    if (index == PSS_PROPERTY_NAMES.size()) {
      return failAt(PssErrorCode::UNKNOWN_PROPERTY, propertyLocation, "Unknown property", propertyName);
    }

    const auto property = static_cast<PssProperty>(index);
    PssValue declaration;

    if (property == PssProperty::FOREGROUND || property == PssProperty::BACKGROUND ||
        property == PssProperty::FRAME_LINE_FOREGROUND || property == PssProperty::FRAME_LINE_BACKGROUND ||
        property == PssProperty::FRAME_TITLE_FOREGROUND || property == PssProperty::FRAME_TITLE_BACKGROUND ||
        property == PssProperty::FRAME_ENDS_FOREGROUND || property == PssProperty::FRAME_ENDS_BACKGROUND) {
      const auto color = parseColor(text);

      if (!color) {
        return failAt(PssErrorCode::INVALID_VALUE, valueLocation, "Invalid property value", propertyName);
      }

      declaration.color = *color;
    } else if (property == PssProperty::MIN_WIDTH) {
      const auto width = parseWidth(text);

      if (!width || state_ != StyleState::ANY) {
        return failAt(PssErrorCode::INVALID_WIDTH, valueLocation, "Expected nonnegative state-independent cell width",
                      propertyName);
      }

      declaration.width = *width;
    } else {
      if (text != "true" && text != "false") {
        return failAt(PssErrorCode::INVALID_VALUE, valueLocation, "Invalid property value", propertyName);
      }

      declaration.boolean = text == "true";
    }

    if (!admit(sink_.declaration(property, declaration, propertyLocation))) {
      return false;
    }

    return skipSpace() && expect(';');
  }

  std::string_view source_;
  const char *sourceData_;
  std::size_t sourceSize_;
  Sink &sink_;
  PssPolicy policy_;
  std::size_t maxBytes_;
  std::size_t maxRules_;
  PssLocation location_;
  PssError error_;
  std::size_t rules_{};
  StyleState state_{StyleState::ANY};
};

/** @param property Destination identity.
 * @param value Single populated scalar declaration.
 * @param destination Existing declarations updated with last-occurrence semantics.
 */
constexpr void assignPssDeclaration(PssProperty property, PssValue value, StyleDeclarations &destination) {
  switch (property) {
  case PssProperty::FOREGROUND:
    destination.foreground = value.color;

    break;
  case PssProperty::BACKGROUND:
    destination.background = value.color;

    break;
  case PssProperty::BOLD:
    destination.bold = value.boolean;

    break;
  case PssProperty::ITALIC:
    destination.italic = value.boolean;

    break;
  case PssProperty::UNDERLINE:
    destination.underline = value.boolean;

    break;
  case PssProperty::FRAME_LINE_FOREGROUND:
    destination.frameLine.foreground = value.color;

    break;
  case PssProperty::FRAME_LINE_BACKGROUND:
    destination.frameLine.background = value.color;

    break;
  case PssProperty::FRAME_LINE_BOLD:
    destination.frameLine.bold = value.boolean;

    break;
  case PssProperty::FRAME_LINE_ITALIC:
    destination.frameLine.italic = value.boolean;

    break;
  case PssProperty::FRAME_LINE_UNDERLINE:
    destination.frameLine.underline = value.boolean;

    break;
  case PssProperty::FRAME_TITLE_FOREGROUND:
    destination.frameTitle.foreground = value.color;

    break;
  case PssProperty::FRAME_TITLE_BACKGROUND:
    destination.frameTitle.background = value.color;

    break;
  case PssProperty::FRAME_TITLE_BOLD:
    destination.frameTitle.bold = value.boolean;

    break;
  case PssProperty::FRAME_TITLE_ITALIC:
    destination.frameTitle.italic = value.boolean;

    break;
  case PssProperty::FRAME_TITLE_UNDERLINE:
    destination.frameTitle.underline = value.boolean;

    break;
  case PssProperty::FRAME_ENDS_FOREGROUND:
    destination.frameEnds.foreground = value.color;

    break;
  case PssProperty::FRAME_ENDS_BACKGROUND:
    destination.frameEnds.background = value.color;

    break;
  case PssProperty::FRAME_ENDS_BOLD:
    destination.frameEnds.bold = value.boolean;

    break;
  case PssProperty::FRAME_ENDS_ITALIC:
    destination.frameEnds.italic = value.boolean;

    break;
  case PssProperty::FRAME_ENDS_UNDERLINE:
    destination.frameEnds.underline = value.boolean;

    break;
  case PssProperty::MIN_WIDTH:
    destination.minWidth = value.width;

    break;
  }
}
} // namespace portals::detail
