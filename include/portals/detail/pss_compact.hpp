// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/pss_parser.hpp>

/// @brief Owning compile-time PSS v0 snapshots using the existing runtime cascade.
namespace portals::style::ct {
/** @brief Maximum compile-time source bytes, independently of the larger runtime budget. */
inline constexpr std::size_t MAX_CT_PSS_BYTES = 16 * 1024;
/** @brief Maximum compile-time source-order rules. */
inline constexpr std::size_t MAX_CT_PSS_RULES = 256;
/** @brief Maximum compile-time class predicates across the whole sheet. */
inline constexpr std::size_t MAX_CT_PSS_CLASSES = 1024;
/** @brief Maximum compile-time declaration occurrences, including overwritten duplicates. */
inline constexpr std::size_t MAX_CT_PSS_DECLARATIONS = 1536;
/** @brief Maximum shared owning selector and source-identity text bytes. */
inline constexpr std::size_t MAX_CT_PSS_TEXT_BYTES = 16 * 1024;
/** @brief Maximum caller-supplied compile-time UTF-8 source identity bytes. */
inline constexpr std::size_t MAX_CT_PSS_SOURCE_NAME_BYTES = 512;
/** @brief Maximum complete compact descriptor object size. */
inline constexpr std::size_t MAX_CT_PSS_DESCRIPTOR_BYTES = 192 * 1024;

namespace detail {
using portals::detail::pssError;
using portals::detail::PssParser;
using portals::detail::PssPolicy;
using portals::detail::PssSpan;
using portals::detail::PssValue;

/** @brief Compact owning-arena reference; all admitted CT text fits uint32_t. */
struct PssText {
  std::uint32_t offset{}; ///< Start within the descriptor's shared arena.
  std::uint32_t size{};   ///< Byte count.
};

/** @brief Compact original-source coordinates within the named CT byte limit. */
struct PssPosition {
  std::uint32_t offset{};  ///< Zero-based byte offset.
  std::uint32_t line{1};   ///< One-based LF line.
  std::uint32_t column{1}; ///< One-based byte column.
};

/** @brief Compact absent occurrence marker. */
inline constexpr auto NO_COMPACT_DECLARATION = std::numeric_limits<std::uint32_t>::max();

/** @brief Compact owning source-order rule data, without per-rule text arenas. */
struct PssCompactRule {
  PssText type;                      ///< Optional type identity in the shared arena.
  PssText id;                        ///< Optional target identity in the shared arena.
  std::uint32_t classBegin{};        ///< First class-reference index.
  std::uint32_t classCount{};        ///< Number of conjunction classes.
  StyleState state{StyleState::ANY}; ///< Presentation-state predicate.
  StyleDeclarations values;          ///< Last-winner scalar declarations.
  PssPosition selector;              ///< Original selector start.
  std::uint32_t declarationBegin{};  ///< First declaration occurrence index.
  std::uint32_t declarationCount{};  ///< All occurrences, including duplicates.
  std::array<std::uint32_t, PSS_PROPERTY_COUNT> effectiveDeclaration = [] {
    std::array<std::uint32_t, PSS_PROPERTY_COUNT> result{};
    result.fill(NO_COMPACT_DECLARATION);

    return result;
  }(); ///< Last winning occurrence for each property.
};

/** @brief Compact declaration occurrence with source coordinates and containment. */
struct PssCompactDeclaration {
  std::uint32_t ruleIndex{}; ///< Source-order containing rule.
  PssProperty property{};    ///< Declaration destination.
  PssPosition location;      ///< Original property start.
  PssScalarValue value;      ///< Original owning scalar, including overwritten duplicate values.
};

/** @param location Admitted original coordinates.
 * @return Compact coordinates; CT budgets bound every component.
 */
constexpr PssPosition compactPosition(PssLocation location) {
  return {static_cast<std::uint32_t>(location.offset), static_cast<std::uint32_t>(location.line),
          static_cast<std::uint32_t>(location.column)};
}

/** @param location Compact original coordinates.
 * @return Ordinary runtime coordinates with identical values.
 */
constexpr PssLocation expandPosition(PssPosition location) {
  return {location.offset, location.line, location.column};
}

/** @brief Shared admission counts computed before exact-sized CT construction. */
struct PssCounts {
  std::size_t rules{};        ///< Rules in source order.
  std::size_t classes{};      ///< Total conjunction classes.
  std::size_t declarations{}; ///< Total declaration occurrences.
  std::size_t bytes{};        ///< All copied selector and source-name text.
};

/** @brief Allocation-free counting sink with the same limits as the storing sink. */
struct PssCountSink {
  PssCounts counts; ///< Current admitted counts.

  /** @param location Rule source coordinates.
   * @return Admission result.
   */
  constexpr std::expected<void, PssError> beginRule(PssLocation location) {
    if (counts.rules == MAX_CT_PSS_RULES) {
      return std::unexpected(pssError(PssErrorCode::RULE_LIMIT, location, "Too many compile-time style rules"));
    }

    ++counts.rules;

    return {};
  }

  /** @param span Source text span counted as owned bytes.
   * @param location Original coordinates for overflow diagnostics.
   * @return Admission result.
   */
  constexpr std::expected<void, PssError> type(PssSpan span, PssLocation location) {
    return text(span.size, location);
  }

  /** @param span Source ID text span.
   * @param location Original source coordinates.
   * @return Admission result.
   */
  constexpr std::expected<void, PssError> id(PssSpan span, PssLocation location) {
    return text(span.size, location);
  }

  /** @param span Source class text span.
   * @param location Original source coordinates.
   * @return Admission result.
   */
  constexpr std::expected<void, PssError> className(PssSpan span, PssLocation location) {
    if (counts.classes == MAX_CT_PSS_CLASSES) {
      return std::unexpected(pssError(PssErrorCode::CLASS_LIMIT, location, "Too many compile-time selector classes"));
    }

    ++counts.classes;

    return text(span.size, location);
  }

  /** @param state Validated state, which consumes no additional record.
   * @return Successful admission.
   */
  constexpr std::expected<void, PssError> state(StyleState state) {
    (void)state;

    return {};
  }

  /** @param property Validated destination.
   * @param value Validated scalar.
   * @param location Original property coordinates.
   * @return Admission result counting every occurrence.
   */
  constexpr std::expected<void, PssError> declaration(PssProperty property, PssValue value, PssLocation location) {
    (void)property;
    (void)value;

    if (counts.declarations == MAX_CT_PSS_DECLARATIONS) {
      return std::unexpected(
        pssError(PssErrorCode::DECLARATION_LIMIT, location, "Too many compile-time declaration occurrences"));
    }

    ++counts.declarations;

    return {};
  }

  /** @return Successful complete-rule admission. */
  constexpr std::expected<void, PssError> endRule() {
    return {};
  }

  /** @param bytes Text bytes admitted into the one shared arena.
   * @param location Original coordinates for a limit error.
   * @return Admission result with overflow-safe subtraction.
   */
  constexpr std::expected<void, PssError> text(std::size_t bytes, PssLocation location) {
    if (bytes > MAX_CT_PSS_TEXT_BYTES - counts.bytes) {
      return std::unexpected(
        pssError(PssErrorCode::TEXT_LIMIT, location, "Compile-time PSS exceeds owning text budget"));
    }

    counts.bytes += bytes;

    return {};
  }
};

/** @tparam R Numeric rule capacity.
 * @tparam C Numeric class-reference capacity.
 * @tparam D Numeric declaration-occurrence capacity.
 * @tparam B Numeric owning-text capacity.
 */
template <std::size_t R, std::size_t C, std::size_t D, std::size_t B> class PssCompactSink;
} // namespace detail

/** @brief Immutable owning scalar stylesheet; its public type contains only numeric capacities.
 * @tparam RuleCapacity Maximum stored rules, bounded by MAX_CT_PSS_RULES.
 * @tparam ClassCapacity Maximum global class references, bounded by MAX_CT_PSS_CLASSES.
 * @tparam DeclarationCapacity Maximum global occurrences, bounded by MAX_CT_PSS_DECLARATIONS.
 * @tparam ByteCapacity One owning text arena, bounded by MAX_CT_PSS_TEXT_BYTES.
 * Snapshot materialization uses the existing cascade; parameter reactivity remains the CT-002 LiveTheme API.
 */
template <std::size_t RuleCapacity, std::size_t ClassCapacity, std::size_t DeclarationCapacity,
          std::size_t ByteCapacity>
class CompactSheet {
  static_assert(RuleCapacity <= MAX_CT_PSS_RULES && ClassCapacity <= MAX_CT_PSS_CLASSES &&
                  DeclarationCapacity <= MAX_CT_PSS_DECLARATIONS && ByteCapacity <= MAX_CT_PSS_TEXT_BYTES,
                "Compact stylesheet capacities exceed CT PSS budgets");

public:
  /** @brief Construct a valid empty owning description. */
  constexpr CompactSheet() = default;

  /** @return Stored rules as borrowed immutable data, valid for this object's lifetime. */
  constexpr std::span<const detail::PssCompactRule> rules() const noexcept {
    return {rules_.data(), ruleCount_};
  }

  /** @return Global immutable class references into this object's text arena. */
  constexpr std::span<const detail::PssText> classes() const noexcept {
    return {classes_.data(), classCount_};
  }

  /** @return All immutable source declaration occurrences, including overwritten duplicates. */
  constexpr std::span<const detail::PssCompactDeclaration> declarations() const noexcept {
    return {declarations_.data(), declarationCount_};
  }

  /** @param range Admitted reference obtained from this descriptor's immutable records.
   * @return Borrowed owned bytes; the reference must belong to this descriptor.
   */
  constexpr std::string_view text(detail::PssText range) const noexcept {
    if (range.size == 0) {
      return {};
    }

    return {bytes_.data() + range.offset, range.size};
  }

  /** @return Owned caller-supplied source identity, or empty for anonymous input. */
  constexpr std::string_view sourceName() const noexcept {
    return text(sourceName_);
  }

  /** @return Whether metadata describes actual PSS bytes or synthetic typed ordinals. */
  constexpr PssOriginKind originKind() const noexcept {
    return originKind_;
  }

  /** @return Populated shared text bytes, excluding unused conservative capacity. */
  constexpr std::size_t ownedTextBytes() const noexcept {
    return byteCount_;
  }

private:
  template <std::size_t R, std::size_t C, std::size_t D, std::size_t B> friend class detail::PssCompactSink;

  std::array<detail::PssCompactRule, RuleCapacity> rules_{};
  std::array<detail::PssText, ClassCapacity> classes_{};
  std::array<detail::PssCompactDeclaration, DeclarationCapacity> declarations_{};
  std::array<char, ByteCapacity> bytes_{};
  detail::PssText sourceName_;
  PssOriginKind originKind_{PssOriginKind::PSS_BYTES};
  std::size_t ruleCount_{};
  std::size_t classCount_{};
  std::size_t declarationCount_{};
  std::size_t byteCount_{};
};

namespace detail {
/** @brief Allocation-free storing sink writing one bounded owning descriptor.
 * @tparam R Numeric rule capacity.
 * @tparam C Numeric class-reference capacity.
 * @tparam D Numeric declaration-occurrence capacity.
 * @tparam B Numeric owning-text capacity.
 */
template <std::size_t R, std::size_t C, std::size_t D, std::size_t B> class PssCompactSink {
public:
  /** @param source Borrowed original source spans, retained only during construction.
   * @param sheet Owning output destination, borrowed for this synchronous operation.
   */
  constexpr PssCompactSink(std::string_view source, CompactSheet<R, C, D, B> &sheet) : source_(source), sheet_(sheet) {
    static_assert(sizeof(CompactSheet<R, C, D, B>) <= MAX_CT_PSS_DESCRIPTOR_BYTES,
                  "Compact stylesheet object exceeds descriptor budget");
  }

  /** @param name Validated source identity copied into the shared arena.
   * @return Admission result.
   */
  constexpr std::expected<void, PssError> sourceName(std::string_view name) {
    return copyText(name, {}, sheet_.sourceName_);
  }

  /** @param kind Honest coordinate interpretation for the stored source metadata. */
  constexpr void originKind(PssOriginKind kind) {
    sheet_.originKind_ = kind;
  }

  /** @param location Original selector start.
   * @return Admission result before any rule storage is accessed.
   */
  constexpr std::expected<void, PssError> beginRule(PssLocation location) {
    if (sheet_.ruleCount_ == R) {
      return std::unexpected(pssError(PssErrorCode::RULE_LIMIT, location, "Too many compile-time style rules"));
    }

    auto &rule = current();

    rule.selector = compactPosition(location);
    rule.classBegin = static_cast<std::uint32_t>(sheet_.classCount_);
    rule.declarationBegin = static_cast<std::uint32_t>(sheet_.declarationCount_);

    return {};
  }

  /** @param span Original type-name bytes.
   * @param location Source coordinates for admission errors.
   * @return Owning text-copy result.
   */
  constexpr std::expected<void, PssError> type(PssSpan span, PssLocation location) {
    return typeName(source_.substr(span.offset, span.size), location);
  }

  /** @param span Original ID bytes.
   * @param location Source coordinates for admission errors.
   * @return Owning text-copy result.
   */
  constexpr std::expected<void, PssError> id(PssSpan span, PssLocation location) {
    return idName(source_.substr(span.offset, span.size), location);
  }

  /** @param span Original class-name bytes.
   * @param location Source coordinates for admission errors.
   * @return Owning reference admission result.
   */
  constexpr std::expected<void, PssError> className(PssSpan span, PssLocation location) {
    return classBytes(source_.substr(span.offset, span.size), location);
  }

  /** @param bytes Static typed or original parsed type identity, copied before returning.
   * @param location Source coordinates for admission errors.
   * @return Owning text admission result.
   */
  constexpr std::expected<void, PssError> typeName(std::string_view bytes, PssLocation location) {
    return copyText(bytes, location, current().type);
  }

  /** @param bytes Static typed or original parsed ID, copied before returning.
   * @param location Source coordinates for admission errors.
   * @return Owning text admission result.
   */
  constexpr std::expected<void, PssError> idName(std::string_view bytes, PssLocation location) {
    return copyText(bytes, location, current().id);
  }

  /** @param bytes Static typed or original parsed class, copied before returning.
   * @param location Source coordinates for admission errors.
   * @return Owning class admission result.
   */
  constexpr std::expected<void, PssError> classBytes(std::string_view bytes, PssLocation location) {
    if (sheet_.classCount_ == C) {
      return std::unexpected(pssError(PssErrorCode::CLASS_LIMIT, location, "Too many compile-time selector classes"));
    }

    auto admitted = copyText(bytes, location, sheet_.classes_[sheet_.classCount_]);

    if (!admitted) {
      return admitted;
    }

    ++sheet_.classCount_;
    ++current().classCount;

    return {};
  }

  /** @param value Validated existing presentation state.
   * @return Successful scalar storage.
   */
  constexpr std::expected<void, PssError> state(StyleState value) {
    current().state = value;

    return {};
  }

  /** @param property Validated property destination.
   * @param value Validated single scalar declaration.
   * @param location Original property start.
   * @return Occurrence admission and last-winner scalar storage result.
   */
  constexpr std::expected<void, PssError> declaration(PssProperty property, PssValue value, PssLocation location) {
    if (sheet_.declarationCount_ == D) {
      return std::unexpected(
        pssError(PssErrorCode::DECLARATION_LIMIT, location, "Too many compile-time declaration occurrences"));
    }

    auto &rule = current();

    rule.effectiveDeclaration[static_cast<std::size_t>(property)] =
      static_cast<std::uint32_t>(sheet_.declarationCount_);
    ++rule.declarationCount;
    portals::detail::assignPssDeclaration(property, value, rule.values);
    sheet_.declarations_[sheet_.declarationCount_++] = {static_cast<std::uint32_t>(sheet_.ruleCount_), property,
                                                        compactPosition(location), value};

    return {};
  }

  /** @return Successful complete-rule publication within the private CT construction. */
  constexpr std::expected<void, PssError> endRule() {
    ++sheet_.ruleCount_;

    return {};
  }

  /** @param bytes Borrowed name bytes copied before returning.
   * @param location Original coordinates for an arena error.
   * @param destination Stored owning-arena reference populated on success.
   * @return Overflow-safe text admission result.
   */
  constexpr std::expected<void, PssError> copyText(std::string_view bytes, PssLocation location, PssText &destination) {
    if (bytes.size() > B - sheet_.byteCount_) {
      return std::unexpected(
        pssError(PssErrorCode::TEXT_LIMIT, location, "Compile-time PSS exceeds owning text budget"));
    }

    destination = {static_cast<std::uint32_t>(sheet_.byteCount_), static_cast<std::uint32_t>(bytes.size())};

    for (const auto value : bytes) {
      sheet_.bytes_[sheet_.byteCount_++] = value;
    }

    return {};
  }

  /** @tparam SourceR Admitted source rule capacity.
   * @tparam SourceC Admitted source class capacity.
   * @tparam SourceD Admitted source occurrence capacity.
   * @tparam SourceB Admitted source text capacity.
   * @param source Already validated owning staging description.
   * @return Checked copy into exact capacities, without another grammar pass or borrowed source retention.
   */
  template <std::size_t SourceR, std::size_t SourceC, std::size_t SourceD, std::size_t SourceB>
  constexpr std::expected<void, PssError> copyFrom(const CompactSheet<SourceR, SourceC, SourceD, SourceB> &source) {
    if (source.ruleCount_ > R || source.classCount_ > C || source.declarationCount_ > D || source.byteCount_ > B) {
      return std::unexpected(pssError(PssErrorCode::DOCUMENT_LIMIT, {}, "Compact PSS copy exceeds admitted capacity"));
    }

    sheet_.ruleCount_ = source.ruleCount_;
    sheet_.classCount_ = source.classCount_;
    sheet_.declarationCount_ = source.declarationCount_;
    sheet_.byteCount_ = source.byteCount_;
    sheet_.sourceName_ = source.sourceName_;
    sheet_.originKind_ = source.originKind_;

    for (std::size_t index = 0; index < source.ruleCount_; ++index) {
      sheet_.rules_[index] = source.rules_[index];
    }

    for (std::size_t index = 0; index < source.classCount_; ++index) {
      sheet_.classes_[index] = source.classes_[index];
    }

    for (std::size_t index = 0; index < source.declarationCount_; ++index) {
      sheet_.declarations_[index] = source.declarations_[index];
    }

    for (std::size_t index = 0; index < source.byteCount_; ++index) {
      sheet_.bytes_[index] = source.bytes_[index];
    }

    return {};
  }

private:
  constexpr PssCompactRule &current() {
    return sheet_.rules_[sheet_.ruleCount_];
  }

  std::string_view source_;
  CompactSheet<R, C, D, B> &sheet_;
};
} // namespace detail
} // namespace portals::style::ct
