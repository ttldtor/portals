// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/style.hpp>

#include <algorithm>
#include <array>
#include <limits>

namespace portals {
/** @brief PSS v0 declaration identities in metadata and compact descriptions. */
enum class PssProperty {
  FOREGROUND,                  ///< RGB text color.
  BACKGROUND,                  ///< RGB fill color.
  BOLD,                        ///< Explicit bold flag.
  ITALIC,                      ///< Explicit italic flag.
  UNDERLINE,                   ///< Explicit underline flag.
  MIN_WIDTH,                   ///< State-independent cell minimum.
  FRAME_LINE_FOREGROUND,       ///< Frame part frameLine.foreground.
  FRAME_LINE_BACKGROUND,       ///< Frame part frameLine.background.
  FRAME_LINE_BOLD,             ///< Frame part frameLine.bold.
  FRAME_LINE_ITALIC,           ///< Frame part frameLine.italic.
  FRAME_LINE_UNDERLINE,        ///< Frame part frameLine.underline.
  FRAME_TITLE_FOREGROUND,      ///< Frame part frameTitle.foreground.
  FRAME_TITLE_BACKGROUND,      ///< Frame part frameTitle.background.
  FRAME_TITLE_BOLD,            ///< Frame part frameTitle.bold.
  FRAME_TITLE_ITALIC,          ///< Frame part frameTitle.italic.
  FRAME_TITLE_UNDERLINE,       ///< Frame part frameTitle.underline.
  FRAME_ENDS_FOREGROUND,       ///< Frame part frameEnds.foreground.
  FRAME_ENDS_BACKGROUND,       ///< Frame part frameEnds.background.
  FRAME_ENDS_BOLD,             ///< Frame part frameEnds.bold.
  FRAME_ENDS_ITALIC,           ///< Frame part frameEnds.italic.
  FRAME_ENDS_UNDERLINE,        ///< Frame part frameEnds.underline.
  SCROLLBAR_TRACK_FOREGROUND,  ///< Scrollbar track foreground.
  SCROLLBAR_TRACK_BACKGROUND,  ///< Scrollbar track background.
  SCROLLBAR_TRACK_BOLD,        ///< Scrollbar track bold.
  SCROLLBAR_TRACK_ITALIC,      ///< Scrollbar track italic.
  SCROLLBAR_TRACK_UNDERLINE,   ///< Scrollbar track underline.
  SCROLLBAR_THUMB_FOREGROUND,  ///< Scrollbar thumb foreground.
  SCROLLBAR_THUMB_BACKGROUND,  ///< Scrollbar thumb background.
  SCROLLBAR_THUMB_BOLD,        ///< Scrollbar thumb bold.
  SCROLLBAR_THUMB_ITALIC,      ///< Scrollbar thumb italic.
  SCROLLBAR_THUMB_UNDERLINE,   ///< Scrollbar thumb underline.
  SCROLLBAR_ENDS_FOREGROUND,   ///< Scrollbar ends foreground.
  SCROLLBAR_ENDS_BACKGROUND,   ///< Scrollbar ends background.
  SCROLLBAR_ENDS_BOLD,         ///< Scrollbar ends bold.
  SCROLLBAR_ENDS_ITALIC,       ///< Scrollbar ends italic.
  SCROLLBAR_ENDS_UNDERLINE,    ///< Scrollbar ends underline.
  SCROLLBAR_ARROWS_FOREGROUND, ///< Scrollbar arrows foreground.
  SCROLLBAR_ARROWS_BACKGROUND, ///< Scrollbar arrows background.
  SCROLLBAR_ARROWS_BOLD,       ///< Scrollbar arrows bold.
  SCROLLBAR_ARROWS_ITALIC,     ///< Scrollbar arrows italic.
  SCROLLBAR_ARROWS_UNDERLINE,  ///< Scrollbar arrows underline.
  SCROLLBAR_CORNER_FOREGROUND, ///< Scrollbar corner foreground.
  SCROLLBAR_CORNER_BACKGROUND, ///< Scrollbar corner background.
  SCROLLBAR_CORNER_BOLD,       ///< Scrollbar corner bold.
  SCROLLBAR_CORNER_ITALIC,     ///< Scrollbar corner italic.
  SCROLLBAR_CORNER_UNDERLINE,  ///< Scrollbar corner underline.
};

/** @brief Number of supported PSS v0 properties. */
inline constexpr std::size_t PSS_PROPERTY_COUNT = 46;
/** @brief Marker for an absent effective declaration occurrence. */
inline constexpr std::size_t NO_PSS_DECLARATION = std::numeric_limits<std::size_t>::max();
/** @brief Maximum owned ASCII bytes retained in an error subject. */
inline constexpr std::size_t MAX_PSS_ERROR_SUBJECT_BYTES = 64;
/** @brief Maximum UTF-8 source identity bytes in an owning runtime document. */
inline constexpr std::size_t MAX_PSS_SOURCE_NAME_BYTES = 4096;
/** @brief Logical runtime document payload including all metadata occurrences. */
inline constexpr std::size_t MAX_PSS_DOCUMENT_BYTES = 4 * MAX_PSS_BYTES;

/** @brief Coordinates describe either original PSS bytes or explicit typed ordinals. */
enum class PssOriginKind {
  PSS_BYTES,      ///< Actual offsets, LF lines, and byte columns in original PSS input.
  SYNTHETIC_TYPED ///< Ordinals in a typed snapshot; they are not C++ file coordinates.
};

/** @brief Structured PSS errors independent of compiler wording. */
enum class PssErrorCode {
  NONE,                 ///< Successful admission.
  INPUT_LIMIT,          ///< Input bytes exceed the selected budget.
  RULE_LIMIT,           ///< Source-order rule count exceeds the selected budget.
  CLASS_LIMIT,          ///< Compact class predicates exceed their budget.
  DECLARATION_LIMIT,    ///< Compact declaration occurrences exceed their budget.
  TEXT_LIMIT,           ///< Compact owning text exceeds its shared arena.
  DOCUMENT_LIMIT,       ///< Runtime owning payload exceeds its logical budget.
  SOURCE_NAME_LIMIT,    ///< Source identity exceeds its byte budget.
  INVALID_UTF8,         ///< Invalid UTF-8 sequence anywhere in strict input.
  NUL_BYTE,             ///< Embedded NUL in strict input or source identity.
  EXPECTED_SELECTOR,    ///< No v0 selector at the current position.
  EXPECTED_IDENTIFIER,  ///< Expected an ASCII name.
  EXPECTED_PUNCTUATION, ///< Missing required punctuation.
  UNTERMINATED_COMMENT, ///< Missing comment terminator.
  DUPLICATE_ID,         ///< More than one ID predicate in a selector.
  DUPLICATE_STATE,      ///< More than one presentation-state predicate.
  UNKNOWN_STATE,        ///< Unsupported presentation state.
  UNKNOWN_PROPERTY,     ///< Unsupported v0 declaration name.
  INVALID_VALUE,        ///< Value does not have the property's exact scalar syntax.
  INVALID_WIDTH,        ///< Negative, overflowing, or state-dependent minimum width.
  ALLOCATION            ///< Runtime allocation or checked ownership transfer failed.
};

/** @brief Position in original PSS bytes, including an accepted leading BOM. */
struct PssLocation {
  std::size_t offset{};  ///< Zero-based byte offset.
  std::size_t line{1};   ///< One-based line, advanced only by LF.
  std::size_t column{1}; ///< One-based byte column.

  /** @param other Position to compare.
   * @return Whether all source coordinates agree.
   */
  constexpr bool operator==(const PssLocation &other) const = default;
};

/** @brief Allocation-free declaration scalar; its owning occurrence property identifies the active field. */
struct PssScalarValue {
  RgbColor color;       ///< FOREGROUND or BACKGROUND RGB; otherwise zero.
  bool boolean{};       ///< BOLD, ITALIC, or UNDERLINE flag, including explicit false.
  std::int32_t width{}; ///< MIN_WIDTH nonnegative cell minimum; otherwise zero.

  /** @param other Scalar payload to compare.
   * @return Whether all fixed scalar fields agree.
   */
  constexpr bool operator==(const PssScalarValue &other) const = default;
};

/** @brief Allocation-free error owning its bounded subject; message always has static lifetime. */
struct PssError {
  TreeError cause{TreeError::INVALID_ARGUMENT}; ///< Validation or runtime allocation category.
  PssErrorCode code{PssErrorCode::NONE};        ///< Specific parser/admission failure.
  PssLocation location; ///< Original byte coordinates, or identity coordinates for identity validation.
  std::array<char, MAX_PSS_ERROR_SUBJECT_BYTES> subject{}; ///< Owned ASCII subject; non-ASCII bytes become '?'.
  std::size_t subjectSize{}; ///< Populated subject bytes, without a terminator requirement.
  bool subjectTruncated{};   ///< Subject exceeded the fixed owning capacity.
  std::string_view message;  ///< Static English explanation, never a source view.

  /** @return Borrowed subject bytes owned by this error, valid until its mutation or destruction. */
  constexpr std::string_view subjectView() const noexcept {
    return {subject.data(), subjectSize};
  }
};

/** @brief Owning coordinates and occurrence links for one source-order rule. */
struct PssRuleOrigin {
  PssLocation selector;           ///< Start of the complete conjunction selector.
  std::size_t declarationBegin{}; ///< First occurrence index in PssDocument::declarations.
  std::size_t declarationCount{}; ///< Number of occurrences, including overwritten duplicates.
  std::array<std::size_t, PSS_PROPERTY_COUNT> effectiveDeclaration = [] {
    std::array<std::size_t, PSS_PROPERTY_COUNT> result{};
    result.fill(NO_PSS_DECLARATION);

    return result;
  }(); ///< Last occurrence for each property, or NO_PSS_DECLARATION.

  /** @param other Rule origin to compare.
   * @return Whether coordinates and occurrence relationships agree.
   */
  bool operator==(const PssRuleOrigin &other) const = default;
};

/** @brief One declaration's source coordinates, retained even when a later duplicate wins. */
struct PssDeclarationOrigin {
  std::size_t ruleIndex{}; ///< Source-order containing rule index.
  PssProperty property{};  ///< Destination identity.
  PssLocation location;    ///< Start of the property name.
  PssScalarValue value;    ///< Original scalar, retained when a later duplicate overwrites it.

  /** @param other Declaration origin to compare.
   * @return Whether containment, destination, and coordinates agree.
   */
  bool operator==(const PssDeclarationOrigin &other) const = default;
};

/** @brief Owning parsed declarations and provenance; input bytes need not survive the call.
 * Metadata is not automatically installed into a scene by CT-PSS v0.
 */
struct PssDocument {
  /** @brief Suppress implicit noexcept moves because checked MSVC container transfers can allocate. */
  ~PssDocument() = default;

  StyleSheet sheet; ///< Existing source-order runtime declarations, using the existing cascade.
  PssOriginKind originKind{PssOriginKind::PSS_BYTES}; ///< Coordinate interpretation for this document.
  std::string sourceName{""};                         ///< Owned caller-supplied source identity; empty means anonymous.
  std::vector<PssRuleOrigin> origins = std::vector<PssRuleOrigin>(0);                    ///< One origin per rule.
  std::vector<PssDeclarationOrigin> declarations = std::vector<PssDeclarationOrigin>(0); ///< All occurrences.

  /** @param other Owning document to compare.
   * @return Whether declarations, identity, and complete provenance agree.
   */
  bool operator==(const PssDocument &other) const = default;
};

/** @brief Parse strict UTF-8 PSS v0 into owning declarations and source metadata, without I/O.
 * @param bytes Borrowed input, at most MAX_PSS_BYTES; permits one leading UTF-8 BOM, rejects NUL.
 * @param sourceName Borrowed UTF-8 identity copied on success, at most MAX_PSS_SOURCE_NAME_BYTES.
 * @return Complete owning document or an owning error; no partial result is published.
 * Logical payload includes all repeated declaration origins and is bounded by MAX_PSS_DOCUMENT_BYTES.
 * File/scene integration is separate. Allocation and checked transfer failures map to ALLOCATION/NO_MEMORY.
 */
std::expected<PssDocument, PssError> parsePssDocument(std::string_view bytes, std::string_view sourceName = {});
} // namespace portals
