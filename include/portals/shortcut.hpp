// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/input.hpp>

#include <initializer_list>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace portals {
/** @brief Bounded shortcut syntax and value-validation failures, never borrowing source text. */
enum class ShortcutErrorKind {
  INVALID_OPTIONS,      ///< A parser budget is zero.
  SOURCE_LIMIT,         ///< Source exceeds its byte budget.
  ALTERNATIVE_LIMIT,    ///< Too many alternatives.
  STROKE_LIMIT,         ///< Too many key strokes in one alternative.
  EMPTY_ALTERNATIVE,    ///< Missing alternative before or after a separator.
  EMPTY_TOKEN,          ///< Missing token around a plus or comma separator.
  UNKNOWN_KEY,          ///< Unsupported ASCII key/modifier name, including physical qualifiers.
  DUPLICATE_MODIFIER,   ///< One semantic modifier occurs twice in a stroke.
  EXPECTED_KEY,         ///< Modifiers were provided without a key.
  UNSUPPORTED_SEQUENCE, ///< Reserved legacy diagnostic; valid sequences are now supported.
  INVALID_CHORD,        ///< Invalid semantic key/scalar combination.
  NO_MEMORY,            ///< Owning alternative allocation failed.
  INVALID_ACTION_SET,   ///< Empty action set or invalid action; offset identifies the action index.
  AMBIGUOUS_PREFIX,     ///< An alternative is a proper prefix of another in this registration.
  RESERVED_CANCEL       ///< Unmodified Escape is reserved for cancelling pending input.
};

/** @brief Allocation-free diagnostic independent of source and scene lifetime. */
struct ShortcutError {
  ShortcutErrorKind cause{}; ///< Exact parser/value failure.
  std::size_t offset{};      ///< Zero-based byte offset; typed chord errors use their alternative index.
  /** @param other Original typed error to compare.
   * @return Whether both cause and offset match.
   */
  bool operator==(const ShortcutError &other) const = default;
};

/** @brief Configurable finite parser limits; every value must be positive. */
struct ShortcutOptions {
  std::size_t maxSourceBytes{4096}; ///< Maximum runtime source length in bytes.
  std::size_t maxAlternatives{32};  ///< Maximum alternatives retained by one registration.
  std::size_t maxStrokes{8};        ///< Maximum strokes in one alternative.
};

/** @brief Command admission independent of low-level KeyEvent binding behavior. */
struct ShortcutBehavior {
  bool repeat{}; ///< Explicitly admit native REPEAT events; PRESS is always admitted, RELEASE never is.
};

/** @brief One owning multi-stroke alternative, without action or held-key state. */
struct ShortcutSequence {
  /** @brief Construct empty storage; debug iterator proxy allocation may throw. */
  ShortcutSequence() : strokes(0) {
  }

  /** @param chords Ordered strokes copied for this alternative; validation requires at least two. */
  explicit ShortcutSequence(std::initializer_list<key::Chord> chords) : strokes(chords) {
  }

  /** @param other Strokes copied without borrowing caller storage. */
  ShortcutSequence(const ShortcutSequence &other) = default;

  /** @param other Strokes transferred inside the caller's checked allocation boundary. */
  ShortcutSequence(ShortcutSequence &&other) : strokes(0) {
    strokes.swap(other.strokes);
  }

  /** @param other Strokes copied into this sequence.
   * @return This value; allocation failures propagate to the checked caller.
   */
  ShortcutSequence &operator=(const ShortcutSequence &other) = default;

  /** @param other Strokes exchanged without invoking a debug-vector move.
   * @return This sequence; both values remain valid.
   */
  ShortcutSequence &operator=(ShortcutSequence &&other) noexcept {
    strokes.swap(other.strokes);

    return *this;
  }

  std::vector<key::Chord> strokes; ///< Owned ordered strokes with exact semantic modifiers.
};

/** @brief Owning shortcut alternatives; one registration invokes at most once per completed event. */
struct Shortcut {
  /** @brief Construct empty owning storage; debug-container allocation is allowed to throw. */
  Shortcut() : alternatives(0), sequences(0) {
  }

  /** @param chords Alternatives copied from storage borrowed only during construction. */
  explicit Shortcut(std::initializer_list<key::Chord> chords) : alternatives(chords), sequences(0) {
  }

  /** @param other Owning alternatives copied without retaining a source view. */
  Shortcut(const Shortcut &other) = default;

  /** @param other Owning alternatives transferred; moved-from storage remains empty.
   * This potentially throwing construction deliberately avoids a noexcept vector move:
   * some debug standard libraries allocate an iterator proxy in that move.
   */
  Shortcut(Shortcut &&other) : alternatives(0), sequences(0) {
    alternatives.swap(other.alternatives);
    sequences.swap(other.sequences);
  }

  /** @param other Alternatives copied into this value.
   * @return This owning shortcut; allocation failures propagate to the checked caller.
   */
  Shortcut &operator=(const Shortcut &other) = default;

  /** @param other Alternatives exchanged with this value; both objects remain valid.
   * @return This owning shortcut without allocating or invoking a vector move.
   */
  Shortcut &operator=(Shortcut &&other) noexcept {
    alternatives.swap(other.alternatives);
    sequences.swap(other.sequences);

    return *this;
  }

  std::vector<key::Chord> alternatives;    ///< Owned values, never views into a caller string or initializer list.
  std::vector<ShortcutSequence> sequences; ///< Owned sequences sharing the alternative budget with single chords.
};

/** @param source Borrowed for this call only; ASCII names ignore case and surrounding ASCII whitespace.
 * @param options Positive finite byte/alternative/stroke budgets.
 * @return Owned alternatives or a typed byte offset. Plus joins modifiers and keys, pipe joins alternatives.
 * Plus-separated keys share all modifiers of their segment; comma starts independent modifiers.
 * Proper-prefix alternatives and unmodified Escape after the first stroke are rejected.
 * No physical modifier identity or AltGr interpretation is inferred.
 */
std::expected<Shortcut, ShortcutError> parseShortcut(std::string_view source, ShortcutOptions options = {});

/** @param shortcut Borrowed owning values to validate before atomic registration.
 * @param options Positive finite alternative/stroke budgets shared by singles and sequences.
 * @return Success or an allocation-free typed value/index failure.
 */
std::expected<void, ShortcutError> validateShortcut(const Shortcut &shortcut, ShortcutOptions options = {}) noexcept;

/** @param shortcut Borrowed validated alternatives; never retained or mutated.
 * @param event Borrowed input event. ASCII letters fold case, with every modifier compared exactly.
 * @param behavior PRESS-only default; repeat requires explicit opt-in.
 * @return Original KeyEvent value, or a PRESS CHARACTER value preserving modified text scalar/modifiers.
 * Unmodified text matches only an explicitly registered character shortcut; other text passes through.
 * This stateless predicate checks single chords only; ManagedScene executes sequences.
 */
std::optional<KeyEvent> matchShortcut(const Shortcut &shortcut, const InputEvent &event,
                                      ShortcutBehavior behavior = {}) noexcept;

namespace detail {
/** @brief Fixed action mask used only after checked selection construction. */
enum class ShortcutActions : unsigned char {
  PRESS = 1,       ///< Admit PRESS.
  REPEAT = 2,      ///< Admit native REPEAT.
  RELEASE = 4,     ///< Admit native RELEASE.
  PRESS_REPEAT = 3 ///< Legacy repeat opt-in.
};
struct KeySelectionAccess;
} // namespace detail

/** @brief Copyable immutable owning shortcut selection, or a deferred typed construction error.
 * Copies share normalized values; no source string or initializer list is retained.
 * Moving/copying this receipt allocates nothing. Registration origin belongs to onKey.
 */
class KeySelection {
private:
  friend struct detail::KeySelectionAccess;

  KeySelection(std::expected<std::shared_ptr<const Shortcut>, ShortcutError> shortcut, detail::ShortcutActions actions,
               ShortcutOptions options) noexcept
      : shortcut_(std::move(shortcut)), actions_(actions), options_(options) {
  }

  std::expected<std::shared_ptr<const Shortcut>, ShortcutError> shortcut_;
  detail::ShortcutActions actions_;
  ShortcutOptions options_;
};

namespace detail {
/** @param event Native key or text event borrowed for this call.
 * @param actions Fixed action mask applied to this stroke.
 * @return Admitted original key or PRESS text; no synthetic native phases.
 */
std::optional<KeyEvent> shortcutEvent(const InputEvent &event, ShortcutActions actions) noexcept;

/** @param chord Validated semantic chord.
 * @param event Admitted stroke compared with ASCII letter folding.
 * @return Whether key, scalar and exact semantic modifiers match.
 */
bool shortcutChordMatches(key::Chord chord, const KeyEvent &event) noexcept;

/** @param shortcut Validated owning alternatives borrowed for this call.
 * @param index Combined index, singles followed by sequences; must be in range.
 * @return Borrowed strokes valid until the shortcut is modified or destroyed.
 */
std::span<const key::Chord> shortcutAlternative(const Shortcut &shortcut, std::size_t index) noexcept;

/** @param shortcut Validated owning alternatives borrowed during matching.
 * @param event Original native event or text, which represents PRESS only.
 * @param actions Explicit fixed action set.
 * @return Original admitted key, converted PRESS text, or no match.
 */
std::optional<KeyEvent> matchShortcutActions(const Shortcut &shortcut, const InputEvent &event,
                                             ShortcutActions actions) noexcept;

/** @brief Internal bridge keeping selection payload immutable and checked. */
struct KeySelectionAccess {
  /** @param shortcut Checked normalized alternatives transferred within the allocation catch.
   * @param actions Borrowed explicit native phases; invalid values retain their index.
   * @param options Original finite normalization limits.
   * @return Immutable receipt or deferred typed error; receipt transfer cannot allocate.
   */
  static KeySelection make(std::expected<Shortcut, ShortcutError> &&shortcut, std::initializer_list<KeyAction> actions,
                           ShortcutOptions options);

  /** @param selection Owning receipt borrowed during checked registration.
   * @return Immutable payload or deferred construction failure, borrowed from selection.
   */
  static const auto &shortcut(const KeySelection &selection) noexcept {
    return selection.shortcut_;
  }

  /** @param selection Owning receipt borrowed during registration.
   * @return Validated fixed action set.
   */
  static ShortcutActions actions(const KeySelection &selection) noexcept {
    return selection.actions_;
  }

  /** @param selection Owning receipt borrowed during registration.
   * @return Original finite normalization limits.
   */
  static ShortcutOptions options(const KeySelection &selection) noexcept {
    return selection.options_;
  }
};

/** @param chord Owning constexpr chord value.
 * @param options Positive shortcut budgets.
 * @return Validated owning single-alternative shortcut or a typed error.
 */
std::expected<Shortcut, ShortcutError> prepareShortcut(key::Chord chord, ShortcutOptions options = {});
/** @param chords Alternatives borrowed only during this call and copied before registration.
 * @param options Positive shortcut budgets.
 * @return Validated owned alternatives or a typed error.
 */
std::expected<Shortcut, ShortcutError> prepareShortcut(std::initializer_list<key::Chord> chords,
                                                       ShortcutOptions options = {});

/** @param source Shortcut text borrowed only during this call.
 * @param options Positive finite parser budgets.
 * @return Normalized owning alternatives or a typed error.
 */
inline std::expected<Shortcut, ShortcutError> prepareShortcut(std::string_view source, ShortcutOptions options = {}) {
  return parseShortcut(source, options);
}

/** @param shortcut Owning alternatives transferred to the result after validation.
 * @param options Positive finite alternative budgets.
 * @return Validated owning shortcut or a typed error.
 */
std::expected<Shortcut, ShortcutError> prepareShortcut(const Shortcut &shortcut, ShortcutOptions options = {});
/** @param shortcut Owning alternatives moved only inside the checked allocation boundary.
 * @param options Positive finite alternative budgets.
 * @return Validated owning shortcut or a typed error, including transfer allocation failure.
 */
std::expected<Shortcut, ShortcutError> prepareShortcut(Shortcut &&shortcut, ShortcutOptions options = {});

/** @param shortcut Checked normalized value borrowed until its owning storage is transferred inside the catch.
 * @return Pending owning shortcut or typed error; moving this receipt into a pipe stage allocates nothing.
 */
std::expected<std::shared_ptr<Shortcut>, ShortcutError>
retainShortcut(std::expected<Shortcut, ShortcutError> &&shortcut);

/** @brief A shortcut value or string accepted by the checked owning normalizer.
 * @tparam Source Chord, owning alternatives, initializer list or string-view-convertible source.
 */
template <class Source>
concept ShortcutSource = requires(Source source) { prepareShortcut(std::move(source)); };
} // namespace detail
} // namespace portals
