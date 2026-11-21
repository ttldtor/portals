// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace portals {
/// Explicit lexical profiles; unknown names resolve to PLAIN.
enum class DocumentLanguage {
  PLAIN,              ///< Source bytes have the normal role.
  POSTGRESQL_LEXICAL, ///< Bounded PostgreSQL source profile, not a semantic parser.
  MYSQL_LEXICAL,      ///< Bounded MySQL dump/stored-program source profile.
  MARKDOWN_SOURCE     ///< Bounded raw Markdown source profile, not a renderer.
};

/** @brief Syntax projection state; text remains readable while context is prepared. */
enum class DocumentSyntaxState {
  DISABLED,            ///< Plain text selected; no lexical worker or cache is needed.
  PENDING,             ///< Some displayed source clusters have no proven lexical role yet.
  READY,               ///< All displayed clusters have source-qualified lexical roles.
  UNSUPPORTED_PROFILE, ///< A construct exceeds the selected lexical profile; plain text remains available.
  RESOURCE_LIMIT,      ///< The fixed lexical policy was exhausted; plain text remains available.
  UNAVAILABLE,         ///< Optional syntax work failed; the plain frame remains readable.
  REPLAY_UNAVAILABLE,  ///< The source cannot restore the required earlier lexical context.
  PREVIEW              ///< A readable provisional projection remains after bounded refinement stops.
};

/// Recovery policy; the language still selects the lexical rules.
enum class DocumentSyntaxPolicy {
  AUTO,    ///< Preview immediately and refine within the automatic source-admission budget.
  PREVIEW, ///< Reuse known exact context or show bounded local-fragment colors without long recovery.
  EXACT    ///< Allow cancellable recovery from a proven checkpoint or the real source origin.
};

/// Accuracy of the syntax roles actually displayed, independently of recovery errors.
enum class DocumentSyntaxAccuracy {
  NONE,    ///< Plain source text without syntax-role projection.
  PREVIEW, ///< Best-effort local-fragment roles; the enclosing lexical context is not proven.
  EXACT    ///< Source-qualified roles within the selected lexical profile.
};

/// Compact typed policy values for viewer descriptions and commands.
namespace syntax {
inline constexpr auto AUTO = DocumentSyntaxPolicy::AUTO;       ///< Bounded automatic refinement.
inline constexpr auto PREVIEW = DocumentSyntaxPolicy::PREVIEW; ///< Local work only.
inline constexpr auto EXACT = DocumentSyntaxPolicy::EXACT;     ///< Explicit full recovery.
} // namespace syntax

/// Theme-independent source roles implemented by these profiles.
enum class SemanticRole {
  NORMAL,     ///< Unclassified source and whitespace.
  KEYWORD,    ///< Recognized SQL keyword.
  NUMBER,     ///< Decimal SQL number component.
  STRING,     ///< SQL literal or literal Markdown code region.
  COMMENT,    ///< SQL comment or executable-comment wrapper bytes.
  IDENTIFIER, ///< SQL unquoted or quoted identifier.
  OPERATOR,   ///< SQL punctuation or operator character.
  HEADING,    ///< Markdown ATX heading marker and text.
  MARKUP      ///< Markdown delimiter/punctuation or MySQL client directive/terminator.
};

/// One nonempty semantic interval in original source bytes.
struct TokenRun {
  std::uint64_t byteOffset{}; ///< Inclusive source byte start.
  std::uint64_t byteLength{}; ///< Number of original bytes, never decoded characters.
  SemanticRole role{};        ///< Semantic role for every byte in this interval.
};

/// Streaming progress, caller backpressure or an explicit lexical limitation.
enum class DocumentHighlightStatus {
  NEED_INPUT,            ///< Supplied scalars are consumed; more input or EOF is needed.
  OUTPUT_FULL,           ///< Supply a nonempty output span and resume unconsumed input.
  WORK_LIMIT,            ///< Resume after this call exhausted its bounded work budget.
  DONE,                  ///< EOF finalized and all staged runs delivered.
  INVALID_CONFIGURATION, ///< Invalid language, zero work limit or out-of-range limits.
  INVALID_INPUT,         ///< Non-scalar value, zero length, discontinuity or uint64 overflow.
  UNSUPPORTED_PROFILE,   ///< Construct requires a different lexical profile.
  RESOURCE_LIMIT         ///< A delimiter, nesting or fixed mode-item capacity is exceeded.
};

/// Committed progress; errors leave their triggering transition uncommitted.
struct DocumentHighlightResult {
  std::size_t inputConsumed{};      ///< Decoded scalars admitted during this call.
  std::size_t outputProduced{};     ///< Runs written to caller storage during this call.
  DocumentHighlightStatus status{}; ///< Reason this bounded call stopped.
  std::uint64_t errorOffset{};      ///< Triggering scalar offset, or delayed construct start.
};

/// MySQL lexical string modes supported initially and by source SET SQL_MODE.
enum class DocumentMySqlMode {
  NORMAL,              ///< Backslash escapes inside single/double quoted strings.
  NO_BACKSLASH_ESCAPES ///< Literal backslashes; doubled delimiters still quote themselves.
};

/// Fixed-capacity policy copied into checkpoints.
struct DocumentHighlighterOptions {
  DocumentLanguage language{DocumentLanguage::PLAIN}; ///< Explicit supported lexical profile.
  std::size_t workLimit{4096}; ///< Scalar admissions and run deliveries per call; nonzero. EOF adds one fixed step.
  std::uint8_t maxDelimiterLength{32}; ///< SQL tag, MySQL client or Markdown delimiter capacity, in characters; 1..64.
  std::uint16_t maxCommentDepth{64};   ///< Maximum simultaneously open SQL block comments; nonzero.
  DocumentMySqlMode mysqlMode{DocumentMySqlMode::NORMAL}; ///< Initial MySQL escape policy; ANSI_QUOTES unsupported.
  std::uint32_t mysqlServerVersion{80400};                ///< Version-comment target in Mmmrr/MMmmrr form, 1..999999.
};

/** Resolve supported ASCII names without allocating.
 * @param name Case-insensitive plain/text, sql/postgresql/postgres, mysql or markdown/md name.
 * @return Explicit lexical profile; any unknown name resolves to PLAIN.
 */
DocumentLanguage documentLanguageFromName(std::string_view name) noexcept;

} // namespace portals
