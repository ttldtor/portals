// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/detail/pss_compact.hpp>
#include <portals/style_static.hpp>

namespace portals::style::ct {
namespace detail {
/** @brief Named immediate-evaluation boundary for direct literal grammar errors.
 * @param error Owning first parser error selected during immediate evaluation.
 */
void pssInvalidSyntax(PssError error);
/** @brief Named immediate-evaluation boundary for direct literal selector errors.
 * @param error Owning selector diagnostic selected during immediate evaluation.
 */
void pssInvalidSelector(PssError error);
/** @brief Named immediate-evaluation boundary for unknown direct literal properties.
 * @param error Owning unknown-property diagnostic selected during immediate evaluation.
 */
void pssUnknownProperty(PssError error);
/** @brief Named immediate-evaluation boundary for invalid scalar values.
 * @param error Owning scalar diagnostic selected during immediate evaluation.
 */
void pssInvalidValue(PssError error);
/** @brief Named immediate-evaluation boundary for invalid minimum widths.
 * @param error Owning minimum-width diagnostic selected during immediate evaluation.
 */
void pssInvalidWidth(PssError error);
/** @brief Named immediate-evaluation boundary for strict UTF-8 or NUL admission.
 * @param error Owning encoding diagnostic selected during immediate evaluation.
 */
void pssInvalidUtf8(PssError error);
/** @brief Named immediate-evaluation boundary for compile-time capacity admission.
 * @param error Owning budget diagnostic selected during immediate evaluation.
 */
void pssLimitExceeded(PssError error);

/** @param error Complete owning parser error selected during immediate evaluation.
 * Direct function parameters cannot become C++23 template diagnostic arguments.
 * These boundaries retain the first category and compiler invocation location without input NTTPs.
 */
constexpr void rejectPss(PssError error) {
  switch (error.code) {
  case PssErrorCode::UNKNOWN_PROPERTY:
    pssUnknownProperty(error);

    break;
  case PssErrorCode::INVALID_VALUE:
    pssInvalidValue(error);

    break;
  case PssErrorCode::INVALID_WIDTH:
    pssInvalidWidth(error);

    break;
  case PssErrorCode::INVALID_UTF8:
  case PssErrorCode::NUL_BYTE:
    pssInvalidUtf8(error);

    break;
  case PssErrorCode::EXPECTED_SELECTOR:
  case PssErrorCode::EXPECTED_IDENTIFIER:
  case PssErrorCode::DUPLICATE_ID:
  case PssErrorCode::DUPLICATE_STATE:
  case PssErrorCode::UNKNOWN_STATE:
    pssInvalidSelector(error);

    break;
  case PssErrorCode::INPUT_LIMIT:
  case PssErrorCode::RULE_LIMIT:
  case PssErrorCode::CLASS_LIMIT:
  case PssErrorCode::DECLARATION_LIMIT:
  case PssErrorCode::TEXT_LIMIT:
  case PssErrorCode::DOCUMENT_LIMIT:
  case PssErrorCode::SOURCE_NAME_LIMIT:
    pssLimitExceeded(error);

    break;
  default:
    pssInvalidSyntax(error);

    break;
  }
}

/** @tparam Code First admission category selected by immediate evaluation.
 * @return True after exactly one category-specific static assertion is instantiated.
 * Fixed messages are portable C++23; source-dependent text remains in the bounded carrier arguments.
 */
template <PssErrorCode Code> consteval bool pssReason() {
  if constexpr (Code == PssErrorCode::UNKNOWN_PROPERTY) {
    static_assert(Code == PssErrorCode::NONE, "Unknown PSS property");
  } else if constexpr (Code == PssErrorCode::EXPECTED_SELECTOR) {
    static_assert(Code == PssErrorCode::NONE, "Invalid PSS selector: expected type, ID, class, state, or '*'");
  } else if constexpr (Code == PssErrorCode::EXPECTED_IDENTIFIER) {
    static_assert(Code == PssErrorCode::NONE, "Expected PSS ASCII identifier");
  } else if constexpr (Code == PssErrorCode::DUPLICATE_ID) {
    static_assert(Code == PssErrorCode::NONE, "Invalid PSS selector: duplicate ID");
  } else if constexpr (Code == PssErrorCode::DUPLICATE_STATE) {
    static_assert(Code == PssErrorCode::NONE, "Invalid PSS selector: duplicate state");
  } else if constexpr (Code == PssErrorCode::UNKNOWN_STATE) {
    static_assert(Code == PssErrorCode::NONE, "Unknown PSS presentation state");
  } else if constexpr (Code == PssErrorCode::INVALID_VALUE) {
    static_assert(Code == PssErrorCode::NONE, "Invalid PSS property value: expected RGB color or true/false");
  } else if constexpr (Code == PssErrorCode::INVALID_WIDTH) {
    static_assert(Code == PssErrorCode::NONE,
                  "Invalid PSS min-width: expected nonnegative state-independent cell width");
  } else if constexpr (Code == PssErrorCode::INVALID_UTF8) {
    static_assert(Code == PssErrorCode::NONE, "Invalid UTF-8 in PSS input or source identity");
  } else if constexpr (Code == PssErrorCode::NUL_BYTE) {
    static_assert(Code == PssErrorCode::NONE, "PSS input or source identity contains NUL");
  } else if constexpr (Code == PssErrorCode::INPUT_LIMIT) {
    static_assert(Code == PssErrorCode::NONE, "PSS input exceeds MAX_CT_PSS_BYTES");
  } else if constexpr (Code == PssErrorCode::RULE_LIMIT) {
    static_assert(Code == PssErrorCode::NONE, "PSS rules exceed MAX_CT_PSS_RULES");
  } else if constexpr (Code == PssErrorCode::CLASS_LIMIT) {
    static_assert(Code == PssErrorCode::NONE, "PSS selector classes exceed MAX_CT_PSS_CLASSES");
  } else if constexpr (Code == PssErrorCode::DECLARATION_LIMIT) {
    static_assert(Code == PssErrorCode::NONE, "PSS declaration occurrences exceed MAX_CT_PSS_DECLARATIONS");
  } else if constexpr (Code == PssErrorCode::TEXT_LIMIT) {
    static_assert(Code == PssErrorCode::NONE, "PSS owned text exceeds MAX_CT_PSS_TEXT_BYTES");
  } else if constexpr (Code == PssErrorCode::SOURCE_NAME_LIMIT) {
    static_assert(Code == PssErrorCode::NONE, "PSS source identity exceeds MAX_CT_PSS_SOURCE_NAME_BYTES");
  } else if constexpr (Code == PssErrorCode::EXPECTED_PUNCTUATION) {
    static_assert(Code == PssErrorCode::NONE, "Expected PSS punctuation: '{', ':', ';', or '}'");
  } else if constexpr (Code == PssErrorCode::UNTERMINATED_COMMENT) {
    static_assert(Code == PssErrorCode::NONE, "Unterminated PSS block comment");
  } else {
    static_assert(Code == PssErrorCode::NONE, "Invalid PSS input or owning output capacity");
  }

  return true;
}

/** @brief Bounded C++23 diagnostic carrier; compiler formatting of subject bytes differs.
 * @tparam Code First parser/admission error category.
 * @tparam Offset Original zero-based byte offset.
 * @tparam Line Original one-based LF line.
 * @tparam Column Original one-based byte column.
 * @tparam Subject Bounded owning ASCII error subject, never the complete input.
 */
template <PssErrorCode Code, std::size_t Offset, std::size_t Line, std::size_t Column,
          std::array<char, MAX_PSS_ERROR_SUBJECT_BYTES> Subject>
struct PssFailure {
  static_assert(pssReason<Code>());
};

/** @brief Admission counts and first error for a provider, without source ownership. */
struct PssInspection {
  PssCounts counts; ///< Admitted output counts, meaningful only when valid is true.
  PssError error;   ///< First owning diagnostic.
  bool valid{};     ///< Complete strict grammar and output admission succeeded.
};

/** @param name Borrowed source identity used only for strict admission.
 * @return Valid UTF-8/NUL/name-size admission result.
 */
constexpr std::expected<void, PssError> validateCtPssName(std::string_view name) {
  if (name.size() > MAX_CT_PSS_SOURCE_NAME_BYTES) {
    return std::unexpected(
      pssError(PssErrorCode::SOURCE_NAME_LIMIT, {}, "Compile-time PSS source identity exceeds budget"));
  }

  return portals::detail::validatePssUtf8(name);
}

/** @param bytes Borrowed source with an explicitly preserved byte extent.
 * @param sourceName Borrowed UTF-8 identity included in owning-text admission.
 * @return Strict shared-parser admission and exact numeric storage counts.
 */
constexpr PssInspection inspectPss(std::string_view bytes, std::string_view sourceName) {
  auto name = validateCtPssName(sourceName);

  if (!name) {
    return {{}, name.error(), false};
  }

  PssCountSink sink;
  auto copied = sink.text(sourceName.size(), {});

  if (!copied) {
    return {sink.counts, copied.error(), false};
  }

  auto parsed = PssParser(bytes, sink, PssPolicy::STRICT_UTF8, MAX_CT_PSS_BYTES, MAX_CT_PSS_RULES).parse();

  if (!parsed) {
    return {sink.counts, parsed.error(), false};
  }

  return {sink.counts, {}, true};
}

/** @brief Default anonymous provider preserves a zero-byte string_view extent. */
struct AnonymousPssSource {
  /** @return Empty anonymous source identity. */
  constexpr std::string_view operator()() const {
    return {};
  }
};

/** @brief Private bounded owning staging result; no input bytes or template rule pack enter its type. */
struct PssStaging {
  CompactSheet<MAX_CT_PSS_RULES, MAX_CT_PSS_CLASSES, MAX_CT_PSS_DECLARATIONS, MAX_CT_PSS_TEXT_BYTES> sheet;
  ///< One temporary owning arena used only during immediate evaluation.
  PssError error; ///< First admission error.
  bool valid{};   ///< Complete parse succeeded.
};

/** @param bytes Original input with a preserved extent.
 * @param sourceName Caller-supplied identity copied into the staging arena.
 * @return One grammar pass plus a bounded owning staging result, later copied to exact numeric capacities.
 */
constexpr PssStaging stagePss(std::string_view bytes, std::string_view sourceName) {
  PssStaging result;
  auto name = validateCtPssName(sourceName);

  if (!name) {
    result.error = name.error();

    return result;
  }

  PssCompactSink sink(bytes, result.sheet);
  auto copied = sink.sourceName(sourceName);

  if (!copied) {
    result.error = copied.error();

    return result;
  }

  auto parsed = PssParser(bytes, sink, PssPolicy::STRICT_UTF8, MAX_CT_PSS_BYTES, MAX_CT_PSS_RULES).parse();

  if (!parsed) {
    result.error = parsed.error();

    return result;
  }

  result.valid = true;

  return result;
}

/** @brief Captureless provider retaining an explicit byte length.
 * @tparam T Empty default-constructible callable returning exactly string_view.
 * Raw character pointers are deliberately excluded because they lose embedded NUL suffixes.
 */
template <class T>
concept PssSourceProvider = std::is_empty_v<T> && std::default_initializable<T> && requires {
  { T{}() } -> std::same_as<std::string_view>;
};

/** @tparam R Numeric rule capacity.
 * @tparam C Numeric global class capacity.
 * @tparam D Numeric global occurrence capacity.
 * @tparam B Numeric shared owning-text capacity.
 * @param bytes Strict source bytes with a preserved extent.
 * @param sourceName Admitted identity copied into ownership.
 * @return Owning validated snapshot or a named immediate-evaluation rejection.
 */
template <std::size_t R, std::size_t C, std::size_t D, std::size_t B>
consteval auto buildPss(std::string_view bytes, std::string_view sourceName) {
  CompactSheet<R, C, D, B> result;
  PssCompactSink sink(bytes, result);
  auto copied = sink.sourceName(sourceName);

  if (!copied) {
    rejectPss(copied.error());
  }

  auto parsed = PssParser(bytes, sink, PssPolicy::STRICT_UTF8, MAX_CT_PSS_BYTES, MAX_CT_PSS_RULES).parse();

  if (!parsed) {
    rejectPss(parsed.error());
  }

  return result;
}
} // namespace detail

/** @brief Preserve the complete extent of a literal for rich compile-time diagnostics.
 * @tparam N Literal array extent including the final terminator.
 * @param bytes Constant array; embedded NUL bytes remain part of the returned extent.
 * @return Borrowed explicit-length bytes; a provider must return static literal storage.
 */
template <std::size_t N> constexpr std::string_view text(const char (&bytes)[N]) {
  return {bytes, N - 1};
}

/** @brief Convenient strict PSS literal snapshot, with bounded numeric public capacities.
 * @tparam N Literal byte extent including its final terminator.
 * @param bytes Constant array, at most MAX_CT_PSS_BYTES excluding its final terminator.
 * @param sourceName Constant UTF-8 identity copied into the shared arena, or empty for anonymous input.
 * @return Immutable owning scalar snapshot; materialization and scene installation remain runtime.
 * Failures name a category and compiler invocation location. Use the provider overload for precise PSS coordinates.
 */
template <std::size_t N> consteval auto pss(const char (&bytes)[N], std::string_view sourceName = std::string_view{}) {
  constexpr auto INPUT_BYTES = N - 1;
  constexpr auto RULE_CAPACITY = std::min(INPUT_BYTES / 3, MAX_CT_PSS_RULES);
  constexpr auto CLASS_CAPACITY = std::min(INPUT_BYTES / 2, MAX_CT_PSS_CLASSES);
  constexpr auto DECLARATION_CAPACITY = std::min(INPUT_BYTES / 10, MAX_CT_PSS_DECLARATIONS);
  constexpr auto BYTE_CAPACITY = std::min(INPUT_BYTES + MAX_CT_PSS_SOURCE_NAME_BYTES, MAX_CT_PSS_TEXT_BYTES);
  auto validName = detail::validateCtPssName(sourceName);

  if (!validName) {
    detail::rejectPss(validName.error());
  }

  return detail::buildPss<RULE_CAPACITY, CLASS_CAPACITY, DECLARATION_CAPACITY, BYTE_CAPACITY>({bytes, INPUT_BYTES},
                                                                                              sourceName);
}

/** @brief Strict PSS provider with exact storage capacities and bounded coordinate diagnostics.
 * @tparam Source Captureless default-constructible constant callable returning exactly string_view.
 * @tparam Origin Same provider contract for a UTF-8 source identity; defaults to anonymous.
 * @param source Type-only provider; its constant bytes retain their explicit extent, including embedded NUL.
 * @param origin Type-only source-identity provider; no callback is retained.
 * @return Exact-sized owning scalar snapshot with a numeric public type.
 * Use a string_view literal suffix or text(literal); const char* providers are rejected before parsing.
 */
template <detail::PssSourceProvider Source, detail::PssSourceProvider Origin = detail::AnonymousPssSource>
consteval auto pss(Source source, Origin origin = {}) {
  (void)source;
  (void)origin;

  constexpr auto BYTES = Source{}();
  constexpr auto SOURCE_NAME = Origin{}();
  constexpr auto STAGING = detail::stagePss(BYTES, SOURCE_NAME);

  if constexpr (!STAGING.valid) {
    (void)sizeof(detail::PssFailure<STAGING.error.code, STAGING.error.location.offset, STAGING.error.location.line,
                                    STAGING.error.location.column, STAGING.error.subject>);

    return CompactSheet<0, 0, 0, 0>{};
  } else {
    CompactSheet<STAGING.sheet.rules().size(), STAGING.sheet.classes().size(), STAGING.sheet.declarations().size(),
                 STAGING.sheet.ownedTextBytes()>
      result;
    detail::PssCompactSink sink({}, result);
    auto copied = sink.copyFrom(STAGING.sheet);

    if (!copied) {
      detail::rejectPss(copied.error());
    }

    return result;
  }
}

/** @tparam R Numeric rule capacity.
 * @tparam C Numeric class capacity.
 * @tparam D Numeric declaration capacity.
 * @tparam B Numeric owning-text capacity.
 * @param description Immutable owning snapshot borrowed only during conversion.
 * @return Ordinary owning StyleSheet; allocations may throw bad_alloc, including checked container copies.
 * This deliberately discards provenance. Use materializeDocument() to retain all source metadata.
 */
template <std::size_t R, std::size_t C, std::size_t D, std::size_t B>
StyleSheet materialize(const CompactSheet<R, C, D, B> &description) {
  StyleSheet result;

  result.rules.reserve(description.rules().size());

  for (const auto &rule : description.rules()) {
    StyleRule target;

    target.selector.type = description.text(rule.type);
    target.selector.id = description.text(rule.id);
    target.selector.state = rule.state;
    target.declarations = rule.values;
    target.selector.classes.reserve(rule.classCount);

    for (std::size_t index = 0; index < rule.classCount; ++index) {
      target.selector.classes.emplace_back(description.text(description.classes()[rule.classBegin + index]));
    }

    result.rules.push_back(target);
  }

  return result;
}

/** @tparam R Numeric rule capacity.
 * @tparam C Numeric class capacity.
 * @tparam D Numeric declaration capacity.
 * @tparam B Numeric owning-text capacity.
 * @param description Immutable owning snapshot borrowed only during conversion.
 * @return Complete owning document or ALLOCATION/NO_MEMORY; all allocation and checked return transfers are caught.
 * Input storage and the descriptor may be destroyed after a successful return.
 */
template <std::size_t R, std::size_t C, std::size_t D, std::size_t B>
std::expected<PssDocument, PssError> materializeDocument(const CompactSheet<R, C, D, B> &description) {
  try {
    PssDocument result;

    result.sheet = materialize(description);
    result.originKind = description.originKind();
    result.sourceName = description.sourceName();
    result.origins.reserve(description.rules().size());
    result.declarations.reserve(description.declarations().size());

    for (const auto &rule : description.rules()) {
      PssRuleOrigin origin;

      origin.selector = detail::expandPosition(rule.selector);
      origin.declarationBegin = rule.declarationBegin;
      origin.declarationCount = rule.declarationCount;

      for (std::size_t index = 0; index < PSS_PROPERTY_COUNT; ++index) {
        origin.effectiveDeclaration[index] = rule.effectiveDeclaration[index] == detail::NO_COMPACT_DECLARATION
                                               ? NO_PSS_DECLARATION
                                               : rule.effectiveDeclaration[index];
      }

      result.origins.push_back(origin);
    }

    for (const auto &declaration : description.declarations()) {
      result.declarations.push_back(
        {declaration.ruleIndex, declaration.property, detail::expandPosition(declaration.location), declaration.value});
    }

    return result;
  } catch (const std::bad_alloc &) {
    auto error = detail::pssError(PssErrorCode::ALLOCATION, {}, "PSS materialization allocation failed");

    error.cause = TreeError::NO_MEMORY;

    return std::unexpected(error);
  }
}

namespace detail {
/** @brief Recognize the existing static owning conjunction selector types.
 * @tparam T Candidate selector type.
 */
template <class T> struct IsPssSelector : std::false_type {};

/** @tparam Type Static type identity.
 * @tparam Id Static target identity.
 * @tparam Classes Static required class identities.
 */
template <Literal Type, Literal Id, Literal... Classes>
struct IsPssSelector<Selector<Type, Id, Classes...>> : std::true_type {};

/** @brief Internal bridge constructing nongeneric scalar rules from owned static identities. */
struct FlatRuleAccess;
} // namespace detail

/** @brief Nongeneric typed scalar rule for bounded array construction.
 * Selector views refer exclusively to static template-owned identities produced by flatRule().
 * The final compactSheet() copies every identity into one owning text arena. No expression or callback survives.
 */
class FlatRule {
public:
  /** @brief Construct a universal empty rule without borrowed names. */
  constexpr FlatRule() = default;

  /** @return Static template-owned type identity, empty for universal matching. */
  constexpr std::string_view type() const noexcept {
    return type_;
  }

  /** @return Static template-owned optional target identity. */
  constexpr std::string_view id() const noexcept {
    return id_;
  }

  /** @return Static template-owned conjunction class identities. */
  constexpr std::span<const std::string_view> classes() const noexcept {
    return classes_;
  }

  /** @return Validated existing presentation state. */
  constexpr StyleState state() const noexcept {
    return state_;
  }

  /** @return Validated scalar snapshot with absent properties preserved. */
  constexpr const StyleDeclarations &values() const noexcept {
    return values_;
  }

private:
  friend struct detail::FlatRuleAccess;

  std::string_view type_;
  std::string_view id_;
  std::span<const std::string_view> classes_;
  StyleState state_{StyleState::ANY};
  StyleDeclarations values_;
};

namespace detail {
/** @brief Bounded bridge preserving static identity lifetime independently of selector object lifetime. */
struct FlatRuleAccess {
  /** @tparam S Existing owning static selector specialization.
   * @param state Validated presentation state.
   * @param values Validated evaluated scalar declarations.
   * @return Nongeneric snapshot referencing only template-static identity storage.
   */
  template <class S> static consteval FlatRule make(StyleState state, StyleDeclarations values) {
    FlatRule result;

    result.type_ = S::TYPE.view() == "*" ? std::string_view{} : S::TYPE.view();
    result.id_ = S::ID.view();
    result.classes_ = S::CLASS_NAMES;
    result.state_ = state;
    result.values_ = values;

    return result;
  }
};

/** @brief Safe parameter type lookup; a missing token never instantiates tuple_element out of range.
 * @tparam Index Compile-time parameter index.
 * @tparam Tuple Owning existing parameter-definition tuple type.
 * @tparam Present Whether the index belongs to that tuple.
 */
template <std::size_t Index, class Tuple, bool Present> struct FlatParameterType {
  using Type = void; ///< Missing-token marker, never evaluated as a scalar.
};

/** @tparam Index Admitted parameter index.
 * @tparam Tuple Owning existing parameter-definition tuple type.
 */
template <std::size_t Index, class Tuple> struct FlatParameterType<Index, Tuple, true> {
  using Type = std::remove_cvref_t<decltype(std::declval<std::tuple_element_t<Index, Tuple>>().initial)>;
  ///< Existing defaults()/evaluate() copy the parameter scalar by value.
};

/** @brief Type-only preflight keeps rejected flat expressions out of the existing evaluator.
 * @tparam E Existing scalar or expression type.
 * @tparam P Existing parameter-definition pack type.
 */
template <class E, class P> struct FlatExpressionType {
  using Type = E;                               ///< Direct scalar's exact type, with no implicit conversion.
  static constexpr bool TOKENS_KNOWN = true;    ///< Scalars contain no token identity.
  static constexpr bool MIX_TYPES_VALID = true; ///< Scalars contain no blend operands.
};

/** @tparam Name Owning static token identity.
 * @tparam P Existing definition types, each storing its exact scalar initial type.
 */
template <Literal Name, class... P> struct FlatExpressionType<Token<Name>, Parameters<P...>> {
  static constexpr auto INDEX = parameterIndex<Name>(Parameters<P...>{}); ///< Safe name lookup result.
  static constexpr bool TOKENS_KNOWN = INDEX < sizeof...(P); ///< Whether the identity belongs to this definition pack.
  static constexpr bool MIX_TYPES_VALID = true;              ///< A token has no blend operands.
  using Type = typename FlatParameterType<INDEX, std::tuple<P...>, TOKENS_KNOWN>::Type; ///< Scalar or missing marker.
};

/** @tparam Left Existing left color-expression type.
 * @tparam Right Existing right color-expression type.
 * @tparam P Existing parameter-definition pack type.
 */
template <class Left, class Right, class P> struct FlatExpressionType<Mix<Left, Right>, P> {
  using LeftType = FlatExpressionType<std::remove_cvref_t<Left>, P>;   ///< Existing by-value left operand semantics.
  using RightType = FlatExpressionType<std::remove_cvref_t<Right>, P>; ///< Existing by-value right operand semantics.
  using Type = RgbColor; ///< Accepted blends always evaluate to the existing RGB scalar.
  static constexpr bool TOKENS_KNOWN = LeftType::TOKENS_KNOWN && RightType::TOKENS_KNOWN; ///< Both identities resolved.
  static constexpr bool MIX_TYPES_VALID = LeftType::MIX_TYPES_VALID && RightType::MIX_TYPES_VALID &&
                                          std::same_as<typename LeftType::Type, RgbColor> &&
                                          std::same_as<typename RightType::Type, RgbColor>; ///< Exact RGB operands.
};

/** @tparam D Existing typed declaration specialization.
 * @tparam P Existing parameter-definition pack type.
 * @return Whether its preflight scalar exactly matches its existing destination.
 */
template <class D, class P> consteval bool flatDeclarationTypeMatches() {
  using Expression = std::remove_cvref_t<decltype(std::declval<D>().expression)>;
  using Actual = typename FlatExpressionType<Expression, P>::Type;
  constexpr bool COLOR =
    D::KIND == Property::FOREGROUND || D::KIND == Property::BACKGROUND || D::KIND == Property::FRAME_LINE_FOREGROUND ||
    D::KIND == Property::FRAME_LINE_BACKGROUND || D::KIND == Property::FRAME_TITLE_FOREGROUND ||
    D::KIND == Property::FRAME_TITLE_BACKGROUND || D::KIND == Property::FRAME_ENDS_FOREGROUND ||
    D::KIND == Property::FRAME_ENDS_BACKGROUND || D::KIND == Property::SCROLLBAR_TRACK_FOREGROUND ||
    D::KIND == Property::SCROLLBAR_TRACK_BACKGROUND || D::KIND == Property::SCROLLBAR_THUMB_FOREGROUND ||
    D::KIND == Property::SCROLLBAR_THUMB_BACKGROUND || D::KIND == Property::SCROLLBAR_ENDS_FOREGROUND ||
    D::KIND == Property::SCROLLBAR_ENDS_BACKGROUND || D::KIND == Property::SCROLLBAR_CORNER_FOREGROUND ||
    D::KIND == Property::SCROLLBAR_CORNER_BACKGROUND;
  constexpr bool METRIC = D::KIND == Property::MIN_WIDTH;
  using Expected = std::conditional_t<COLOR, RgbColor, std::conditional_t<METRIC, std::int32_t, bool>>;

  return std::same_as<Actual, Expected>;
}

/** @tparam S Existing static selector type.
 * @return Whether all static byte identities belong to the shared v0 grammar.
 */
template <class S> consteval bool validFlatSelector() {
  if (!S::TYPE.view().empty() && S::TYPE.view() != "*" && !portals::detail::isPssIdentifier(S::TYPE.view())) {
    return false;
  }

  if (!S::ID.view().empty() && !portals::detail::isPssIdentifier(S::ID.view())) {
    return false;
  }

  for (const auto name : S::CLASS_NAMES) {
    if (!portals::detail::isPssIdentifier(name)) {
      return false;
    }
  }

  return true;
}

/** @param property Property inspected for presence.
 * @param value Evaluated typed rule scalars.
 * @return Whether the property is explicitly present, including false.
 */
constexpr bool pssPropertyPresent(PssProperty property, const StyleDeclarations &value) {
  switch (property) {
  case PssProperty::FOREGROUND:
    return value.foreground.has_value();
  case PssProperty::BACKGROUND:
    return value.background.has_value();
  case PssProperty::BOLD:
    return value.bold.has_value();
  case PssProperty::ITALIC:
    return value.italic.has_value();
  case PssProperty::UNDERLINE:
    return value.underline.has_value();
  case PssProperty::FRAME_LINE_FOREGROUND:
    return value.frameLine.foreground.has_value();
  case PssProperty::FRAME_LINE_BACKGROUND:
    return value.frameLine.background.has_value();
  case PssProperty::FRAME_LINE_BOLD:
    return value.frameLine.bold.has_value();
  case PssProperty::FRAME_LINE_ITALIC:
    return value.frameLine.italic.has_value();
  case PssProperty::FRAME_LINE_UNDERLINE:
    return value.frameLine.underline.has_value();
  case PssProperty::FRAME_TITLE_FOREGROUND:
    return value.frameTitle.foreground.has_value();
  case PssProperty::FRAME_TITLE_BACKGROUND:
    return value.frameTitle.background.has_value();
  case PssProperty::FRAME_TITLE_BOLD:
    return value.frameTitle.bold.has_value();
  case PssProperty::FRAME_TITLE_ITALIC:
    return value.frameTitle.italic.has_value();
  case PssProperty::FRAME_TITLE_UNDERLINE:
    return value.frameTitle.underline.has_value();
  case PssProperty::FRAME_ENDS_FOREGROUND:
    return value.frameEnds.foreground.has_value();
  case PssProperty::FRAME_ENDS_BACKGROUND:
    return value.frameEnds.background.has_value();
  case PssProperty::FRAME_ENDS_BOLD:
    return value.frameEnds.bold.has_value();
  case PssProperty::FRAME_ENDS_ITALIC:
    return value.frameEnds.italic.has_value();
  case PssProperty::FRAME_ENDS_UNDERLINE:
    return value.frameEnds.underline.has_value();
  case PssProperty::SCROLLBAR_TRACK_FOREGROUND:
    return value.scrollbarTrack.foreground.has_value();
  case PssProperty::SCROLLBAR_TRACK_BACKGROUND:
    return value.scrollbarTrack.background.has_value();
  case PssProperty::SCROLLBAR_TRACK_BOLD:
    return value.scrollbarTrack.bold.has_value();
  case PssProperty::SCROLLBAR_TRACK_ITALIC:
    return value.scrollbarTrack.italic.has_value();
  case PssProperty::SCROLLBAR_TRACK_UNDERLINE:
    return value.scrollbarTrack.underline.has_value();
  case PssProperty::SCROLLBAR_THUMB_FOREGROUND:
    return value.scrollbarThumb.foreground.has_value();
  case PssProperty::SCROLLBAR_THUMB_BACKGROUND:
    return value.scrollbarThumb.background.has_value();
  case PssProperty::SCROLLBAR_THUMB_BOLD:
    return value.scrollbarThumb.bold.has_value();
  case PssProperty::SCROLLBAR_THUMB_ITALIC:
    return value.scrollbarThumb.italic.has_value();
  case PssProperty::SCROLLBAR_THUMB_UNDERLINE:
    return value.scrollbarThumb.underline.has_value();
  case PssProperty::SCROLLBAR_ENDS_FOREGROUND:
    return value.scrollbarEnds.foreground.has_value();
  case PssProperty::SCROLLBAR_ENDS_BACKGROUND:
    return value.scrollbarEnds.background.has_value();
  case PssProperty::SCROLLBAR_ENDS_BOLD:
    return value.scrollbarEnds.bold.has_value();
  case PssProperty::SCROLLBAR_ENDS_ITALIC:
    return value.scrollbarEnds.italic.has_value();
  case PssProperty::SCROLLBAR_ENDS_UNDERLINE:
    return value.scrollbarEnds.underline.has_value();
  case PssProperty::SCROLLBAR_CORNER_FOREGROUND:
    return value.scrollbarCorner.foreground.has_value();
  case PssProperty::SCROLLBAR_CORNER_BACKGROUND:
    return value.scrollbarCorner.background.has_value();
  case PssProperty::SCROLLBAR_CORNER_BOLD:
    return value.scrollbarCorner.bold.has_value();
  case PssProperty::SCROLLBAR_CORNER_ITALIC:
    return value.scrollbarCorner.italic.has_value();
  case PssProperty::SCROLLBAR_CORNER_UNDERLINE:
    return value.scrollbarCorner.underline.has_value();
  case PssProperty::MIN_WIDTH:
    return value.minWidth.has_value();
  }

  return false;
}

/** @param property Present property selected by the caller.
 * @param value Validated evaluated typed scalars containing that property.
 * @return Small parsed-scalar payload sharing the same destination assignment path.
 */
constexpr PssValue pssScalar(PssProperty property, const StyleDeclarations &value) {
  PssValue result;

  switch (property) {
  case PssProperty::FOREGROUND:
    result.color = *value.foreground;

    break;
  case PssProperty::BACKGROUND:
    result.color = *value.background;

    break;
  case PssProperty::BOLD:
    result.boolean = *value.bold;

    break;
  case PssProperty::ITALIC:
    result.boolean = *value.italic;

    break;
  case PssProperty::UNDERLINE:
    result.boolean = *value.underline;

    break;
  case PssProperty::FRAME_LINE_FOREGROUND:
    result.color = *value.frameLine.foreground;

    break;
  case PssProperty::FRAME_LINE_BACKGROUND:
    result.color = *value.frameLine.background;

    break;
  case PssProperty::FRAME_LINE_BOLD:
    result.boolean = *value.frameLine.bold;

    break;
  case PssProperty::FRAME_LINE_ITALIC:
    result.boolean = *value.frameLine.italic;

    break;
  case PssProperty::FRAME_LINE_UNDERLINE:
    result.boolean = *value.frameLine.underline;

    break;
  case PssProperty::FRAME_TITLE_FOREGROUND:
    result.color = *value.frameTitle.foreground;

    break;
  case PssProperty::FRAME_TITLE_BACKGROUND:
    result.color = *value.frameTitle.background;

    break;
  case PssProperty::FRAME_TITLE_BOLD:
    result.boolean = *value.frameTitle.bold;

    break;
  case PssProperty::FRAME_TITLE_ITALIC:
    result.boolean = *value.frameTitle.italic;

    break;
  case PssProperty::FRAME_TITLE_UNDERLINE:
    result.boolean = *value.frameTitle.underline;

    break;
  case PssProperty::FRAME_ENDS_FOREGROUND:
    result.color = *value.frameEnds.foreground;

    break;
  case PssProperty::FRAME_ENDS_BACKGROUND:
    result.color = *value.frameEnds.background;

    break;
  case PssProperty::FRAME_ENDS_BOLD:
    result.boolean = *value.frameEnds.bold;

    break;
  case PssProperty::FRAME_ENDS_ITALIC:
    result.boolean = *value.frameEnds.italic;

    break;
  case PssProperty::FRAME_ENDS_UNDERLINE:
    result.boolean = *value.frameEnds.underline;

    break;
  case PssProperty::SCROLLBAR_TRACK_FOREGROUND:
    result.color = *value.scrollbarTrack.foreground;

    break;
  case PssProperty::SCROLLBAR_TRACK_BACKGROUND:
    result.color = *value.scrollbarTrack.background;

    break;
  case PssProperty::SCROLLBAR_TRACK_BOLD:
    result.boolean = *value.scrollbarTrack.bold;

    break;
  case PssProperty::SCROLLBAR_TRACK_ITALIC:
    result.boolean = *value.scrollbarTrack.italic;

    break;
  case PssProperty::SCROLLBAR_TRACK_UNDERLINE:
    result.boolean = *value.scrollbarTrack.underline;

    break;
  case PssProperty::SCROLLBAR_THUMB_FOREGROUND:
    result.color = *value.scrollbarThumb.foreground;

    break;
  case PssProperty::SCROLLBAR_THUMB_BACKGROUND:
    result.color = *value.scrollbarThumb.background;

    break;
  case PssProperty::SCROLLBAR_THUMB_BOLD:
    result.boolean = *value.scrollbarThumb.bold;

    break;
  case PssProperty::SCROLLBAR_THUMB_ITALIC:
    result.boolean = *value.scrollbarThumb.italic;

    break;
  case PssProperty::SCROLLBAR_THUMB_UNDERLINE:
    result.boolean = *value.scrollbarThumb.underline;

    break;
  case PssProperty::SCROLLBAR_ENDS_FOREGROUND:
    result.color = *value.scrollbarEnds.foreground;

    break;
  case PssProperty::SCROLLBAR_ENDS_BACKGROUND:
    result.color = *value.scrollbarEnds.background;

    break;
  case PssProperty::SCROLLBAR_ENDS_BOLD:
    result.boolean = *value.scrollbarEnds.bold;

    break;
  case PssProperty::SCROLLBAR_ENDS_ITALIC:
    result.boolean = *value.scrollbarEnds.italic;

    break;
  case PssProperty::SCROLLBAR_ENDS_UNDERLINE:
    result.boolean = *value.scrollbarEnds.underline;

    break;
  case PssProperty::SCROLLBAR_CORNER_FOREGROUND:
    result.color = *value.scrollbarCorner.foreground;

    break;
  case PssProperty::SCROLLBAR_CORNER_BACKGROUND:
    result.color = *value.scrollbarCorner.background;

    break;
  case PssProperty::SCROLLBAR_CORNER_BOLD:
    result.boolean = *value.scrollbarCorner.bold;

    break;
  case PssProperty::SCROLLBAR_CORNER_ITALIC:
    result.boolean = *value.scrollbarCorner.italic;

    break;
  case PssProperty::SCROLLBAR_CORNER_UNDERLINE:
    result.boolean = *value.scrollbarCorner.underline;

    break;
  case PssProperty::MIN_WIDTH:
    result.width = *value.minWidth;

    break;
  }

  return result;
}
} // namespace detail

/** @brief Evaluate one typed rule into nongeneric scalar data before array construction.
 * @tparam P Existing scalar parameter-definition types; identities must be unique.
 * @tparam S Existing static selector type, including the universal compound selector API.
 * @tparam D Existing typed declarations, at most one per destination with exact evaluated types.
 * @param definitions Owned parameter definitions borrowed only during immediate default evaluation.
 * @param selector Owning static selector; only its template-static name storage is observed by the returned rule.
 * @param declarations Owning expressions evaluated once from parameter defaults, then discarded.
 * @return Nongeneric immutable scalar snapshot; it does not participate in LiveTheme updates.
 * Token names, exact scalar types, selector grammar, state, duplicate properties, and width are checked here.
 */
template <class... P, class S, class... D>
  requires(detail::IsPssSelector<S>::value && (detail::IsDeclaration<D>::value && ...))
consteval FlatRule flatRule(detail::Parameters<P...> definitions, S selector, D... declarations) {
  static_assert(detail::uniqueParameters(detail::Parameters<P...>{}),
                "Flat style parameter names must be unique and nonempty");
  static_assert(detail::validFlatSelector<S>(), "Flat style selector requires PSS ASCII identifiers");
  static_assert(sizeof...(D) <= PSS_PROPERTY_COUNT, "Flat style rule exceeds the v0 property count");

  using Definitions = detail::Parameters<P...>;
  constexpr bool TOKENS_KNOWN = (detail::FlatExpressionType<std::remove_cvref_t<decltype(std::declval<D>().expression)>,
                                                            Definitions>::TOKENS_KNOWN &&
                                 ...);
  constexpr bool MIX_TYPES_VALID =
    (detail::FlatExpressionType<std::remove_cvref_t<decltype(std::declval<D>().expression)>,
                                Definitions>::MIX_TYPES_VALID &&
     ...);
  constexpr bool TYPES_MATCH = (detail::flatDeclarationTypeMatches<D, Definitions>() && ...);

  // Rejected expressions must never instantiate evaluate(), std::apply(), or a second independent value error.
  if constexpr (!TOKENS_KNOWN) {
    static_assert(TOKENS_KNOWN, "Static style token requires a known parameter");

    return {};
  } else if constexpr (!MIX_TYPES_VALID) {
    static_assert(MIX_TYPES_VALID, "Static style mix requires color expressions");

    return {};
  } else if constexpr (!TYPES_MATCH) {
    static_assert(TYPES_MATCH, "Static style declaration value type mismatch");

    return {};
  } else {
    const detail::Rule<S, D...> rule{selector, {declarations...}};

    detail::validateRule(rule, definitions);

    return detail::FlatRuleAccess::make<S>(selector.state,
                                           detail::declarations(rule, definitions, definitions.defaults()));
  }
}

/** @tparam S Existing owning static selector type.
 * @tparam D Existing typed scalar declarations or constant expressions requiring no parameters.
 * @param selector Owning static selector copied through its static identity storage.
 * @param declarations Constant expressions evaluated immediately; unknown tokens are rejected.
 * @return Nongeneric scalar snapshot for the array compactSheet() API.
 */
template <class S, class... D>
  requires(detail::IsPssSelector<S>::value && (detail::IsDeclaration<D>::value && ...))
consteval FlatRule flatRule(S selector, D... declarations) {
  return flatRule(parameters(), selector, declarations...);
}

/** @brief Construct a bounded owning stylesheet from nongeneric typed rules.
 * @tparam N Numeric array extent, at most MAX_CT_PSS_RULES; rule types do not enter the public result type.
 * @param rules Scalar snapshots; names reference static storage and are copied before returning.
 * @param sourceName Constant UTF-8 identity copied into the shared arena, or anonymous when empty.
 * @return Owning compact snapshot with synthetic typed ordinals, explicitly distinguished from PSS byte positions.
 * Rule offset/line identify the zero/one-based rule ordinal; declaration offset/column identify property ordinals.
 * No C++ file/line provenance is claimed. Live parameter updates remain the existing CT-002 API.
 */
template <std::size_t N>
consteval auto compactSheet(const std::array<FlatRule, N> &rules, std::string_view sourceName = std::string_view{}) {
  static_assert(N <= MAX_CT_PSS_RULES, "Compact style sheet exceeds the CT rule limit");
  constexpr auto RULE_CAPACITY = std::min(N, MAX_CT_PSS_RULES);
  constexpr auto DECLARATION_CAPACITY = std::min(N * PSS_PROPERTY_COUNT, MAX_CT_PSS_DECLARATIONS);
  auto validName = detail::validateCtPssName(sourceName);

  if (!validName) {
    detail::rejectPss(validName.error());
  }

  CompactSheet<RULE_CAPACITY, MAX_CT_PSS_CLASSES, DECLARATION_CAPACITY, MAX_CT_PSS_TEXT_BYTES> result;
  detail::PssCompactSink sink({}, result);

  sink.originKind(PssOriginKind::SYNTHETIC_TYPED);

  const auto admit = [](std::expected<void, PssError> admitted) {
    if (!admitted) {
      detail::rejectPss(admitted.error());
    }
  };

  admit(sink.sourceName(sourceName));

  for (std::size_t index = 0; index < rules.size(); ++index) {
    const auto &rule = rules[index];
    const PssLocation location{index, index + 1, 1};

    admit(sink.beginRule(location));
    admit(sink.typeName(rule.type(), location));
    admit(sink.idName(rule.id(), location));
    admit(sink.state(rule.state()));

    for (const auto name : rule.classes()) {
      admit(sink.classBytes(name, location));
    }

    for (std::size_t propertyIndex = 0; propertyIndex < PSS_PROPERTY_COUNT; ++propertyIndex) {
      const auto property = static_cast<PssProperty>(propertyIndex);

      if (detail::pssPropertyPresent(property, rule.values())) {
        admit(sink.declaration(property, detail::pssScalar(property, rule.values()),
                               {propertyIndex, index + 1, propertyIndex + 1}));
      }
    }

    admit(sink.endRule());
  }

  return result;
}
} // namespace portals::style::ct
