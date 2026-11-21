// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/document_highlight.hpp>

#include <algorithm>
#include <limits>

namespace portals {
namespace {

using State = detail::HighlightState;
using Sql = detail::HighlightSqlMode;
using Markdown = detail::HighlightMarkdownMode;
using Status = DocumentHighlightStatus;
using Role = SemanticRole;
using MySqlComment = detail::HighlightMySqlComment;
using MySqlSet = detail::HighlightMySqlSet;
using MySqlTarget = detail::HighlightMySqlTarget;

bool isMySql(const State &state) noexcept {
  return state.options.language == DocumentLanguage::MYSQL_LEXICAL ||
         (state.sqlFence && state.embeddedLanguage == DocumentLanguage::MYSQL_LEXICAL);
}

constexpr std::string_view MYSQL_KEYWORDS[]{"CALL",      "DECLARE", "DEFINER",        "DETERMINISTIC", "DO",
                                            "ELSEIF",    "ENGINE",  "EVENT",          "EXPLAIN",       "FUNCTION",
                                            "IF",        "IGNORE",  "KEY_BLOCK_SIZE", "LOCK",          "LOOP",
                                            "PROCEDURE", "REPEAT",  "REPLACE",        "RETURN",        "RETURNS",
                                            "SET",       "SHOW",    "STRAIGHT_JOIN",  "TRIGGER",       "UNLOCK",
                                            "UNTIL",     "USE",     "WHILE"};

constexpr std::string_view NON_LEXICAL_MYSQL_MODES[]{"ALLOW_INVALID_DATES",    "ERROR_FOR_DIVISION_BY_ZERO",
                                                     "HIGH_NOT_PRECEDENCE",    "IGNORE_SPACE",
                                                     "NO_AUTO_VALUE_ON_ZERO",  "NO_DIR_IN_CREATE",
                                                     "NO_ENGINE_SUBSTITUTION", "NO_UNSIGNED_SUBTRACTION",
                                                     "NO_ZERO_DATE",           "NO_ZERO_IN_DATE",
                                                     "ONLY_FULL_GROUP_BY",     "PAD_CHAR_TO_FULL_LENGTH",
                                                     "PIPES_AS_CONCAT",        "STRICT_ALL_TABLES",
                                                     "STRICT_TRANS_TABLES",    "TIME_TRUNCATE_FRACTIONAL"};

constexpr std::string_view KEYWORDS[]{
  "ALL",        "ALTER",     "AND",       "AS",         "ASC",    "BEGIN",  "BETWEEN", "BY",       "CASE",
  "CAST",       "CHECK",     "COMMIT",    "CONSTRAINT", "COPY",   "CREATE", "CROSS",   "DATABASE", "DEFAULT",
  "DELETE",     "DESC",      "DISTINCT",  "DROP",       "ELSE",   "END",    "EXCEPT",  "EXISTS",   "FALSE",
  "FETCH",      "FOR",       "FOREIGN",   "FROM",       "FULL",   "GROUP",  "HAVING",  "IN",       "INDEX",
  "INNER",      "INSERT",    "INTERSECT", "INTO",       "IS",     "JOIN",   "KEY",     "LEFT",     "LIKE",
  "LIMIT",      "NOT",       "NULL",      "OFFSET",     "ON",     "OR",     "ORDER",   "OUTER",    "PRIMARY",
  "REFERENCES", "RETURNING", "RIGHT",     "ROLLBACK",   "SCHEMA", "SELECT", "STDIN",   "TABLE",    "THEN",
  "TO",         "TRUE",      "TRUNCATE",  "UNION",      "UNIQUE", "UPDATE", "USING",   "VALUES",   "VIEW",
  "WHEN",       "WHERE",     "WITH"};

bool newline(char32_t c) noexcept {
  return c == U'\r' || c == U'\n';
}

bool space(char32_t c) noexcept {
  return c == U' ' || c == U'\t';
}

bool digit(char32_t c) noexcept {
  return c >= U'0' && c <= U'9';
}

bool letter(char32_t c) noexcept {
  return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || c == U'_' || c >= 0x80;
}

bool identifier(char32_t c) noexcept {
  return letter(c) || digit(c) || c == U'$';
}

char upper(char32_t c) noexcept {
  return static_cast<char>(c >= U'a' && c <= U'z' ? c - U'a' + U'A' : c);
}

char lower(char32_t c) noexcept {
  return static_cast<char>(c >= U'A' && c <= U'Z' ? c - U'A' + U'a' : c);
}

TokenRun interval(const DecodedScalar &scalar, Role role) noexcept {
  return {scalar.byteOffset, scalar.byteLength, role};
}

void append(TokenRun &run, const DecodedScalar &scalar) noexcept {
  if (run.byteLength == 0) {
    run.byteOffset = scalar.byteOffset;
  }

  run.byteLength += scalar.byteLength;
}

void stage(State &state, TokenRun run) noexcept {
  if (run.byteLength == 0) {
    return;
  }

  auto &queue = state.capturing ? state.speculative : state.queue;
  auto &count = state.capturing ? state.speculativeCount : state.queued;

  if (count != 0) {
    auto &previous = queue[count - 1];

    if (previous.role == run.role && previous.byteOffset + previous.byteLength == run.byteOffset) {
      previous.byteLength += run.byteLength;

      return;
    }
  }

  // A transition emits at most a prefix, two numeric suffixes, and its current scalar.
  // Fence speculation consists only of delimiter punctuation and whitespace, so it
  // also has at most four role changes, independently of the source-line length.
  queue[count++] = run;
}

void emit(State &state, const DecodedScalar &scalar, Role role) noexcept {
  stage(state, interval(scalar, role));
}

void flushPending(State &state, Role role) noexcept {
  state.pending.role = role;
  stage(state, state.pending);
  state.pending = {};
}

void flushMarkdown(State &state, Role role) noexcept {
  state.markdownPending.role = role;
  stage(state, state.markdownPending);
  state.markdownPending = {};
}

std::string_view word(const State &state) noexcept {
  return {state.word.data(), state.wordLength};
}

bool keyword(std::string_view value) noexcept {
  return std::find(std::begin(KEYWORDS), std::end(KEYWORDS), value) != std::end(KEYWORDS);
}

Status observeMySqlWord(State &state, std::string_view value) noexcept {
  auto &mysql = state.mysql;

  if (mysql.wordVariable && mysql.set == MySqlSet::OFF) {
    return Status::NEED_INPUT;
  }

  if (state.statementStart && value == "CREATE") {
    mysql.createStatement = true;
  }

  if (mysql.createStatement &&
      (value == "PROCEDURE" || value == "FUNCTION" || value == "TRIGGER" || value == "EVENT")) {
    mysql.routine = true;
    mysql.set = MySqlSet::OFF;
  }

  if (mysql.routine) {
    return Status::NEED_INPUT;
  }

  if (mysql.set == MySqlSet::OFF) {
    if (state.statementStart && value == "SET") {
      mysql.set = MySqlSet::TARGET;
      mysql.target = MySqlTarget::OTHER;
      mysql.atCount = 0;
    }

    return Status::NEED_INPUT;
  }

  if (mysql.set == MySqlSet::TARGET) {
    if (value == "GLOBAL") {
      return Status::UNSUPPORTED_PROFILE;
    }

    if (value == "SESSION" || value == "LOCAL") {
      return Status::NEED_INPUT;
    }

    mysql.target = value == "SQL_MODE" && mysql.atCount != 1       ? MySqlTarget::MODE
                   : value == "OLD_SQL_MODE" && mysql.atCount == 1 ? MySqlTarget::SAVED_MODE
                                                                   : MySqlTarget::OTHER;
    mysql.set = MySqlSet::TARGET_NAME;
    mysql.atCount = 0;

    return Status::NEED_INPUT;
  }

  if (mysql.set == MySqlSet::VALUE) {
    if (mysql.target == MySqlTarget::SAVED_MODE && mysql.atCount == 2 && value == "SQL_MODE") {
      mysql.pendingSavedMode = mysql.currentMode;
      mysql.pendingSavedValid = true;
      mysql.set = MySqlSet::VALUE_TAIL;
    } else if (mysql.target == MySqlTarget::MODE && mysql.atCount == 1 && value == "OLD_SQL_MODE" && mysql.savedValid) {
      mysql.pendingMode = mysql.savedMode;
      mysql.pendingModeValid = true;
      mysql.set = MySqlSet::VALUE_TAIL;
    } else if (mysql.target != MySqlTarget::OTHER) {
      return Status::UNSUPPORTED_PROFILE;
    } else {
      mysql.set = MySqlSet::OTHER_VALUE;
    }

    mysql.atCount = 0;
  } else if ((mysql.set == MySqlSet::TARGET_NAME || mysql.set == MySqlSet::VALUE_TAIL) &&
             mysql.target != MySqlTarget::OTHER) {
    return Status::UNSUPPORTED_PROFILE;
  }

  return Status::NEED_INPUT;
}

Status observeMySqlSymbol(State &state, char32_t c) noexcept {
  auto &mysql = state.mysql;

  if (mysql.set == MySqlSet::OFF || space(c) || newline(c)) {
    return Status::NEED_INPUT;
  }

  if (c == U',' && mysql.setDepth == 0 && (mysql.set != MySqlSet::VALUE || mysql.target == MySqlTarget::OTHER)) {
    mysql.set = MySqlSet::TARGET;
    mysql.target = MySqlTarget::OTHER;
    mysql.atCount = 0;

    return Status::NEED_INPUT;
  }

  if ((mysql.set == MySqlSet::TARGET || mysql.set == MySqlSet::VALUE) && c == U'@') {
    if (mysql.atCount == 2) {
      return Status::UNSUPPORTED_PROFILE;
    }

    ++mysql.atCount;

    return Status::NEED_INPUT;
  }

  if (mysql.set == MySqlSet::TARGET && c == U'.') {
    return Status::NEED_INPUT;
  }

  if (mysql.set == MySqlSet::TARGET_NAME && c == U'=') {
    mysql.set = MySqlSet::VALUE;
    mysql.atCount = 0;

    return Status::NEED_INPUT;
  }

  if (mysql.target != MySqlTarget::OTHER) {
    if (mysql.set == MySqlSet::VALUE_TAIL && c == U';') {
      return Status::NEED_INPUT;
    }

    return Status::UNSUPPORTED_PROFILE;
  }

  mysql.set = MySqlSet::OTHER_VALUE;

  if (c == U'(') {
    if (mysql.setDepth == std::numeric_limits<std::uint16_t>::max()) {
      return Status::RESOURCE_LIMIT;
    }

    ++mysql.setDepth;
  } else if (c == U')' && mysql.setDepth != 0) {
    --mysql.setDepth;
  }

  return Status::NEED_INPUT;
}

Status finishMySqlModeWord(State &state) noexcept {
  auto &mysql = state.mysql;
  const std::string_view value{mysql.modeWord.data(), mysql.modeWordLength};

  if (value == "NO_BACKSLASH_ESCAPES") {
    mysql.modeNoBackslash = true;
  } else if (!value.empty() && std::find(std::begin(NON_LEXICAL_MYSQL_MODES), std::end(NON_LEXICAL_MYSQL_MODES),
                                         value) == std::end(NON_LEXICAL_MYSQL_MODES)) {
    return Status::UNSUPPORTED_PROFILE;
  }

  mysql.modeWordLength = 0;
  mysql.modeWordEnded = false;

  return Status::NEED_INPUT;
}

Status observeMySqlModeCharacter(State &state, char32_t c) noexcept {
  auto &mysql = state.mysql;

  if (c == U',') {
    return finishMySqlModeWord(state);
  }

  if (space(c)) {
    mysql.modeWordEnded = mysql.modeWordLength != 0;

    return Status::NEED_INPUT;
  }

  if (!((c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z') || c == U'_') || mysql.modeWordEnded) {
    return Status::UNSUPPORTED_PROFILE;
  }

  if (mysql.modeWordLength == mysql.modeWord.size()) {
    return Status::RESOURCE_LIMIT;
  }

  mysql.modeWord[mysql.modeWordLength++] = upper(c);

  return Status::NEED_INPUT;
}

Status finishMySqlModeLiteral(State &state) noexcept {
  const auto status = finishMySqlModeWord(state);

  if (status != Status::NEED_INPUT) {
    return status;
  }

  state.mysql.pendingMode =
    state.mysql.modeNoBackslash ? DocumentMySqlMode::NO_BACKSLASH_ESCAPES : DocumentMySqlMode::NORMAL;
  state.mysql.pendingModeValid = true;
  state.mysql.modeLiteral = false;
  state.mysql.set = MySqlSet::VALUE_TAIL;

  return Status::NEED_INPUT;
}

Status beginMySqlModeLiteral(State &state) noexcept {
  auto &mysql = state.mysql;

  if (mysql.set == MySqlSet::VALUE_TAIL && mysql.target != MySqlTarget::OTHER) {
    return Status::UNSUPPORTED_PROFILE;
  }

  if (mysql.set == MySqlSet::VALUE && mysql.target != MySqlTarget::OTHER) {
    if (mysql.target != MySqlTarget::MODE || mysql.atCount != 0) {
      return Status::UNSUPPORTED_PROFILE;
    }

    mysql.modeLiteral = true;
    mysql.modeNoBackslash = false;
    mysql.modeWordEnded = false;
    mysql.modeWordLength = 0;
  }

  return Status::NEED_INPUT;
}

bool relevantMySqlValue(const State &state) noexcept {
  return isMySql(state) && state.mysql.target != MySqlTarget::OTHER &&
         (state.mysql.set == MySqlSet::VALUE || state.mysql.set == MySqlSet::VALUE_TAIL);
}

Status commitMySqlStatement(State &state) noexcept {
  auto &mysql = state.mysql;
  const bool incomplete =
    mysql.target != MySqlTarget::OTHER && (mysql.set == MySqlSet::VALUE || mysql.set == MySqlSet::TARGET_NAME);

  if (incomplete || mysql.modeLiteral) {
    return Status::UNSUPPORTED_PROFILE;
  }

  if (mysql.pendingSavedValid) {
    mysql.savedMode = mysql.pendingSavedMode;
    mysql.savedValid = true;
  }

  if (mysql.pendingModeValid) {
    mysql.currentMode = mysql.pendingMode;
  }

  mysql.pendingSavedValid = false;
  mysql.pendingModeValid = false;
  mysql.createStatement = false;
  mysql.routine = false;
  mysql.set = MySqlSet::OFF;
  mysql.target = MySqlTarget::OTHER;
  mysql.atCount = 0;
  mysql.setDepth = 0;
  mysql.variablePrefix = false;
  mysql.wordVariable = false;
  state.statementStart = true;

  return Status::NEED_INPUT;
}

Status classifyWord(State &state) noexcept {
  const auto value = word(state);

  if (isMySql(state)) {
    if (value == "DELIMITER" && state.mysql.wordLineStart && state.mysql.comment == MySqlComment::NONE) {
      if (!state.statementStart) {
        return Status::UNSUPPORTED_PROFILE;
      }

      flushPending(state, Role::MARKUP);
      state.sqlMode = Sql::MYSQL_DIRECTIVE_GAP;
      state.mysql.replacementLength = 0;
      state.mysql.directiveSpace = false;

      return Status::NEED_INPUT;
    }

    const auto status = observeMySqlWord(state, value);

    if (status != Status::NEED_INPUT) {
      return status;
    }
  } else if (value == "DELIMITER" || value == "SET") {
    return Status::UNSUPPORTED_PROFILE;
  }

  if (!isMySql(state) && state.statementStart && value == "COPY") {
    state.copyHeader = true;
  }

  if (state.copyHeader && state.parentheses == 0) {
    if (value == "FROM") {
      state.copyFrom = true;
    } else if (state.copyFrom) {
      state.copyStdin = value == "STDIN";
      state.copyFrom = false;
    }
  }

  state.statementStart = false;
  const bool mysqlKeyword =
    isMySql(state) && !state.mysql.wordVariable &&
    std::find(std::begin(MYSQL_KEYWORDS), std::end(MYSQL_KEYWORDS), value) != std::end(MYSQL_KEYWORDS);

  const bool baseKeyword =
    keyword(value) && (!isMySql(state) || (!state.mysql.wordVariable && value != "COPY" && value != "STDIN"));

  flushPending(state, baseKeyword || mysqlKeyword ? Role::KEYWORD : Role::IDENTIFIER);
  state.sqlMode = Sql::TEXT;

  return Status::NEED_INPUT;
}

void startWord(State &state, const DecodedScalar &scalar) noexcept {
  state.pending = interval(scalar, Role::IDENTIFIER);
  state.wordLength = 0;
  state.mysql.wordLineStart = state.mysql.lineStart;
  state.mysql.wordVariable = state.mysql.variablePrefix;
  state.mysql.variablePrefix = false;

  if (scalar.scalar < 0x80) {
    state.word[state.wordLength++] = upper(scalar.scalar);
    state.sqlMode = Sql::WORD;
  } else {
    flushPending(state, Role::IDENTIFIER);
    state.sqlMode = Sql::IDENTIFIER;
    state.statementStart = false;
  }
}

void flushNumericSuffix(State &state) noexcept {
  for (std::uint8_t i = 0; i < state.suffixLength; ++i) {
    const auto c = state.suffix[i].scalar;

    emit(state, state.suffix[i], c == U'e' || c == U'E' ? Role::IDENTIFIER : Role::OPERATOR);
  }

  state.suffixLength = 0;
}

bool scanNumber(State &state, const DecodedScalar &scalar) noexcept {
  const auto c = scalar.scalar;

  if (state.sqlMode == Sql::NUMBER_SUFFIX) {
    const auto first = state.suffix[0].scalar;
    const bool exponent = first == U'e' || first == U'E';

    if (exponent && state.suffixLength == 1 && (c == U'+' || c == U'-')) {
      state.suffix[state.suffixLength++] = scalar;

      return true;
    }

    if (digit(c)) {
      for (std::uint8_t i = 0; i < state.suffixLength; ++i) {
        emit(state, state.suffix[i], Role::NUMBER);
      }

      state.suffixLength = 0;
      state.numberPhase = exponent ? 2 : 1;
      state.sqlMode = Sql::NUMBER;
      emit(state, scalar, Role::NUMBER);

      return true;
    }

    flushNumericSuffix(state);
    state.sqlMode = Sql::TEXT;

    return false;
  }

  if (digit(c)) {
    emit(state, scalar, Role::NUMBER);

    return true;
  }

  if ((c == U'.' && state.numberPhase == 0) || ((c == U'e' || c == U'E') && state.numberPhase < 2)) {
    state.suffix[0] = scalar;
    state.suffixLength = 1;
    state.sqlMode = Sql::NUMBER_SUFFIX;

    return true;
  }

  state.sqlMode = Sql::TEXT;

  return false;
}

Status scanBlockComment(State &state, const DecodedScalar &scalar) noexcept {
  const auto c = scalar.scalar;

  if (state.previous == U'/' && c == U'*') {
    if (isMySql(state)) {
      return Status::UNSUPPORTED_PROFILE;
    }

    if (state.commentDepth == state.options.maxCommentDepth) {
      return Status::RESOURCE_LIMIT;
    }

    ++state.commentDepth;
    state.previous = 0;
  } else if (state.previous == U'*' && c == U'/') {
    --state.commentDepth;
    state.previous = 0;

    if (state.commentDepth == 0) {
      state.sqlMode = Sql::TEXT;
    }
  } else {
    state.previous = c == U'/' || c == U'*' ? c : 0;
  }

  emit(state, scalar, Role::COMMENT);

  return Status::NEED_INPUT;
}

bool scanQuoted(State &state, const DecodedScalar &scalar) noexcept {
  const auto c = scalar.scalar;
  const bool single = state.sqlMode == Sql::SINGLE || state.sqlMode == Sql::SINGLE_CLOSE;
  const bool backtick = state.sqlMode == Sql::BACKTICK || state.sqlMode == Sql::BACKTICK_CLOSE;
  const auto quote = backtick ? U'`' : single ? U'\'' : U'"';
  const bool string = single || (isMySql(state) && !backtick);
  const auto role = string ? Role::STRING : Role::IDENTIFIER;
  const bool closing =
    state.sqlMode == Sql::SINGLE_CLOSE || state.sqlMode == Sql::DOUBLE_CLOSE || state.sqlMode == Sql::BACKTICK_CLOSE;

  if (closing) {
    if (c != quote) {
      state.sqlMode = Sql::TEXT;

      return false;
    }

    state.sqlMode = backtick ? Sql::BACKTICK : single ? Sql::SINGLE : Sql::DOUBLE;
    emit(state, scalar, role);

    return true;
  }

  emit(state, scalar, role);

  if (string && state.escapeString && state.escaped) {
    state.escaped = false;
  } else if (string && state.escapeString && c == U'\\') {
    state.escaped = true;
  } else if (c == quote) {
    state.sqlMode = backtick ? Sql::BACKTICK_CLOSE : single ? Sql::SINGLE_CLOSE : Sql::DOUBLE_CLOSE;
  }

  return true;
}

Status scanDollarOpen(State &state, const DecodedScalar &scalar, bool &consumed) noexcept {
  const auto c = scalar.scalar;

  if (c == U'$') {
    append(state.pending, scalar);
    flushPending(state, Role::STRING);
    state.sqlMode = Sql::DOLLAR_BODY;
    state.dollarMatch = 0;
    consumed = true;

    return Status::NEED_INPUT;
  }

  if (c >= 0x80 && letter(c)) {
    return Status::UNSUPPORTED_PROFILE;
  }

  if (letter(c) || (state.delimiterLength != 0 && digit(c))) {
    if (state.delimiterLength == state.options.maxDelimiterLength) {
      return Status::RESOURCE_LIMIT;
    }

    state.delimiter[state.delimiterLength++] = c;
    append(state.pending, scalar);
    consumed = true;

    return Status::NEED_INPUT;
  }

  // Dollar tags are ASCII in this profile; their byte lengths may still differ by
  // source encoding. The opening dollar interval is retained separately in suffix.
  emit(state, state.suffix[0], Role::OPERATOR);
  state.pending.byteOffset += state.suffix[0].byteLength;
  state.pending.byteLength -= state.suffix[0].byteLength;
  flushPending(state, Role::IDENTIFIER);
  state.sqlMode = Sql::TEXT;
  consumed = false;

  return Status::NEED_INPUT;
}

void scanDollarBody(State &state, const DecodedScalar &scalar) noexcept {
  const auto c = scalar.scalar;
  const auto expected = state.dollarMatch == 0 || state.dollarMatch == state.delimiterLength + 1
                          ? U'$'
                          : state.delimiter[state.dollarMatch - 1];

  emit(state, scalar, Role::STRING);

  if (c == expected) {
    ++state.dollarMatch;

    if (state.dollarMatch == state.delimiterLength + 2) {
      state.sqlMode = Sql::TEXT;
      state.dollarMatch = 0;
    }
  } else {
    // No tag contains '$'; a mismatching dollar is the only possible new prefix.
    state.dollarMatch = c == U'$' ? 1 : 0;
  }
}

bool copyMode(Sql mode) noexcept {
  return mode == Sql::COPY_START || mode == Sql::COPY_ESCAPE || mode == Sql::COPY_DOT || mode == Sql::COPY_DATA;
}

void scanCopyData(State &state, const DecodedScalar &scalar) noexcept {
  const auto c = scalar.scalar;

  if (state.sqlMode == Sql::COPY_START && c == U'\\') {
    state.pending = interval(scalar, Role::OPERATOR);
    state.sqlMode = Sql::COPY_ESCAPE;

    return;
  }

  if (state.sqlMode == Sql::COPY_ESCAPE && c == U'.') {
    append(state.pending, scalar);
    state.sqlMode = Sql::COPY_DOT;

    return;
  }

  if (state.sqlMode == Sql::COPY_DOT && newline(c)) {
    flushPending(state, Role::OPERATOR);
    emit(state, scalar, Role::NORMAL);
    state.sqlMode = Sql::TEXT;
    state.statementStart = true;

    return;
  }

  flushPending(state, Role::STRING);
  emit(state, scalar, Role::STRING);
  state.sqlMode = newline(c) ? Sql::COPY_START : Sql::COPY_DATA;
}

Status scanSqlText(State &state, const DecodedScalar &scalar) noexcept {
  const auto c = scalar.scalar;

  if (space(c) || newline(c)) {
    state.mysql.variablePrefix = false;
    emit(state, scalar, Role::NORMAL);

    if (newline(c) && state.copyAwaitLine) {
      state.copyAwaitLine = false;
      state.copyHeaderCr = c == U'\r';
      state.sqlMode = Sql::COPY_START;
    }

    return Status::NEED_INPUT;
  }

  if (letter(c)) {
    if (relevantMySqlValue(state) && (state.mysql.set == MySqlSet::VALUE_TAIL || c >= 0x80)) {
      return Status::UNSUPPORTED_PROFILE;
    }

    startWord(state, scalar);

    return Status::NEED_INPUT;
  }

  if (digit(c)) {
    if (relevantMySqlValue(state)) {
      return Status::UNSUPPORTED_PROFILE;
    }

    state.sqlMode = Sql::NUMBER;
    state.mysql.variablePrefix = false;
    state.numberPhase = 0;
    state.statementStart = false;
    emit(state, scalar, Role::NUMBER);

    return Status::NEED_INPUT;
  }

  if (c == U'-' || c == U'/') {
    state.mysql.variablePrefix = false;
    state.pending = interval(scalar, Role::OPERATOR);
    state.suffix[0] = scalar;
    state.sqlMode = c == U'-' ? Sql::DASH : Sql::SLASH;

    return Status::NEED_INPUT;
  }

  if (c == U'\'' || c == U'"' || (isMySql(state) && c == U'`')) {
    const bool quotedMySqlTarget = isMySql(state) && c == U'`' && state.mysql.set == MySqlSet::TARGET;

    if (quotedMySqlTarget || (c == U'`' && relevantMySqlValue(state))) {
      return Status::UNSUPPORTED_PROFILE;
    }

    if (isMySql(state) && c != U'`') {
      const auto status = beginMySqlModeLiteral(state);

      if (status != Status::NEED_INPUT) {
        return status;
      }
    }

    state.sqlMode = c == U'`' ? Sql::BACKTICK : c == U'\'' ? Sql::SINGLE : Sql::DOUBLE;
    state.mysql.variablePrefix = false;
    state.escapeString = isMySql(state) && c != U'`' && state.mysql.currentMode == DocumentMySqlMode::NORMAL;
    state.escaped = false;
    state.statementStart = false;
    emit(state, scalar, c == U'\'' || (isMySql(state) && c == U'"') ? Role::STRING : Role::IDENTIFIER);

    return Status::NEED_INPUT;
  }

  if (c == U'$' && !isMySql(state)) {
    state.pending = interval(scalar, Role::OPERATOR);
    state.suffix[0] = scalar;
    state.delimiterLength = 0;
    state.sqlMode = Sql::DOLLAR_OPEN;
    state.statementStart = false;

    return Status::NEED_INPUT;
  }

  if (isMySql(state)) {
    if (c == U'#') {
      state.mysql.variablePrefix = false;
      state.sqlMode = Sql::LINE_COMMENT;
      emit(state, scalar, Role::COMMENT);

      return Status::NEED_INPUT;
    }

    const auto status = observeMySqlSymbol(state, c);

    if (status != Status::NEED_INPUT) {
      return status;
    }

    state.statementStart = false;
    state.mysql.variablePrefix = c == U'@';
    emit(state, scalar, c >= 0x80 || c == 0 ? Role::NORMAL : Role::OPERATOR);

    return Status::NEED_INPUT;
  }

  if (state.copyHeader && c == U'(') {
    if (state.parentheses == std::numeric_limits<std::uint16_t>::max()) {
      return Status::RESOURCE_LIMIT;
    }

    ++state.parentheses;
  } else if (state.copyHeader && c == U')' && state.parentheses != 0) {
    --state.parentheses;
  }

  if (c == U';') {
    state.copyAwaitLine = state.copyHeader && state.copyStdin;
    state.copyHeader = false;
    state.copyFrom = false;
    state.copyStdin = false;
    state.parentheses = 0;
    state.statementStart = true;
  } else {
    state.statementStart = false;
  }

  emit(state, scalar, c >= 0x80 || c == 0 ? Role::NORMAL : Role::OPERATOR);

  return Status::NEED_INPUT;
}

Status scanMySqlDirective(State &state, const DecodedScalar &scalar) noexcept;

Status scanSqlCore(State &state, const DecodedScalar &scalar) noexcept {
  const auto c = scalar.scalar;

  if (state.sqlMode == Sql::UNICODE_PREFIX) {
    if (c == U'\'' || c == U'"') {
      return Status::UNSUPPORTED_PROFILE;
    }

    flushPending(state, Role::IDENTIFIER);
    emit(state, state.suffix[0], Role::OPERATOR);
    state.sqlMode = Sql::TEXT;
  }

  if (state.copyHeaderCr) {
    state.copyHeaderCr = false;

    if (c == U'\n') {
      emit(state, scalar, Role::NORMAL);

      return Status::NEED_INPUT;
    }
  }

  if (copyMode(state.sqlMode)) {
    scanCopyData(state, scalar);

    return Status::NEED_INPUT;
  }

  if (state.sqlMode == Sql::WORD) {
    if (identifier(c)) {
      append(state.pending, scalar);

      if (c < 0x80 && state.wordLength < state.word.size()) {
        state.word[state.wordLength++] = upper(c);
      } else {
        flushPending(state, Role::IDENTIFIER);
        state.sqlMode = Sql::IDENTIFIER;
        state.statementStart = false;
      }

      return Status::NEED_INPUT;
    }

    if (!isMySql(state) && c == U'\'' && word(state) == "E") {
      flushPending(state, Role::STRING);
      state.sqlMode = Sql::SINGLE;
      state.escapeString = true;
      state.escaped = false;
      state.statementStart = false;
      emit(state, scalar, Role::STRING);

      return Status::NEED_INPUT;
    }

    if (!isMySql(state) && c == U'&' && word(state) == "U") {
      state.sqlMode = Sql::UNICODE_PREFIX;
      state.suffix[0] = scalar;
      state.statementStart = false;

      return Status::NEED_INPUT;
    }

    if (!isMySql(state) && c == U'\'' && (word(state) == "B" || word(state) == "X")) {
      return Status::UNSUPPORTED_PROFILE;
    }

    const auto status = classifyWord(state);

    if (status != Status::NEED_INPUT) {
      return status;
    }

    if (state.sqlMode == Sql::MYSQL_DIRECTIVE_GAP) {
      return scanMySqlDirective(state, scalar);
    }
  }

  if (state.sqlMode == Sql::IDENTIFIER) {
    if (identifier(c)) {
      emit(state, scalar, Role::IDENTIFIER);

      return Status::NEED_INPUT;
    }

    state.sqlMode = Sql::TEXT;
  }

  if ((state.sqlMode == Sql::NUMBER || state.sqlMode == Sql::NUMBER_SUFFIX) && scanNumber(state, scalar)) {
    return Status::NEED_INPUT;
  }

  if (state.sqlMode == Sql::MYSQL_DASH) {
    if (c == U'-') {
      const auto status = observeMySqlSymbol(state, U'-');

      if (status != Status::NEED_INPUT) {
        return status;
      }

      emit(state, state.suffix[0], Role::OPERATOR);
      state.pending.byteOffset += state.suffix[0].byteLength;
      state.pending.byteLength -= state.suffix[0].byteLength;
      state.suffix[0] = {U'-', state.pending.byteOffset, static_cast<std::uint8_t>(state.pending.byteLength), false};
      append(state.pending, scalar);

      return Status::NEED_INPUT;
    }

    const bool comment = c <= 0x20 || c == 0x7F;

    if (!comment) {
      const auto status = observeMySqlSymbol(state, U'-');

      if (status != Status::NEED_INPUT) {
        return status;
      }
    }

    flushPending(state, comment ? Role::COMMENT : Role::OPERATOR);
    state.sqlMode = comment && !newline(c) ? Sql::LINE_COMMENT : Sql::TEXT;
  }

  if (state.sqlMode == Sql::DASH || state.sqlMode == Sql::SLASH) {
    const bool line = state.sqlMode == Sql::DASH && c == U'-';
    const bool block = state.sqlMode == Sql::SLASH && c == U'*';

    if (line || block) {
      if (block && isMySql(state) && state.mysql.comment == MySqlComment::ACTIVE) {
        return Status::UNSUPPORTED_PROFILE;
      }

      append(state.pending, scalar);

      if (line && isMySql(state)) {
        state.sqlMode = Sql::MYSQL_DASH;

        return Status::NEED_INPUT;
      }

      flushPending(state, Role::COMMENT);
      state.sqlMode = line ? Sql::LINE_COMMENT : isMySql(state) ? Sql::MYSQL_COMMENT_OPEN : Sql::BLOCK_COMMENT;
      state.commentDepth = block ? 1 : 0;
      state.previous = 0;

      return Status::NEED_INPUT;
    }

    if (isMySql(state)) {
      const auto status = observeMySqlSymbol(state, state.sqlMode == Sql::DASH ? U'-' : U'/');

      if (status != Status::NEED_INPUT) {
        return status;
      }
    }

    flushPending(state, Role::OPERATOR);
    state.sqlMode = Sql::TEXT;
  }

  if (state.sqlMode == Sql::LINE_COMMENT) {
    if (!newline(c)) {
      emit(state, scalar, Role::COMMENT);

      return Status::NEED_INPUT;
    }

    state.sqlMode = Sql::TEXT;
  }

  if (state.sqlMode == Sql::BLOCK_COMMENT) {
    return scanBlockComment(state, scalar);
  }

  if ((state.sqlMode == Sql::SINGLE || state.sqlMode == Sql::SINGLE_CLOSE || state.sqlMode == Sql::DOUBLE ||
       state.sqlMode == Sql::DOUBLE_CLOSE || state.sqlMode == Sql::BACKTICK || state.sqlMode == Sql::BACKTICK_CLOSE) &&
      scanQuoted(state, scalar)) {
    return Status::NEED_INPUT;
  }

  if (state.sqlMode == Sql::DOLLAR_OPEN) {
    bool consumed = false;
    const auto status = scanDollarOpen(state, scalar, consumed);

    if (status != Status::NEED_INPUT || consumed) {
      return status;
    }
  }

  if (state.sqlMode == Sql::DOLLAR_BODY) {
    scanDollarBody(state, scalar);

    return Status::NEED_INPUT;
  }

  return scanSqlText(state, scalar);
}

Status finishSql(State &state) noexcept;

bool directiveMode(Sql mode) noexcept {
  return mode == Sql::MYSQL_DIRECTIVE_GAP || mode == Sql::MYSQL_DIRECTIVE_VALUE || mode == Sql::MYSQL_DIRECTIVE_TRAIL;
}

bool delimiterCharacter(char32_t c) noexcept {
  return c >= U'!' && c <= U'~' && !letter(c) && !digit(c) && c != U'\'' && c != U'"' && c != U'`' && c != U'\\' &&
         c != U'#' && c != U'*' && c != U'-';
}

Status commitMySqlDirective(State &state) noexcept {
  auto &mysql = state.mysql;

  if (mysql.replacementLength == 0) {
    return Status::UNSUPPORTED_PROFILE;
  }

  mysql.terminator = mysql.replacement;
  mysql.terminatorLength = mysql.replacementLength;
  // KMP prefix lengths make repeated overlapping punctuation linear in admitted
  // source length; no directive text or prior source chunks need to be retained.
  mysql.fallback[0] = 0;

  for (std::uint8_t i = 1; i < mysql.terminatorLength; ++i) {
    auto length = mysql.fallback[i - 1];

    while (length != 0 && mysql.terminator[i] != mysql.terminator[length]) {
      length = mysql.fallback[length - 1];
    }

    mysql.fallback[i] = mysql.terminator[i] == mysql.terminator[length] ? length + 1 : 0;
  }

  mysql.replacementLength = 0;
  state.sqlMode = Sql::TEXT;

  return Status::NEED_INPUT;
}

Status scanMySqlDirective(State &state, const DecodedScalar &scalar) noexcept {
  const auto c = scalar.scalar;
  auto &mysql = state.mysql;

  if (newline(c)) {
    const auto status = commitMySqlDirective(state);

    if (status == Status::NEED_INPUT) {
      emit(state, scalar, Role::NORMAL);
    }

    return status;
  }

  if (space(c)) {
    mysql.directiveSpace = true;
    if (state.sqlMode == Sql::MYSQL_DIRECTIVE_VALUE) {
      state.sqlMode = Sql::MYSQL_DIRECTIVE_TRAIL;
    }

    emit(state, scalar, Role::MARKUP);

    return Status::NEED_INPUT;
  }

  if (!mysql.directiveSpace || state.sqlMode == Sql::MYSQL_DIRECTIVE_TRAIL || !delimiterCharacter(c)) {
    return Status::UNSUPPORTED_PROFILE;
  }

  if (mysql.replacementLength == state.options.maxDelimiterLength) {
    return Status::RESOURCE_LIMIT;
  }

  mysql.replacement[mysql.replacementLength++] = c;
  state.sqlMode = Sql::MYSQL_DIRECTIVE_VALUE;
  emit(state, scalar, Role::MARKUP);

  return Status::NEED_INPUT;
}

Status scanMySqlInner(State &state, const DecodedScalar &scalar) noexcept {
  if (state.sqlMode == Sql::MYSQL_COMMENT_OPEN) {
    if (scalar.scalar == U'!') {
      state.mysql.comment = MySqlComment::VERSION;
      state.mysql.version = 0;
      state.mysql.versionDigits = 0;
      state.sqlMode = Sql::TEXT;
      emit(state, scalar, Role::COMMENT);

      return Status::NEED_INPUT;
    }

    state.sqlMode = Sql::BLOCK_COMMENT;
  }

  if (state.mysql.modeLiteral) {
    const bool single = state.sqlMode == Sql::SINGLE || state.sqlMode == Sql::SINGLE_CLOSE;
    const auto quote = single ? U'\'' : U'"';
    const bool closed = state.sqlMode == Sql::SINGLE_CLOSE || state.sqlMode == Sql::DOUBLE_CLOSE;

    if (closed) {
      if (scalar.scalar == quote) {
        return Status::UNSUPPORTED_PROFILE;
      }

      const auto status = finishMySqlModeLiteral(state);

      if (status != Status::NEED_INPUT) {
        return status;
      }
    } else if (scalar.scalar != quote) {
      const auto status = observeMySqlModeCharacter(state, scalar.scalar);

      if (status != Status::NEED_INPUT) {
        return status;
      }
    }
  }

  return scanSqlCore(state, scalar);
}

Status finishMySqlToken(State &state) noexcept {
  if (state.mysql.modeLiteral) {
    if (state.sqlMode != Sql::SINGLE_CLOSE && state.sqlMode != Sql::DOUBLE_CLOSE) {
      return Status::UNSUPPORTED_PROFILE;
    }

    const auto status = finishMySqlModeLiteral(state);

    if (status != Status::NEED_INPUT) {
      return status;
    }
  }

  const auto status = finishSql(state);

  if (status != Status::NEED_INPUT) {
    return status;
  }

  if (!directiveMode(state.sqlMode)) {
    state.sqlMode = Sql::TEXT;
  }

  return Status::NEED_INPUT;
}

Status scanMySqlVersion(State &state, const DecodedScalar &scalar) noexcept {
  auto &mysql = state.mysql;
  const auto c = scalar.scalar;

  if (digit(c)) {
    if (mysql.versionDigits == 6) {
      return Status::UNSUPPORTED_PROFILE;
    }

    mysql.version = mysql.version * 10 + static_cast<std::uint32_t>(c - U'0');
    ++mysql.versionDigits;
    emit(state, scalar, Role::COMMENT);

    return Status::NEED_INPUT;
  }

  const bool gate = mysql.versionDigits == 5 || mysql.versionDigits == 6;

  if (gate && !space(c) && !newline(c) && c != U'*') {
    return Status::UNSUPPORTED_PROFILE;
  }

  const bool active = mysql.versionDigits == 0 || (gate && mysql.version <= state.options.mysqlServerVersion);

  mysql.comment = active ? MySqlComment::ACTIVE : MySqlComment::NONE;

  if (!active) {
    state.sqlMode = Sql::BLOCK_COMMENT;
    state.previous = 0;

    return scanBlockComment(state, scalar);
  }

  if (c == U'*') {
    mysql.star = scalar;
    mysql.starPending = true;

    return Status::NEED_INPUT;
  }

  return scanMySqlInner(state, scalar);
}

Status scanMySqlExecutable(State &state, const DecodedScalar &scalar) noexcept {
  auto &mysql = state.mysql;

  if (mysql.starPending) {
    if (scalar.scalar == U'/') {
      const auto status = finishMySqlToken(state);

      if (status != Status::NEED_INPUT) {
        return status;
      }

      emit(state, mysql.star, Role::COMMENT);
      emit(state, scalar, Role::COMMENT);
      mysql.starPending = false;
      mysql.comment = MySqlComment::NONE;
      state.sqlMode = Sql::TEXT;

      return Status::NEED_INPUT;
    }

    const auto status = scanMySqlInner(state, mysql.star);

    if (status != Status::NEED_INPUT) {
      return status;
    }

    mysql.starPending = false;
  }

  if (scalar.scalar == U'*') {
    mysql.star = scalar;
    mysql.starPending = true;

    return Status::NEED_INPUT;
  }

  return scanMySqlInner(state, scalar);
}

bool delimiterEligible(Sql mode) noexcept {
  return mode == Sql::TEXT || mode == Sql::WORD || mode == Sql::IDENTIFIER || mode == Sql::NUMBER ||
         mode == Sql::NUMBER_SUFFIX || mode == Sql::DASH || mode == Sql::SLASH || mode == Sql::MYSQL_DASH ||
         mode == Sql::SINGLE_CLOSE || mode == Sql::DOUBLE_CLOSE || mode == Sql::BACKTICK_CLOSE;
}

Status releaseMySqlPrefix(State &state, std::uint8_t released) noexcept {
  auto &mysql = state.mysql;
  auto offset = mysql.matched.byteOffset;

  for (std::uint8_t i = 0; i < released; ++i) {
    const DecodedScalar scalar{mysql.terminator[i], offset, mysql.matchedWidths[i], false};
    const auto status = scanMySqlInner(state, scalar);

    if (status != Status::NEED_INPUT) {
      return status;
    }

    offset += scalar.byteLength;
  }

  // Leave lexical lookahead intact when the entire prefix failed. In particular,
  // a released '/' and the current '*' must still form a block-comment opener.
  for (std::uint8_t i = released; i < mysql.matchedLength; ++i) {
    mysql.matchedWidths[i - released] = mysql.matchedWidths[i];
  }

  mysql.matchedLength -= released;
  mysql.matched.byteLength -= offset - mysql.matched.byteOffset;
  mysql.matched.byteOffset = offset;

  return Status::NEED_INPUT;
}

std::uint8_t mySqlPrefixFallback(const State &state) noexcept {
  return state.mysql.fallback[state.mysql.matchedLength - 1];
}

Status scanMySqlDelimiter(State &state, const DecodedScalar &scalar) noexcept {
  auto &mysql = state.mysql;

  while (mysql.matchedLength != 0 && scalar.scalar != mysql.terminator[mysql.matchedLength]) {
    const auto fallback = mySqlPrefixFallback(state);
    const auto status = releaseMySqlPrefix(state, mysql.matchedLength - fallback);

    if (status != Status::NEED_INPUT) {
      return status;
    }

    if (mysql.matchedLength != 0) {
      const auto boundary = finishMySqlToken(state);

      if (boundary != Status::NEED_INPUT) {
        return boundary;
      }
    }
  }

  if (scalar.scalar != mysql.terminator[mysql.matchedLength]) {
    return scanMySqlInner(state, scalar);
  }

  if (mysql.matchedLength == 0 && state.sqlMode != Sql::TEXT) {
    const auto status = finishMySqlToken(state);

    if (status != Status::NEED_INPUT) {
      return status;
    }
  }

  mysql.matchedWidths[mysql.matchedLength++] = scalar.byteLength;
  append(mysql.matched, scalar);

  if (mysql.matchedLength == mysql.terminatorLength) {
    const auto status = commitMySqlStatement(state);

    if (status != Status::NEED_INPUT) {
      return status;
    }

    mysql.matched.role = Role::MARKUP;
    stage(state, mysql.matched);
    mysql.matched = {};
    mysql.matchedLength = 0;
  }

  return Status::NEED_INPUT;
}

Status scanMySql(State &state, const DecodedScalar &scalar) noexcept {
  Status status = Status::NEED_INPUT;

  if (directiveMode(state.sqlMode)) {
    status = scanMySqlDirective(state, scalar);
  } else if (state.mysql.comment == MySqlComment::VERSION) {
    status = scanMySqlVersion(state, scalar);
  } else if (state.mysql.comment == MySqlComment::ACTIVE) {
    status = scanMySqlExecutable(state, scalar);
  } else if (state.mysql.matchedLength != 0) {
    status = scanMySqlDelimiter(state, scalar);
  } else if (delimiterEligible(state.sqlMode) && scalar.scalar == state.mysql.terminator[0]) {
    status = finishMySqlToken(state);

    if (status == Status::NEED_INPUT) {
      status = directiveMode(state.sqlMode) ? scanMySqlDirective(state, scalar) : scanMySqlDelimiter(state, scalar);
    }
  } else {
    status = scanMySqlInner(state, scalar);
  }

  if (status == Status::NEED_INPUT) {
    state.mysql.lineStart = newline(scalar.scalar) || (state.mysql.lineStart && space(scalar.scalar));
  }

  return status;
}

Status scanSql(State &state, const DecodedScalar &scalar) noexcept {
  return isMySql(state) ? scanMySql(state, scalar) : scanSqlCore(state, scalar);
}

void resetSql(State &state) noexcept {
  state.sqlMode = Sql::TEXT;
  state.pending = {};
  state.wordLength = 0;
  state.suffixLength = 0;
  state.commentDepth = 0;
  state.previous = 0;
  state.escapeString = false;
  state.escaped = false;
  state.statementStart = true;
  state.copyHeader = false;
  state.copyFrom = false;
  state.copyStdin = false;
  state.copyAwaitLine = false;
  state.copyHeaderCr = false;
  state.parentheses = 0;
  state.mysql = {};
  state.mysql.currentMode = state.options.mysqlMode;
}

bool markup(char32_t c) noexcept {
  return c == U'*' || c == U'_' || c == U'[' || c == U']' || c == U'(' || c == U')' || c == U'!' || c == U'>' ||
         c == U'\\' || c == U'#' || c == U'~';
}

Status fenceBody(State &state, const DecodedScalar &scalar) noexcept {
  if (state.sqlFence) {
    return scanSql(state, scalar);
  }

  emit(state, scalar, Role::NORMAL);

  return Status::NEED_INPUT;
}

void releaseSpeculation(State &state) noexcept {
  state.capturing = false;

  for (std::uint8_t i = 0; i < state.speculativeCount; ++i) {
    stage(state, state.speculative[i]);
  }

  state.speculativeCount = 0;
}

void acceptFenceClose(State &state) noexcept {
  state.capturing = false;
  state.speculativeCount = 0;
  flushMarkdown(state, Role::MARKUP);
  resetSql(state);
  state.fenced = false;
  state.sqlFence = false;
  state.markdownMode = Markdown::START;
  state.indent = 0;
}

void beginFenceInfo(State &state) noexcept {
  state.fenced = true;
  state.fenceCharacter = state.runCharacter;
  state.fenceLength = state.runLength;
  state.infoLength = 0;
  state.infoStarted = false;
  state.infoEnded = false;
  state.sqlFence = false;
  state.markdownMode = Markdown::FENCE_INFO;
  flushMarkdown(state, Role::MARKUP);
}

Status scanFenceInfo(State &state, const DecodedScalar &scalar) noexcept {
  const auto c = scalar.scalar;

  if (c == U'`' && state.fenceCharacter == U'`') {
    return Status::UNSUPPORTED_PROFILE;
  }

  emit(state, scalar, newline(c) ? Role::NORMAL : Role::MARKUP);

  if (newline(c)) {
    const std::string_view name{state.infoWord.data(), state.infoLength};

    state.embeddedLanguage = documentLanguageFromName(name);
    state.sqlFence = state.embeddedLanguage == DocumentLanguage::POSTGRESQL_LEXICAL ||
                     state.embeddedLanguage == DocumentLanguage::MYSQL_LEXICAL;
    resetSql(state);
    state.markdownMode = Markdown::START;
    state.indent = 0;

    return Status::NEED_INPUT;
  }

  if (!state.infoEnded) {
    if (space(c)) {
      state.infoEnded = state.infoStarted;
    } else {
      state.infoStarted = true;

      if (c < 0x80 && state.infoLength < state.infoWord.size()) {
        state.infoWord[state.infoLength++] = lower(c);
      } else {
        state.infoLength = 0;
        state.infoEnded = true;
      }
    }
  }

  return Status::NEED_INPUT;
}

Status scanFenceClose(State &state, const DecodedScalar &scalar) noexcept {
  const auto c = scalar.scalar;

  if (state.markdownMode == Markdown::CLOSE_RUN && c == state.fenceCharacter) {
    if (state.runLength == state.options.maxDelimiterLength) {
      return Status::RESOURCE_LIMIT;
    }

    ++state.runLength;
    append(state.markdownPending, scalar);

    return state.sqlFence ? scanSql(state, scalar) : Status::NEED_INPUT;
  }

  if (space(c)) {
    state.markdownMode = Markdown::CLOSE_SPACE;
    append(state.markdownPending, scalar);

    return state.sqlFence ? scanSql(state, scalar) : Status::NEED_INPUT;
  }

  if (newline(c) && state.runLength >= state.fenceLength) {
    acceptFenceClose(state);
    emit(state, scalar, Role::NORMAL);

    return Status::NEED_INPUT;
  }

  if (state.sqlFence) {
    releaseSpeculation(state);
    state.markdownPending = {};
  } else {
    flushMarkdown(state, Role::NORMAL);
  }

  state.markdownMode = newline(c) ? Markdown::START : Markdown::FENCE_TEXT;
  state.indent = 0;

  return fenceBody(state, scalar);
}

Status scanMarkdownStart(State &state, const DecodedScalar &scalar, bool &consumed) noexcept {
  const auto c = scalar.scalar;

  if (c == U' ' && state.indent < 3) {
    ++state.indent;
    consumed = true;

    return state.fenced ? fenceBody(state, scalar) : (emit(state, scalar, Role::NORMAL), Status::NEED_INPUT);
  }

  if (state.fenced) {
    if (c == state.fenceCharacter) {
      state.markdownPending = interval(scalar, Role::MARKUP);
      state.runLength = 1;
      state.markdownMode = Markdown::CLOSE_RUN;
      state.capturing = state.sqlFence;
      consumed = true;

      return state.sqlFence ? scanSql(state, scalar) : Status::NEED_INPUT;
    }

    state.markdownMode = newline(c) ? Markdown::START : Markdown::FENCE_TEXT;
    state.indent = 0;
    consumed = true;

    return fenceBody(state, scalar);
  }

  if (c == U'#') {
    state.markdownPending = interval(scalar, Role::HEADING);
    state.runLength = 1;
    state.markdownMode = Markdown::HASH;
    state.lineCandidate = false;
    consumed = true;

    return Status::NEED_INPUT;
  }

  state.lineCandidate = c == U'`' || c == U'~';
  state.markdownMode = Markdown::TEXT;
  consumed = false;

  return Status::NEED_INPUT;
}

Status scanMarkdownRun(State &state, const DecodedScalar &scalar, bool &consumed) noexcept {
  const auto c = scalar.scalar;

  if (c == state.runCharacter) {
    if (state.runLength == state.options.maxDelimiterLength) {
      return Status::RESOURCE_LIMIT;
    }

    ++state.runLength;
    append(state.markdownPending, scalar);
    consumed = true;

    return Status::NEED_INPUT;
  }

  if (state.lineCandidate && state.runLength >= 3) {
    beginFenceInfo(state);
    consumed = true;

    return scanFenceInfo(state, scalar);
  }

  if (state.runCharacter == U'`') {
    state.inlineLength = state.runLength;
    flushMarkdown(state, Role::STRING);
    state.markdownMode = Markdown::INLINE;
  } else {
    flushMarkdown(state, Role::MARKUP);
    state.markdownMode = Markdown::TEXT;
    state.lineCandidate = false;
  }

  consumed = false;

  return Status::NEED_INPUT;
}

Status scanMarkdown(State &state, const DecodedScalar &scalar) noexcept {
  const auto c = scalar.scalar;
  bool consumed = false;

  if (state.markdownMode == Markdown::START) {
    const auto status = scanMarkdownStart(state, scalar, consumed);

    if (status != Status::NEED_INPUT || consumed) {
      return status;
    }
  }

  if (state.markdownMode == Markdown::CLOSE_RUN || state.markdownMode == Markdown::CLOSE_SPACE) {
    return scanFenceClose(state, scalar);
  }

  if (state.markdownMode == Markdown::FENCE_INFO) {
    return scanFenceInfo(state, scalar);
  }

  if (state.markdownMode == Markdown::FENCE_TEXT) {
    if (newline(c)) {
      state.markdownMode = Markdown::START;
      state.indent = 0;
    }

    return fenceBody(state, scalar);
  }

  if (state.markdownMode == Markdown::HASH) {
    if (c == U'#' && state.runLength < 6) {
      ++state.runLength;
      append(state.markdownPending, scalar);

      return Status::NEED_INPUT;
    }

    if (space(c) || newline(c)) {
      flushMarkdown(state, Role::HEADING);
      state.markdownMode = Markdown::HEADING;
    } else {
      flushMarkdown(state, Role::MARKUP);
      state.markdownMode = Markdown::TEXT;
    }
  }

  if (state.markdownMode == Markdown::HEADING) {
    emit(state, scalar, newline(c) ? Role::NORMAL : Role::HEADING);

    if (newline(c)) {
      state.markdownMode = Markdown::START;
      state.indent = 0;
    }

    return Status::NEED_INPUT;
  }

  if (state.markdownMode == Markdown::RUN) {
    const auto status = scanMarkdownRun(state, scalar, consumed);

    if (status != Status::NEED_INPUT || consumed) {
      return status;
    }
  }

  if (state.markdownMode == Markdown::INLINE_RUN) {
    if (c == U'`') {
      if (state.runLength == state.options.maxDelimiterLength) {
        return Status::RESOURCE_LIMIT;
      }

      ++state.runLength;
      emit(state, scalar, Role::STRING);

      return Status::NEED_INPUT;
    }

    state.markdownMode = state.runLength == state.inlineLength ? Markdown::TEXT : Markdown::INLINE;
    state.lineCandidate = false;
  }

  if (state.markdownMode == Markdown::INLINE) {
    emit(state, scalar, Role::STRING);

    if (c == U'`') {
      state.runLength = 1;
      state.markdownMode = Markdown::INLINE_RUN;
    }

    return Status::NEED_INPUT;
  }

  if (c == U'`' || (c == U'~' && state.lineCandidate)) {
    state.markdownPending = interval(scalar, Role::MARKUP);
    state.runCharacter = c;
    state.runLength = 1;
    state.markdownMode = Markdown::RUN;

    return Status::NEED_INPUT;
  }

  emit(state, scalar, markup(c) ? Role::MARKUP : Role::NORMAL);
  state.lineCandidate = false;

  if (newline(c)) {
    state.markdownMode = Markdown::START;
    state.indent = 0;
  }

  return Status::NEED_INPUT;
}

Status finishSql(State &state) noexcept {
  if (state.sqlMode == Sql::UNICODE_PREFIX) {
    flushPending(state, Role::IDENTIFIER);
    emit(state, state.suffix[0], Role::OPERATOR);

    return Status::NEED_INPUT;
  }

  if (state.sqlMode == Sql::WORD) {
    return classifyWord(state);
  }

  if (state.sqlMode == Sql::NUMBER_SUFFIX) {
    flushNumericSuffix(state);
  } else if (state.sqlMode == Sql::DOLLAR_OPEN) {
    emit(state, state.suffix[0], Role::OPERATOR);
    state.pending.byteOffset += state.suffix[0].byteLength;
    state.pending.byteLength -= state.suffix[0].byteLength;
    flushPending(state, Role::IDENTIFIER);
  } else if (state.sqlMode == Sql::DASH || state.sqlMode == Sql::SLASH || state.sqlMode == Sql::MYSQL_DASH ||
             state.sqlMode == Sql::COPY_DOT) {
    if (isMySql(state)) {
      const auto status = observeMySqlSymbol(state, state.sqlMode == Sql::SLASH ? U'/' : U'-');

      if (status != Status::NEED_INPUT) {
        return status;
      }
    }

    flushPending(state, Role::OPERATOR);
  } else if (state.sqlMode == Sql::COPY_ESCAPE) {
    flushPending(state, Role::STRING);
  }

  return Status::NEED_INPUT;
}

Status finishMySql(State &state) noexcept {
  if (state.mysql.starPending) {
    const auto status = scanMySqlInner(state, state.mysql.star);

    if (status != Status::NEED_INPUT) {
      return status;
    }

    state.mysql.starPending = false;
  }

  if (state.mysql.matchedLength != 0) {
    const auto status = releaseMySqlPrefix(state, state.mysql.matchedLength);

    if (status != Status::NEED_INPUT) {
      return status;
    }
  }

  if (state.mysql.modeLiteral) {
    if (state.sqlMode != Sql::SINGLE_CLOSE && state.sqlMode != Sql::DOUBLE_CLOSE) {
      return Status::UNSUPPORTED_PROFILE;
    }

    const auto status = finishMySqlModeLiteral(state);

    if (status != Status::NEED_INPUT) {
      return status;
    }
  }

  const auto status = finishSql(state);

  if (status != Status::NEED_INPUT) {
    return status;
  }

  return directiveMode(state.sqlMode) ? commitMySqlDirective(state) : commitMySqlStatement(state);
}

Status finishMarkdown(State &state) noexcept {
  if (state.markdownMode == Markdown::CLOSE_RUN || state.markdownMode == Markdown::CLOSE_SPACE) {
    if (state.runLength >= state.fenceLength) {
      acceptFenceClose(state);
    } else if (state.sqlFence) {
      releaseSpeculation(state);
      state.markdownPending = {};
    } else {
      flushMarkdown(state, Role::NORMAL);
    }
  } else if (state.markdownMode == Markdown::HASH) {
    flushMarkdown(state, Role::HEADING);
  } else if (state.markdownMode == Markdown::RUN) {
    const bool fence = state.lineCandidate && state.runLength >= 3;

    flushMarkdown(state, fence || state.runCharacter == U'~' ? Role::MARKUP : Role::STRING);
  }

  if (state.fenced && state.sqlFence) {
    return isMySql(state) ? finishMySql(state) : finishSql(state);
  }

  return Status::NEED_INPUT;
}

bool validOptions(const DocumentHighlighterOptions &options) noexcept {
  const bool language =
    options.language == DocumentLanguage::PLAIN || options.language == DocumentLanguage::POSTGRESQL_LEXICAL ||
    options.language == DocumentLanguage::MYSQL_LEXICAL || options.language == DocumentLanguage::MARKDOWN_SOURCE;

  return language && options.workLimit != 0 && options.maxDelimiterLength != 0 && options.maxDelimiterLength <= 64 &&
         options.maxCommentDepth != 0 &&
         (options.mysqlMode == DocumentMySqlMode::NORMAL ||
          options.mysqlMode == DocumentMySqlMode::NO_BACKSLASH_ESCAPES) &&
         options.mysqlServerVersion != 0 && options.mysqlServerVersion <= 999999;
}

bool validScalar(const State &state, const DecodedScalar &scalar) noexcept {
  return scalar.byteLength != 0 && scalar.byteOffset == state.nextOffset && scalar.scalar <= 0x10FFFF &&
         !(scalar.scalar >= 0xD800 && scalar.scalar <= 0xDFFF) &&
         scalar.byteLength <= std::numeric_limits<std::uint64_t>::max() - scalar.byteOffset;
}

} // namespace

DocumentHighlighter::DocumentHighlighter(DocumentHighlighterOptions options, std::uint64_t initialByteOffset) noexcept {
  state_.options = options;
  state_.nextOffset = initialByteOffset;
  state_.mysql.currentMode = options.mysqlMode;
}

DocumentHighlighter::DocumentHighlighter(const DocumentHighlightCheckpoint &checkpoint) noexcept
    : state_(checkpoint.state_) {
}

DocumentHighlightResult DocumentHighlighter::feed(std::span<const DecodedScalar> input, std::span<TokenRun> output,
                                                  bool endOfInput) noexcept {
  DocumentHighlightResult result{};

  if (!validOptions(state_.options)) {
    result.status = Status::INVALID_CONFIGURATION;

    return result;
  }

  if (state_.eofFinalized && !input.empty()) {
    result.status = Status::INVALID_INPUT;
    result.errorOffset = input.front().byteOffset;

    return result;
  }

  std::size_t work = 0;

  for (;;) {
    if (state_.delivered < state_.queued) {
      if (result.outputProduced == output.size()) {
        result.status = Status::OUTPUT_FULL;

        return result;
      }

      if (work == state_.options.workLimit) {
        result.status = Status::WORK_LIMIT;

        return result;
      }

      const auto run = state_.queue[state_.delivered++];

      if (result.outputProduced != 0 && output[result.outputProduced - 1].role == run.role &&
          output[result.outputProduced - 1].byteOffset + output[result.outputProduced - 1].byteLength ==
            run.byteOffset) {
        output[result.outputProduced - 1].byteLength += run.byteLength;
      } else {
        output[result.outputProduced++] = run;
      }

      ++work;

      continue;
    }

    state_.queued = 0;
    state_.delivered = 0;

    if (state_.eofFinalized) {
      result.status = Status::DONE;

      return result;
    }

    if (result.inputConsumed == input.size() && !endOfInput) {
      result.status = Status::NEED_INPUT;

      return result;
    }

    if (work == state_.options.workLimit) {
      result.status = Status::WORK_LIMIT;

      return result;
    }

    // Copying a bounded owning state makes each admission/EOF transition atomic.
    // Previously delivered runs and previously admitted scalars remain committed.
    auto candidate = state_;
    Status status = Status::NEED_INPUT;
    const bool finalizing = result.inputConsumed == input.size();

    if (finalizing) {
      status = candidate.options.language == DocumentLanguage::MYSQL_LEXICAL        ? finishMySql(candidate)
               : candidate.options.language == DocumentLanguage::POSTGRESQL_LEXICAL ? finishSql(candidate)
               : candidate.options.language == DocumentLanguage::MARKDOWN_SOURCE    ? finishMarkdown(candidate)
                                                                                    : Status::NEED_INPUT;
      candidate.eofFinalized = status == Status::NEED_INPUT;
      result.errorOffset = candidate.pending.byteLength ? candidate.pending.byteOffset : candidate.nextOffset;
    } else {
      const auto &scalar = input[result.inputConsumed];

      if (!validScalar(state_, scalar)) {
        result.status = Status::INVALID_INPUT;
        result.errorOffset = scalar.byteOffset;

        return result;
      }

      if (candidate.options.language == DocumentLanguage::PLAIN) {
        emit(candidate, scalar, Role::NORMAL);
      } else if (candidate.options.language == DocumentLanguage::POSTGRESQL_LEXICAL ||
                 candidate.options.language == DocumentLanguage::MYSQL_LEXICAL) {
        status = scanSql(candidate, scalar);
      } else {
        status = scanMarkdown(candidate, scalar);
      }

      result.errorOffset = scalar.byteOffset;

      if (status == Status::NEED_INPUT) {
        candidate.nextOffset += scalar.byteLength;
        ++result.inputConsumed;
      }
    }

    if (status != Status::NEED_INPUT) {
      result.status = status;

      return result;
    }

    state_ = candidate;

    // EOF has one fixed-size transition; charging it separately could report
    // WORK_LIMIT with zero externally visible progress for a one-unit budget.
    // Its queued runs are charged normally when delivered.
    if (!finalizing) {
      ++work;
    }
  }
}

DocumentHighlightCheckpoint DocumentHighlighter::checkpoint() const noexcept {
  DocumentHighlightCheckpoint result;

  result.state_ = state_;

  return result;
}

std::uint64_t DocumentHighlighter::nextByteOffset() const noexcept {
  return state_.nextOffset;
}

DocumentLanguage documentLanguageFromName(std::string_view name) noexcept {
  const auto equal = [name](std::string_view expected) {
    if (name.size() != expected.size()) {
      return false;
    }

    for (std::size_t i = 0; i < name.size(); ++i) {
      if (lower(static_cast<unsigned char>(name[i])) != expected[i]) {
        return false;
      }
    }

    return true;
  };

  if (equal("sql") || equal("postgresql") || equal("postgres")) {
    return DocumentLanguage::POSTGRESQL_LEXICAL;
  }

  if (equal("mysql")) {
    return DocumentLanguage::MYSQL_LEXICAL;
  }

  if (equal("markdown") || equal("md")) {
    return DocumentLanguage::MARKDOWN_SOURCE;
  }

  return DocumentLanguage::PLAIN;
}

} // namespace portals
