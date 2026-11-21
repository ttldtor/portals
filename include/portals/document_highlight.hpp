// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/document_encoding.hpp>
#include <portals/document_highlight_types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace portals {

namespace detail {

/// Fixed SQL scanner phases; no phase borrows source storage.
enum class HighlightSqlMode {
  TEXT,                  ///< Between tokens.
  WORD,                  ///< Bounded possible keyword prefix.
  UNICODE_PREFIX,        ///< Possible U& prefix awaiting an immediately adjacent quote.
  IDENTIFIER,            ///< Streaming identifier known not to be a keyword.
  NUMBER,                ///< Streaming decimal digits or fraction.
  NUMBER_SUFFIX,         ///< Pending decimal point or exponent marker/sign.
  DASH,                  ///< Pending possible line-comment opener.
  SLASH,                 ///< Pending possible block-comment opener.
  LINE_COMMENT,          ///< Comment until source newline.
  BLOCK_COMMENT,         ///< Nesting-aware comment.
  SINGLE,                ///< Single-quoted string.
  SINGLE_CLOSE,          ///< Quote awaiting doubled-quote disambiguation.
  DOUBLE,                ///< Quoted identifier.
  DOUBLE_CLOSE,          ///< Identifier quote awaiting doubling.
  BACKTICK,              ///< MySQL backtick identifier.
  BACKTICK_CLOSE,        ///< Backtick awaiting doubling.
  MYSQL_DASH,            ///< Two MySQL dashes awaiting required following whitespace/control.
  MYSQL_COMMENT_OPEN,    ///< Comment opener awaiting executable-comment exclamation.
  MYSQL_DIRECTIVE_GAP,   ///< Recognized DELIMITER awaiting its punctuation value.
  MYSQL_DIRECTIVE_VALUE, ///< Bounded replacement delimiter value.
  MYSQL_DIRECTIVE_TRAIL, ///< DELIMITER trailing whitespace.
  DOLLAR_OPEN,           ///< Bounded possible dollar delimiter.
  DOLLAR_BODY,           ///< Dollar literal awaiting exact closing delimiter.
  COPY_START,            ///< COPY data line start, testing its terminator.
  COPY_ESCAPE,           ///< Possible first terminator backslash.
  COPY_DOT,              ///< Possible complete terminator awaiting line end.
  COPY_DATA              ///< Literal COPY payload line.
};

/// Fixed Markdown scanner phases for the documented raw-source profile.
enum class HighlightMarkdownMode {
  START,      ///< Source line start, up to three indentation spaces.
  TEXT,       ///< Ordinary source text.
  HASH,       ///< Possible ATX marker.
  HEADING,    ///< Remaining heading line.
  RUN,        ///< Opening backtick or tilde run.
  INLINE,     ///< Literal inline code region.
  INLINE_RUN, ///< Possible inline closing run.
  FENCE_INFO, ///< Opening fence information line.
  FENCE_TEXT, ///< Literal fenced body line.
  CLOSE_RUN,  ///< Possible closing fence run.
  CLOSE_SPACE ///< Possible closing fence trailing whitespace.
};

/// MySQL executable-comment wrapper, independent of its shared inner SQL phase.
enum class HighlightMySqlComment {
  NONE,    ///< No executable wrapper is active.
  VERSION, ///< Optional version digits following /*!.
  ACTIVE   ///< Version condition admits SQL source roles and mode observation.
};

/// Bounded observation of the supported SET assignment forms.
enum class HighlightMySqlSet {
  OFF,         ///< Outside observed SET source.
  TARGET,      ///< Awaiting a target, optionally qualified by SESSION/LOCAL.
  TARGET_NAME, ///< Target recognized; equals is required for relevant assignments.
  VALUE,       ///< Awaiting a supported mode literal or saved-mode reference.
  VALUE_TAIL,  ///< A relevant value is complete; only a clause/statement boundary is allowed.
  OTHER_VALUE  ///< An unrelated SET value is scanned without evaluation.
};

/// Relevant SET targets; storage is deliberately one known saved-mode slot.
enum class HighlightMySqlTarget {
  OTHER,     ///< Assignment cannot change the lexical mode.
  MODE,      ///< Session SQL_MODE assignment.
  SAVED_MODE ///< \@OLD_SQL_MODE save assignment.
};

/// Fixed MySQL client/source state, with no runtime or variable environment.
struct HighlightMySqlState {
  std::array<char32_t, 64> terminator{U';'};    ///< Current bounded client statement delimiter.
  std::array<char32_t, 64> replacement{};       ///< DELIMITER value under construction before newline/EOF.
  std::array<std::uint8_t, 64> matchedWidths{}; ///< Original-byte widths of a delayed delimiter prefix.
  std::array<std::uint8_t, 64> fallback{};      ///< KMP proper-prefix lengths for bounded delimiter matching.
  std::array<char, 32> modeWord{};              ///< One bounded SQL_MODE item, never the whole literal.
  TokenRun matched{};                           ///< Original byte interval of the delayed delimiter prefix.
  DecodedScalar star{};                         ///< Potential executable-comment closing star.
  std::uint32_t version{};                      ///< Accumulated executable-comment version gate.
  std::uint16_t setDepth{};                     ///< Parenthesis depth within unrelated SET expressions; bound65535.
  std::uint8_t terminatorLength{1};             ///< Current client delimiter length, in ASCII characters.
  std::uint8_t replacementLength{};             ///< New directive delimiter characters retained.
  std::uint8_t matchedLength{};                 ///< Current terminator prefix length.
  std::uint8_t versionDigits{};                 ///< Retained version digit count; bound6.
  std::uint8_t modeWordLength{};                ///< Retained mode-list item length; bound32.
  std::uint8_t atCount{};                       ///< At-sign qualification count, saturated at two.
  HighlightMySqlComment comment{HighlightMySqlComment::NONE}; ///< Executable wrapper phase.
  HighlightMySqlSet set{HighlightMySqlSet::OFF};              ///< Observed SET clause phase.
  HighlightMySqlTarget target{HighlightMySqlTarget::OTHER};   ///< Current assignment target.
  DocumentMySqlMode currentMode{};                            ///< Active session lexical mode.
  DocumentMySqlMode savedMode{};                              ///< Known \@OLD_SQL_MODE value, if savedValid.
  DocumentMySqlMode pendingMode{};                            ///< Mode to commit at the complete client terminator.
  DocumentMySqlMode pendingSavedMode{}; ///< Saved mode to commit at the complete client terminator.
  bool lineStart{true};                 ///< Only source-line indentation has been admitted.
  bool wordLineStart{};                 ///< Current bounded word began at a source-line start.
  bool starPending{};                   ///< A closing star is delayed across a chunk boundary.
  bool createStatement{};               ///< Current statement began with CREATE.
  bool routine{};           ///< Current CREATE defines a stored program; SET is not executed during definition.
  bool savedValid{};        ///< One proven saved-mode slot is available.
  bool pendingModeValid{};  ///< A supported mode update awaits the client terminator.
  bool pendingSavedValid{}; ///< A supported save awaits the client terminator.
  bool modeLiteral{};       ///< A supported SQL_MODE string is being observed.
  bool modeNoBackslash{};   ///< Current mode literal includes NO_BACKSLASH_ESCAPES.
  bool modeWordEnded{};     ///< Trailing whitespace began within a mode-list item.
  bool directiveSpace{};    ///< Required whitespace after the DELIMITER keyword was admitted.
  bool variablePrefix{};    ///< The last admitted source punctuation was an immediate variable-name @ prefix.
  bool wordVariable{};      ///< Current bounded word follows @ and must not introduce a statement or keyword role.
};

/// Trivially owned bounded state; size never depends on input/token/line length.
struct HighlightState {
  DocumentHighlighterOptions options{};             ///< Immutable policy for this scanner generation.
  std::uint64_t nextOffset{};                       ///< Next required original-byte offset.
  TokenRun pending{};                               ///< Delayed token prefix or delimiter interval.
  TokenRun markdownPending{};                       ///< Delayed Markdown delimiter or close-line interval.
  std::array<TokenRun, 8> queue{};                  ///< Constant-capacity classified output staging.
  std::array<TokenRun, 8> speculative{};            ///< SQL runs delayed while testing a fence close.
  std::uint8_t speculativeCount{};                  ///< Number of tentative SQL runs.
  std::uint8_t queued{};                            ///< Number of staged runs.
  std::uint8_t delivered{};                         ///< Staged runs already copied to caller storage.
  std::array<char32_t, 64> delimiter{};             ///< SQL tag characters, excluding dollar signs.
  std::array<char, 16> word{};                      ///< ASCII keyword candidate, uppercased.
  std::array<char, 16> infoWord{};                  ///< Bounded fence language candidate, lowercased.
  std::array<DecodedScalar, 2> suffix{};            ///< Pending numeric dot or exponent/sign.
  std::uint8_t wordLength{};                        ///< Retained ASCII keyword characters.
  std::uint8_t delimiterLength{};                   ///< Retained SQL tag characters or Markdown delimiter length.
  std::uint8_t dollarMatch{};                       ///< Matched closing dollar delimiter prefix length.
  std::uint8_t suffixLength{};                      ///< Number of delayed numeric suffix scalars.
  std::uint8_t numberPhase{};                       ///< Zero for integer, one for fraction, two for exponent digits.
  std::uint16_t commentDepth{};                     ///< Currently open nested block comments.
  char32_t previous{};                              ///< Previous comment delimiter candidate character.
  char32_t runCharacter{};                          ///< Current Markdown delimiter character.
  char32_t fenceCharacter{};                        ///< Opening fence character.
  std::uint8_t runLength{};                         ///< Current Markdown delimiter run length.
  std::uint8_t fenceLength{};                       ///< Required closing fence length.
  std::uint8_t inlineLength{};                      ///< Required inline closing backtick count.
  std::uint8_t indent{};                            ///< Source-line leading spaces seen, saturated at four.
  std::uint8_t infoLength{};                        ///< Retained fence info word length; excess is unknown/plain.
  std::uint16_t parentheses{};                      ///< SQL nesting depth used to identify COPY header direction.
  HighlightSqlMode sqlMode{HighlightSqlMode::TEXT}; ///< SQL scanner phase.
  HighlightMarkdownMode markdownMode{HighlightMarkdownMode::START}; ///< Markdown scanner phase.
  bool escapeString{};                                              ///< True only for explicit PostgreSQL E strings.
  bool escaped{};                                                   ///< Next E-string scalar is escaped.
  bool lineCandidate{}; ///< Delimiter began at a supported source line start.
  bool fenced{};        ///< A Markdown fence is open.
  bool sqlFence{};      ///< Current fence delegates its body to the same SQL scanner.
  DocumentLanguage embeddedLanguage{DocumentLanguage::POSTGRESQL_LEXICAL}; ///< Recognized SQL fence language.
  HighlightMySqlState mysql{};                                             ///< Fixed MySQL client/dump state.
  bool capturing{};          ///< SQL output is currently speculative fence-close output.
  bool infoStarted{};        ///< Fence info language word has begun.
  bool infoEnded{};          ///< Fence language word has ended or overflowed.
  bool statementStart{true}; ///< The next SQL word can introduce a COPY statement.
  bool copyHeader{};         ///< A statement-start COPY header is active.
  bool copyFrom{};           ///< A top-level COPY FROM token has been seen.
  bool copyStdin{};          ///< COPY FROM STDIN was recognized outside parentheses.
  bool copyAwaitLine{};      ///< Header semicolon was seen; next newline begins literal data.
  bool copyHeaderCr{};       ///< A header CR was admitted; an immediately following LF is still header whitespace.
  bool eofFinalized{};       ///< No further input may be admitted.
};

} // namespace detail

class DocumentHighlighter;

/** Owning copy of scanner state, including delayed output.
 * The caller must bind it to source identity/revision, decoding policy and language
 * profile. It does not authorize a cold seek or prove source immutability.
 */
class DocumentHighlightCheckpoint {
  friend class DocumentHighlighter;

  detail::HighlightState state_{}; ///< Owned state, deliberately unqualified by source revision.
};

/** Worker-side, allocation-free streaming lexical projection.
 * Single-owner mutation only; copied checkpoints can be transferred between workers.
 * No views, controls, source owners or callbacks are retained. Cancellation belongs
 * between bounded feed calls. Checkpoints must be source-qualified by the caller.
 */
class DocumentHighlighter {
public:
  /** Establish a proven initial lexical boundary.
   * @param options Explicit profile and bounded processing policy.
   * @param initialByteOffset Original-byte position at a known initial lexical context;
   * nonzero is an assertion by the caller, never inferred cold-seek recovery.
   */
  explicit DocumentHighlighter(DocumentHighlighterOptions options = {}, std::uint64_t initialByteOffset = 0) noexcept;

  /** Restore all owned scanner state without inspecting or retaining source bytes.
   * @param checkpoint State previously qualified against the caller's current source,
   * revision, decoding policy and language profile.
   */
  explicit DocumentHighlighter(const DocumentHighlightCheckpoint &checkpoint) noexcept;

  /** Process a bounded step; retry with unconsumed input and persistent EOF assertion.
   * @param input Consecutive decoded scalars borrowed only for this call.
   * @param output Caller-owned storage borrowed only for this call; zero capacity can
   * yield OUTPUT_FULL without progress and must be enlarged before retrying.
   * @param endOfInput True only when the supplied suffix ends at confirmed source EOF.
   * @return Committed input/output counts and stop reason. Errors do not consume their
   * triggering scalar; earlier committed progress remains valid. Pending keyword or
   * delimiter bytes may be emitted by a later call. Adjacent equal runs may coalesce.
   */
  DocumentHighlightResult feed(std::span<const DecodedScalar> input, std::span<TokenRun> output,
                               bool endOfInput = false) noexcept;

  /** Capture delayed prefixes and staged output as an owning fixed-size value.
   * @return Copyable state; caller must attach source/revision/decoding/profile guards.
   */
  DocumentHighlightCheckpoint checkpoint() const noexcept;

  /** Inspect the exact scalar admission boundary.
   * @return Required byte offset of the next input scalar, not a fully colored frontier.
   */
  std::uint64_t nextByteOffset() const noexcept;

private:
  detail::HighlightState state_{}; ///< Sole mutable scanner state; never shares external storage.
};

} // namespace portals
