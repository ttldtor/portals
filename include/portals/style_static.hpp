// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_static.hpp>

/// @brief Constant-built owning rules expanded into the existing runtime style cascade.
namespace portals::style::ct {
namespace detail {
using ui::ct::Literal;

// Named non-constexpr boundaries make rejected constant inputs diagnosable on both toolchains.
/** @brief Reject out-of-range constant RGB components. */
void staticStyleInvalidRgb();
/** @brief Reject a constant mix factor outside zero to one hundred percent. */
void staticStyleInvalidMix();
/** @brief Reject an unknown constant style state. */
void staticStyleInvalidState();
/** @brief Reject a negative or state-dependent constant minimum width. */
void staticStyleInvalidMetric();

/** @brief Scalar types with nonthrowing ownership and equality.
 * @tparam T Exact parameter value type.
 */
template <class T>
concept ParameterValue = std::same_as<T, RgbColor> || std::same_as<T, bool> || std::same_as<T, std::int32_t>;

/** @brief Internal owning constant parameter definition.
 * @tparam Name Owned parameter identity.
 * @tparam T Exact scalar type.
 */
template <Literal Name, ParameterValue T> struct Parameter {
  static constexpr auto NAME = Name; ///< Owning static identity.
  T initial;                         ///< Constant default value.
};

/** @brief Internal owning parameter pack.
 * @tparam P Constant definition types in declaration order.
 */
template <class... P> struct Parameters {
  std::tuple<P...> entries; ///< Owning definitions.

  /** @return Scalar defaults in declaration order. */
  constexpr auto defaults() const {
    return std::apply(
      [](const auto &...entry) {
        return std::tuple{entry.initial...};
      },
      entries);
  }
};

/** @brief Internal named expression leaf.
 * @tparam Name Owned identity resolved by the containing sheet.
 */
template <Literal Name> struct Token {
  static constexpr auto NAME = Name; ///< Owning static identity.
};

/** @brief Internal acyclic color blend expression.
 * @tparam Left Owned left expression type.
 * @tparam Right Owned right expression type.
 */
template <class Left, class Right> struct Mix {
  Left left;               ///< Owned expression selected at zero percent.
  Right right;             ///< Owned expression selected at one hundred percent.
  std::int32_t percentage; ///< Validated constant blend factor.
};

/** @tparam Name Identity to find.
 * @tparam P Definition types containing static identities.
 * @return Definition index, or the pack size when absent.
 */
template <Literal Name, class... P> consteval std::size_t parameterIndex(Parameters<P...> const &) {
  constexpr std::array NAMES{P::NAME.view()...};

  for (std::size_t index = 0; index < NAMES.size(); ++index) {
    if (NAMES[index] == Name.view()) {
      return index;
    }
  }

  return sizeof...(P);
}

/** @tparam Name Missing identity in an empty parameter pack.
 * @return Zero, the empty pack size.
 */
template <Literal Name> consteval std::size_t parameterIndex(Parameters<> const &) {
  return 0;
}

/** @tparam T Scalar expression type.
 * @tparam P Parameter definition pack type.
 * @tparam V Current value tuple type.
 * @param value Scalar expression copied unchanged.
 * @return Owning scalar value.
 */
template <class T, class P, class V> constexpr T evaluate(T value, const P &, const V &) {
  return value;
}

/** @tparam Name Constant token identity.
 * @tparam P Definition types used to resolve the identity.
 * @tparam V Current value tuple type.
 * @param values Current scalars in definition order.
 * @return Scalar value at the checked identity.
 */
template <Literal Name, class... P, class V>
constexpr auto evaluate(Token<Name>, const Parameters<P...> &, const V &values) {
  constexpr auto INDEX = parameterIndex<Name>(Parameters<P...>{});
  static_assert(INDEX < sizeof...(P), "Static style token requires a known parameter");

  if constexpr (INDEX < sizeof...(P)) {
    return std::get<INDEX>(values);
  }
}

/** @tparam Left Left expression type.
 * @tparam Right Right expression type.
 * @tparam P Parameter definition pack type.
 * @tparam V Current value tuple type.
 * @param expression Owning blend expression.
 * @param parameters Definitions for token resolution.
 * @param values Current scalar tuple.
 * @return Integral blend rounded down per component.
 */
template <class Left, class Right, class P, class V>
constexpr RgbColor evaluate(Mix<Left, Right> expression, const P &parameters, const V &values) {
  auto left = evaluate(expression.left, parameters, values);
  auto right = evaluate(expression.right, parameters, values);
  static_assert(std::same_as<decltype(left), RgbColor> && std::same_as<decltype(right), RgbColor>,
                "Static style mix requires color expressions");

  if constexpr (std::same_as<decltype(left), RgbColor> && std::same_as<decltype(right), RgbColor>) {
    const auto component = [&](std::uint8_t a, std::uint8_t b) {
      constexpr std::int32_t FULL_PERCENTAGE = 100;

      return static_cast<std::uint8_t>((a * (FULL_PERCENTAGE - expression.percentage) + b * expression.percentage) /
                                       FULL_PERCENTAGE);
    };

    return {component(left.red, right.red), component(left.green, right.green), component(left.blue, right.blue)};
  } else {
    return {};
  }
}

/** @brief Internal declaration destinations, exactly matching StyleDeclarations. */
enum class Property {
  FOREGROUND, ///< Text color.
  BACKGROUND, ///< Fill color.
  BOLD,       ///< Bold paint flag.
  ITALIC,     ///< Italic paint flag.
  UNDERLINE,  ///< Underline paint flag.
  MIN_WIDTH   ///< State-independent minimum cell width.
};

/** @brief Internal typed owning declaration.
 * @tparam Kind Existing property destination.
 * @tparam Expression Owned value or expression type.
 */
template <Property Kind, class Expression> struct Declaration {
  static constexpr auto KIND = Kind; ///< Destination identity.
  Expression expression;             ///< Owned value expression.
};

/** @brief Internal conjunction selector with static owning byte identities.
 * @tparam Type Optional widget type.
 * @tparam Id Optional widget identity.
 * @tparam Classes Required class identities.
 */
template <Literal Type, Literal Id, Literal... Classes> struct Selector {
  static constexpr auto TYPE = Type;                          ///< Owning type identity.
  static constexpr auto ID = Id;                              ///< Owning target identity.
  static constexpr std::array CLASS_NAMES{Classes.view()...}; ///< Views into static owned template bytes.
  StyleState state;                                           ///< Existing pseudo-state.
};

/** @brief Internal selector specialization with no class predicates.
 * @tparam Type Optional widget type.
 * @tparam Id Optional widget identity.
 */
template <Literal Type, Literal Id> struct Selector<Type, Id> {
  static constexpr auto TYPE = Type;                              ///< Owning type identity.
  static constexpr auto ID = Id;                                  ///< Owning target identity.
  static constexpr std::array<std::string_view, 0> CLASS_NAMES{}; ///< Empty class predicates.
  StyleState state;                                               ///< Existing pseudo-state.
};

/** @brief Internal owning rule, validated by its containing sheet.
 * @tparam S Static selector type.
 * @tparam D Typed declaration pack.
 */
template <class S, class... D> struct Rule {
  S selector;                    ///< Owning selector.
  std::tuple<D...> declarations; ///< Owning declarations.
};

/** @brief Recognize internal rule types.
 * @tparam T Candidate type.
 */
template <class T> struct IsRule : std::false_type {};

/** @brief Admit owning rule types.
 * @tparam S Selector type.
 * @tparam D Declaration types.
 */
template <class S, class... D> struct IsRule<Rule<S, D...>> : std::true_type {};

/** @brief Recognize internal declaration types.
 * @tparam T Candidate type.
 */
template <class T> struct IsDeclaration : std::false_type {};

/** @brief Admit typed declarations.
 * @tparam K Destination identity.
 * @tparam E Expression type.
 */
template <Property K, class E> struct IsDeclaration<Declaration<K, E>> : std::true_type {};

/** @brief Recognize internal parameter types.
 * @tparam T Candidate type.
 */
template <class T> struct IsParameter : std::false_type {};

/** @brief Admit owning scalar parameters.
 * @tparam N Parameter identity.
 * @tparam V Scalar value type.
 */
template <Literal N, class V> struct IsParameter<Parameter<N, V>> : std::true_type {};

/** @tparam P Parameter definition types.
 * @return True for nonempty unique identities.
 */
template <class... P> consteval bool uniqueParameters(Parameters<P...> const &) {
  constexpr std::array<std::string_view, sizeof...(P)> NAMES{P::NAME.view()...};

  for (std::size_t index = 0; index < NAMES.size(); ++index) {
    if (NAMES[index].empty()) {
      return false;
    }

    for (std::size_t other = index + 1; other < NAMES.size(); ++other) {
      if (NAMES[index] == NAMES[other]) {
        return false;
      }
    }
  }

  return true;
}

/** @tparam D Declaration types.
 * @return True when no destination repeats in the rule.
 */
template <class... D> consteval bool uniqueDeclarations(const std::tuple<D...> &) {
  constexpr std::array<Property, sizeof...(D)> KINDS{D::KIND...};

  for (std::size_t index = 0; index < KINDS.size(); ++index) {
    for (std::size_t other = index + 1; other < KINDS.size(); ++other) {
      if (KINDS[index] == KINDS[other]) {
        return false;
      }
    }
  }

  return true;
}

/** @tparam D Typed declaration type.
 * @tparam P Parameter definition pack type.
 * @tparam V Current scalar tuple type.
 * @param declaration Expression and checked destination.
 * @param destination Existing runtime declaration storage.
 * @param parameters Definitions for token resolution.
 * @param values Current scalar tuple.
 */
template <class D, class P, class V>
constexpr void assign(const D &declaration, StyleDeclarations &destination, const P &parameters, const V &values) {
  auto value = evaluate(declaration.expression, parameters, values);
  constexpr bool COLOR = D::KIND == Property::FOREGROUND || D::KIND == Property::BACKGROUND;
  constexpr bool METRIC = D::KIND == Property::MIN_WIDTH;
  using Expected = std::conditional_t<COLOR, RgbColor, std::conditional_t<METRIC, std::int32_t, bool>>;
  static_assert(std::same_as<decltype(value), Expected>, "Static style declaration value type mismatch");

  if constexpr (std::same_as<decltype(value), Expected>) {
    if constexpr (D::KIND == Property::FOREGROUND) {
      destination.foreground = value;
    } else if constexpr (D::KIND == Property::BACKGROUND) {
      destination.background = value;
    } else if constexpr (D::KIND == Property::BOLD) {
      destination.bold = value;
    } else if constexpr (D::KIND == Property::ITALIC) {
      destination.italic = value;
    } else if constexpr (D::KIND == Property::UNDERLINE) {
      destination.underline = value;
    } else if constexpr (METRIC) {
      destination.minWidth = value;
    }
  }
}

/** @tparam R Owning rule type.
 * @tparam P Parameter definition pack type.
 * @tparam V Current scalar tuple type.
 * @param rule Constant source rule.
 * @param parameters Definitions for token resolution.
 * @param values Current scalar tuple.
 * @return Existing declarations evaluated from the owned expressions.
 */
template <class R, class P, class V>
constexpr StyleDeclarations declarations(const R &rule, const P &parameters, const V &values) {
  StyleDeclarations result{};
  std::apply(
    [&](const auto &...declaration) {
      (assign(declaration, result, parameters, values), ...);
    },
    rule.declarations);

  return result;
}

/** @tparam R Owning rule type.
 * @tparam P Parameter definition pack type.
 * @param rule Constant rule to validate.
 * @param parameters Definitions and defaults used for constant evaluation.
 */
template <class R, class P> consteval void validateRule(const R &rule, const P &parameters) {
  if (rule.selector.state < StyleState::ANY || rule.selector.state > StyleState::CARET) {
    staticStyleInvalidState();
  }

  static_assert(uniqueDeclarations(decltype(rule.declarations){}), "Static style rule repeats a declaration");
  const auto values = declarations(rule, parameters, parameters.defaults());

  if (values.minWidth && (*values.minWidth < 0 || rule.selector.state != StyleState::ANY)) {
    staticStyleInvalidMetric();
  }
}

/** @brief Immutable owning parameter/rule description returned by sheet().
 * @tparam P Constant parameter-set type.
 * @tparam R Constant source-order rule pack.
 */
template <class P, class... R> class Sheet {
public:
  /** @param parameters Owning definitions with constant default values.
   * @param rules Owning rules validated before the constant value is returned.
   */
  consteval Sheet(P parameters, R... rules) : parameters_(parameters), rules_(rules...) {
    static_assert(sizeof...(R) <= MAX_STYLE_RULES, "Static style sheet exceeds the rule limit");
    static_assert(uniqueParameters(P{}), "Static style parameter names must be nonempty and unique");
    (validateRule(rules, parameters), ...);
  }

  /** @return Borrowed immutable definitions; their bytes and defaults belong to this value or its type. */
  constexpr const P &parameters() const noexcept {
    return parameters_;
  }

  /** @return Borrowed immutable rules in original source order; their fields support constant inspection. */
  constexpr const std::tuple<R...> &rules() const noexcept {
    return rules_;
  }

private:
  P parameters_;
  std::tuple<R...> rules_;
};

/** @brief Recognize validated owning sheet types.
 * @tparam T Candidate type.
 */
template <class T> struct IsSheet : std::false_type {};

/** @brief Preserve recognition for constant sheet objects.
 * @tparam T Unqualified candidate type.
 */
template <class T> struct IsSheet<const T> : IsSheet<T> {};

/** @brief Admit validated constant sheets.
 * @tparam P Parameter-set type.
 * @tparam R Constant rule pack.
 */
template <class P, class... R> struct IsSheet<Sheet<P, R...>> : std::true_type {};

/** @brief Internal owning named update member.
 * @tparam Name Owned target identity.
 * @tparam T Candidate scalar type.
 */
template <Literal Name, class T> struct Change {
  static constexpr auto NAME = Name; ///< Owning update identity.
  T value;                           ///< Candidate scalar.
};

/** @tparam C Named update member types.
 * @return True when no identity repeats in a batch.
 */
template <class... C> consteval bool uniqueChanges() {
  constexpr std::array<std::string_view, sizeof...(C)> NAMES{C::NAME.view()...};

  for (std::size_t index = 0; index < NAMES.size(); ++index) {
    for (std::size_t other = index + 1; other < NAMES.size(); ++other) {
      if (NAMES[index] == NAMES[other]) {
        return false;
      }
    }
  }

  return true;
}

/** @brief Private bridge reusing managed lifetime, affinity and mutation boundaries. */
struct Access {
  /** @param scene Existing managed owner.
   * @return Its existing weak lifetime token; never extends scene lifetime.
   */
  static std::weak_ptr<const void> lifetime(ManagedScene &scene) {
    return scene.lifetime_;
  }

  /** @param scene Existing managed owner.
   * @return Immutable UI identity checked before observer dereference.
   */
  static std::thread::id owner(ManagedScene &scene) {
    return scene.owner_;
  }

  /** @param scene Existing owner, borrowed only after affinity and lifetime admission.
   * @return Original STYLE boundary result, including close, preparation, reporting and tree frame checks.
   */
  static std::expected<void, ManagedError> boundary(ManagedScene &scene) {
    return scene.boundary(ManagedPhase::STYLE, scene.root_);
  }
};

/** @param node Stable scene identity for diagnostics.
 * @param cause Original allocation, affinity or lifecycle failure.
 * @return Unformatted managed STYLE failure.
 */
inline ManagedError error(NodeId node, TreeError cause) {
  return {ManagedPhase::STYLE, node, cause, {}, {}};
}
} // namespace detail

/** @brief Validate and construct a constant RGB value without narrowing first.
 * @param red Red component in [0, 255].
 * @param green Green component in [0, 255].
 * @param blue Blue component in [0, 255].
 * @return Existing RGB value.
 */
consteval RgbColor rgb(std::int32_t red, std::int32_t green, std::int32_t blue) {
  constexpr std::int32_t MAX_COMPONENT = 255;

  if (red < 0 || green < 0 || blue < 0 || red > MAX_COMPONENT || green > MAX_COMPONENT || blue > MAX_COMPONENT) {
    detail::staticStyleInvalidRgb();
  }

  return {static_cast<std::uint8_t>(red), static_cast<std::uint8_t>(green), static_cast<std::uint8_t>(blue)};
}

/** @tparam Name Nonempty parameter name, owned in the type.
 * @tparam T Exactly RgbColor, bool or int32_t.
 * @param initial Constant default value; metric constraints are validated by sheet().
 * @return Owning typed parameter definition.
 */
template <detail::Literal Name, detail::ParameterValue T> consteval auto parameter(T initial) {
  return detail::Parameter<Name, T>{initial};
}

/** @tparam P Typed parameter definition pack; names must be unique.
 * @param entries Constant definitions, copied into the resulting value.
 * @return Owning parameter set; an empty set is supported.
 */
template <class... P>
  requires(detail::IsParameter<P>::value && ...)
consteval auto parameters(P... entries) {
  static_assert(detail::uniqueParameters(detail::Parameters<P...>{}),
                "Static style parameter names must be nonempty and unique");

  return detail::Parameters<P...>{{entries...}};
}

/** @tparam Name Parameter name, checked for existence and type by sheet().
 * @return Constant expression observing a parameter value by name.
 */
template <detail::Literal Name> consteval auto token() {
  return detail::Token<Name>{};
}

/** @brief Blend colors using integral components, rounding down.
 * @tparam Left Constant color or nested color expression type.
 * @tparam Right Constant color or nested color expression type.
 * @param left Color selected at zero percent.
 * @param right Color selected at one hundred percent.
 * @param percentage Constant blend factor in [0, 100].
 * @return Owning acyclic expression; token names and color types are validated by sheet().
 */
template <class Left, class Right> consteval auto mix(Left left, Right right, std::int32_t percentage) {
  constexpr std::int32_t FULL_PERCENTAGE = 100;

  if (percentage < 0 || percentage > FULL_PERCENTAGE) {
    detail::staticStyleInvalidMix();
  }

  return detail::Mix<Left, Right>{left, right, percentage};
}

/** @tparam Type Existing runtime widget type name; empty and "*" match every type.
 * @tparam Id Optional target ID; empty matches every ID.
 * @tparam Classes Required class names, all of which must match the target.
 * @param state Existing pseudo-state; metric declarations require ANY.
 * @return Owning selector description; bytes retain existing runtime matching semantics.
 */
template <detail::Literal Type = "", detail::Literal Id = "", detail::Literal... Classes>
consteval auto selector(StyleState state = StyleState::ANY) {
  if (state < StyleState::ANY || state > StyleState::CARET) {
    detail::staticStyleInvalidState();
  }

  return detail::Selector<Type, Id, Classes...>{state};
}

/** @tparam E Color or color expression type.
 * @param expression Foreground value evaluated when the sheet is materialized.
 * @return Typed declaration.
 */
template <class E> consteval auto foreground(E expression) {
  return detail::Declaration<detail::Property::FOREGROUND, E>{expression};
}

/** @tparam E Color or color expression type.
 * @param expression Background value evaluated when the sheet is materialized.
 * @return Typed declaration.
 */
template <class E> consteval auto background(E expression) {
  return detail::Declaration<detail::Property::BACKGROUND, E>{expression};
}

/** @tparam E Boolean or boolean token type.
 * @param expression Bold value evaluated when the sheet is materialized.
 * @return Typed declaration.
 */
template <class E> consteval auto bold(E expression) {
  return detail::Declaration<detail::Property::BOLD, E>{expression};
}

/** @tparam E Boolean or boolean token type.
 * @param expression Italic value evaluated when the sheet is materialized.
 * @return Typed declaration.
 */
template <class E> consteval auto italic(E expression) {
  return detail::Declaration<detail::Property::ITALIC, E>{expression};
}

/** @tparam E Boolean or boolean token type.
 * @param expression Underline value evaluated when the sheet is materialized.
 * @return Typed declaration.
 */
template <class E> consteval auto underline(E expression) {
  return detail::Declaration<detail::Property::UNDERLINE, E>{expression};
}

/** @tparam E int32_t or integer token type.
 * @param expression Minimum width evaluated when the sheet is materialized; must be nonnegative.
 * @return Typed declaration, permitted only on an ANY-state selector.
 */
template <class E> consteval auto minWidth(E expression) {
  return detail::Declaration<detail::Property::MIN_WIDTH, E>{expression};
}

/** @tparam S Constant selector type returned by selector().
 * @tparam D Constant declaration pack, with at most one declaration of each kind.
 * @param target Selector copied into the rule.
 * @param declarations Declarations copied into the rule; an empty rule is supported.
 * @return Owning rule; sheet() validates token names, types and default metrics.
 */
template <class S, class... D>
  requires(detail::IsDeclaration<D>::value && ...)
consteval auto rule(S target, D... declarations) {
  static_assert(detail::uniqueDeclarations(std::tuple<D...>{}), "Static style rule repeats a declaration");

  return detail::Rule<S, D...>{target, {declarations...}};
}

/** @tparam P Parameter-set type returned by parameters().
 * @tparam R Constant rule pack, preserving source order.
 * @param values Owning parameter definitions and defaults.
 * @param rules Owning rules; bounded by MAX_STYLE_RULES.
 * @return Validated immutable description usable in a constant expression.
 */
template <class P, class... R>
  requires(detail::IsRule<R>::value && ...)
consteval auto sheet(P values, R... rules) {
  static_assert(sizeof...(R) <= MAX_STYLE_RULES, "Static style sheet exceeds the rule limit");

  if constexpr (sizeof...(R) <= MAX_STYLE_RULES) {
    return detail::Sheet<P, R...>(values, rules...);
  }
}

/** @brief Constant counterpart of portals::themeFromTokens with live named color parameters.
 * @tparam P Optional extra parameter definitions; standard color names are reserved by uniqueness checks.
 * @tparam R Optional extra rules appended after the six existing standard-state rules.
 * @param defaults Default foreground, background, accent, muted, selection and heading colors.
 * @param extraParameters Additional owning definitions, for example a control-specific minimum width.
 * @param extraRules Additional owning rules, preserving the existing cascade's source-order behavior.
 * @return Validated owning sheet with standard token names matching ThemeTokens member names.
 */
template <class... P, class... R>
  requires(detail::IsRule<R>::value && ...)
consteval auto themeFromTokens(ThemeTokens defaults, detail::Parameters<P...> extraParameters = {}, R... extraRules) {
  static_cast<void>(extraParameters); // An empty parameter pack otherwise leaves no expression using this argument.

  return sheet(
    parameters(parameter<"foreground">(defaults.foreground), parameter<"background">(defaults.background),
               parameter<"accent">(defaults.accent), parameter<"muted">(defaults.muted),
               parameter<"selection">(defaults.selection), parameter<"heading">(defaults.heading),
               std::get<P>(extraParameters.entries)...),
    rule(selector(), foreground(token<"foreground">()), background(token<"background">())),
    rule(selector(StyleState::FOCUSED), foreground(token<"background">()), background(token<"accent">()), bold(true)),
    rule(selector(StyleState::DISABLED), foreground(token<"muted">()), background(token<"background">()), bold(false)),
    rule(selector(StyleState::SELECTED), foreground(token<"foreground">()), background(token<"selection">()),
         bold(true)),
    rule(selector(StyleState::HEADER), foreground(token<"heading">()), background(token<"background">()), bold(true)),
    rule(selector(StyleState::CARET), foreground(token<"accent">()), background(token<"background">()), bold(false)),
    extraRules...);
}

namespace detail {
/** @tparam S Validated sheet type.
 * @tparam V Current scalar tuple type.
 * @param description Owning rule structure borrowed during conversion.
 * @param values Current scalars, borrowed only for expression evaluation.
 * @return Ordinary owning stylesheet; allocation may throw.
 */
template <class S, class V> StyleSheet materializeValues(const S &description, const V &values) {
  StyleSheet result;
  result.rules.reserve(std::tuple_size_v<std::remove_cvref_t<decltype(description.rules())>>);
  std::apply(
    [&](const auto &...rule) {
      if constexpr (sizeof...(rule) > 0) {
        const auto append = [&](const auto &source) {
          using SelectorType = std::remove_cvref_t<decltype(source.selector)>;
          StyleRule target;
          target.selector.type = SelectorType::TYPE.view();
          target.selector.id = SelectorType::ID.view();
          target.selector.state = source.selector.state;
          target.selector.classes.reserve(SelectorType::CLASS_NAMES.size());

          for (const auto name : SelectorType::CLASS_NAMES) {
            target.selector.classes.emplace_back(name);
          }

          target.declarations = declarations(source, description.parameters(), values);
          result.rules.push_back(std::move(target));
        };
        (append(rule), ...);
      }
    },
    description.rules());

  return result;
}
} // namespace detail

/** @tparam S Validated constant sheet type.
 * @param description Owning description, borrowed only during this conversion.
 * @return Ordinary owning stylesheet with default parameter values; allocation may throw.
 * The existing engine remains responsible for specificity, source order and invalidation.
 */
template <class S>
  requires detail::IsSheet<S>::value
StyleSheet materialize(const S &description) {
  return detail::materializeValues(description, description.parameters().defaults());
}

/** @tparam Name Parameter name checked by LiveTheme::apply().
 * @tparam T Exact candidate value type; implicit parameter conversions are rejected.
 * @param value Owning scalar candidate value.
 * @return Update member for one atomic batch.
 */
template <detail::Literal Name, class T> auto change(T value) {
  return detail::Change<Name, T>{value};
}

/** @brief Copyable UI-affine observer with owned rules and current scalar parameter values.
 * Copies share values but never extend scene lifetime. One handle owns no subscription or callback.
 * Destruction and copies are thread-independent; operations require the scene's UI thread.
 * Only this controller's changes are tracked: external theme() replacement supersedes its sheet
 * until the next unequal parameter update. An equal update deliberately does not reinstall it.
 * @tparam S Validated owning static stylesheet type.
 */
template <class S>
  requires detail::IsSheet<S>::value
class LiveTheme {
  using Values = decltype(std::declval<S>().parameters().defaults());

  struct State {
    S description;
    Values values;
    ManagedScene *scene;
    std::weak_ptr<const void> lifetime;
    std::thread::id owner;
    NodeId root;
    bool applying{};
  };

public:
  /** @brief Construct an empty observer; operations return CLOSED. */
  LiveTheme() = default;

  /** @param scene Existing managed owner, observed through its weak lifetime and UI identity.
   * @param description Owning immutable sheet copied into this handle's shared state.
   * @return Observer after installation, or exact managed/style error with no controller published.
   * Allocations precede installation. No user callback is retained by this controller.
   */
  static std::expected<LiveTheme, ManagedError> attach(ManagedScene &scene, const S &description) {
    auto allowed = detail::Access::boundary(scene);

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    const auto root = scene.root().id();
    const auto lifetime = detail::Access::lifetime(scene);
    const auto owner = detail::Access::owner(scene);

    try {
      LiveTheme result;
      result.state_ =
        std::make_shared<State>(description, description.parameters().defaults(), &scene, lifetime, owner, root);
      auto sheet = detail::materializeValues(result.state_->description, result.state_->values);
      allowed = boundary(result.state_);

      if (!allowed) {
        return std::unexpected(allowed.error());
      }

      auto installed = result.state_->scene->theme(std::move(sheet));

      if (!installed) {
        return std::unexpected(installed.error());
      }

      return result;
    } catch (const std::bad_alloc &) {
      return std::unexpected(detail::error(root, TreeError::NO_MEMORY));
    }
  }

  /** @tparam Name Known parameter name.
   * @return Current scalar value or CLOSED, WRONG_THREAD, or the existing managed boundary error.
   */
  template <detail::Literal Name> auto get() const {
    constexpr auto INDEX = index<Name>();
    using Value = std::tuple_element_t<INDEX, Values>;
    std::expected<Value, ManagedError> result;
    const auto state = state_;
    auto allowed = boundary(state);

    if (!allowed) {
      return decltype(result)(std::unexpected(allowed.error()));
    }

    return decltype(result)(std::get<INDEX>(state->values));
  }

  /** @tparam Name Known parameter name.
   * @tparam T Exact parameter value type.
   * @param value Candidate value; negative runtime metrics are rejected by the existing style engine.
   * @return True after a committed value change, false for an equal value, or original managed/style error.
   */
  template <detail::Literal Name, class T> std::expected<bool, ManagedError> set(T value) const {
    return apply(change<Name>(value));
  }

  /** @tparam C Named update pack; names are known, unique, and values have exact parameter types.
   * @param changes Candidates committed together only after the existing engine accepts the complete sheet.
   * @return True after a committed batch, false for equal values, or original managed/style error.
   * Equal batches still check the scene boundary but allocate nothing and request no invalidation.
   * A rejected batch leaves both current values and installed widget styles unchanged.
   */
  template <class... C> std::expected<bool, ManagedError> apply(C... changes) const {
    static_assert(detail::uniqueChanges<C...>(), "Static style update repeats a parameter");
    const auto state = state_; // A hook may destroy or replace the calling handle during style staging.
    auto allowed = boundary(state);

    if (!allowed) {
      return std::unexpected(allowed.error());
    }

    auto candidate = state->values;
    (update(candidate, changes), ...);

    if (candidate == state->values) {
      return false;
    }

    struct Guard {
      State &state;

      explicit Guard(State &value) : state(value) {
        state.applying = true;
      }

      ~Guard() {
        state.applying = false;
      }
    } guard(*state);

    try {
      auto sheet = detail::materializeValues(state->description, candidate);
      allowed = boundary(state, true);

      if (!allowed) {
        return std::unexpected(allowed.error());
      }

      auto installed = state->scene->theme(std::move(sheet));

      if (!installed) {
        return std::unexpected(installed.error());
      }

      state->values = candidate;

      return true;
    } catch (const std::bad_alloc &) {
      return std::unexpected(detail::error(state->root, TreeError::NO_MEMORY));
    }
  }

private:
  template <detail::Literal Name> static consteval std::size_t index() {
    using P = std::remove_cvref_t<decltype(std::declval<S>().parameters())>;
    constexpr auto INDEX = detail::parameterIndex<Name>(P{});
    static_assert(INDEX < std::tuple_size_v<Values>, "Static style update requires a known parameter");

    return INDEX;
  }

  template <class C> static void update(Values &values, const C &change) {
    constexpr auto INDEX = index<C::NAME>();
    using Value = std::tuple_element_t<INDEX, Values>;
    static_assert(std::same_as<decltype(change.value), Value>, "Static style update value type mismatch");

    if constexpr (std::same_as<decltype(change.value), Value>) {
      std::get<INDEX>(values) = change.value;
    }
  }

  static std::expected<void, ManagedError> boundary(const std::shared_ptr<State> &state, bool completing = false) {
    if (!state) {
      return std::unexpected(detail::error({}, TreeError::CLOSED));
    }

    // UI identity is checked before the raw observer or weak token can touch the scene.
    if (state->owner != std::this_thread::get_id()) {
      return std::unexpected(detail::error(state->root, TreeError::WRONG_THREAD));
    }

    if (state->lifetime.expired()) {
      return std::unexpected(detail::error(state->root, TreeError::CLOSED));
    }

    if (state->applying && !completing) {
      return std::unexpected(detail::error(state->root, TreeError::WOULD_BLOCK));
    }

    return detail::Access::boundary(*state->scene);
  }

  std::shared_ptr<State> state_;
};

/** @tparam S Validated static stylesheet type.
 * @param scene Existing managed owner; its Application must outlive it as usual.
 * @param description Owning description copied before the existing engine installs its default values.
 * @return Copyable observer or original managed/style failure; no second cascade or retained UI tree.
 */
template <class S>
  requires detail::IsSheet<S>::value
auto live(ManagedScene &scene, const S &description) {
  return LiveTheme<S>::attach(scene, description);
}
} // namespace portals::style::ct
