// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace portals {

/** @brief Caller-supplied monotonic milliseconds; no wall clock or I/O is used. */
using InputTime = std::uint64_t;

namespace key {
/** @brief Semantic modifier tags; physical left/right keys are not inferred. */
enum class Modifier {
  SHIFT, ///< Observed Shift intent.
  ALT,   ///< Observed Alt intent.
  CTRL   ///< Observed Control intent.
};
inline constexpr Modifier SHIFT = Modifier::SHIFT; ///< Shift query tag.
inline constexpr Modifier ALT = Modifier::ALT;     ///< Alt query tag.
inline constexpr Modifier CTRL = Modifier::CTRL;   ///< Control query tag.
} // namespace key

/** @brief Observed modifiers, also usable by a native input adapter. */
struct InputModifiers {
  bool shift{};   ///< Shift modifier.
  bool alt{};     ///< Alt modifier (legacy ESC prefix is ambiguous).
  bool control{}; ///< Control modifier.
  bool super{};   ///< Super modifier; never inferred by this VT profile.

  /** @param modifier Semantic modifier to inspect.
   * @return Whether this event reports the requested modifier.
   */
  constexpr bool has(key::Modifier modifier) const noexcept {
    switch (modifier) {
    case key::Modifier::SHIFT:
      return shift;
    case key::Modifier::ALT:
      return alt;
    case key::Modifier::CTRL:
      return control;
    }

    return false;
  }

  /** @brief Compare the complete event value.
   * @param other Event or metadata value to compare, without interpreting its payload.
   * @return True when every stored field is equal; false otherwise.
   */
  bool operator==(const InputModifiers &other) const = default;
};

/** @brief Semantic key identities; character uses KeyEvent::scalar. */
enum class InputKey {
  CHARACTER,  ///< Character key identified by KeyEvent::scalar; VT uses this for control combinations.
  ESCAPE,     ///< Escape key, including a standalone ESC resolved by timeout or end of input.
  ENTER,      ///< Enter key; the VT profile maps carriage return to this identity.
  TAB,        ///< Tab key; modifiers distinguish Shift+Tab.
  BACKSPACE,  ///< Backspace key; the VT profile accepts both BS and DEL bytes.
  UP,         ///< Up arrow key.
  DOWN,       ///< Down arrow key.
  RIGHT,      ///< Right arrow key.
  LEFT,       ///< Left arrow key.
  HOME,       ///< Home navigation key.
  END,        ///< End navigation key.
  INSERT,     ///< Insert key.
  DELETE_KEY, ///< Delete key, distinct from the DEL byte decoded as Backspace.
  PAGE_UP,    ///< Page Up navigation key.
  PAGE_DOWN,  ///< Page Down navigation key.
  F1,         ///< Function key F1.
  F2,         ///< Function key F2.
  F3,         ///< Function key F3.
  F4,         ///< Function key F4.
  F5,         ///< Function key F5.
  F6,         ///< Function key F6.
  F7,         ///< Function key F7.
  F8,         ///< Function key F8.
  F9,         ///< Function key F9.
  F10,        ///< Function key F10.
  F11,        ///< Function key F11.
  F12         ///< Function key F12.
};

namespace key {
/** @brief Compile-time semantic shortcut; letters match ASCII case with exact modifiers. */
struct Chord {
  InputKey key{};             ///< Semantic key identity.
  char32_t scalar{};          ///< ASCII character identity, zero for named keys.
  InputModifiers modifiers{}; ///< Exact observed semantic modifiers.
};

/** @param modifier Additional semantic modifier.
 * @param chord Shortcut copied and extended without physical-key inference.
 * @return Combined shortcut value.
 */
constexpr Chord operator+(Modifier modifier, Chord chord) noexcept {
  switch (modifier) {
  case Modifier::SHIFT:
    chord.modifiers.shift = true;
    break;
  case Modifier::ALT:
    chord.modifiers.alt = true;
    break;
  case Modifier::CTRL:
    chord.modifiers.control = true;
    break;
  }

  return chord;
}

/** @param first First modifier tag.
 * @param second Second modifier tag.
 * @return Observed semantic modifier set for a later chord.
 */
constexpr InputModifiers operator+(Modifier first, Modifier second) noexcept {
  return (first + (second + Chord{})).modifiers;
}

/** @param modifiers Semantic modifiers to merge.
 * @param chord Shortcut copied and extended.
 * @return Combined shortcut with no physical-key or AltGr inference.
 */
constexpr Chord operator+(InputModifiers modifiers, Chord chord) noexcept {
  chord.modifiers.shift = chord.modifiers.shift || modifiers.shift;
  chord.modifiers.alt = chord.modifiers.alt || modifiers.alt;
  chord.modifiers.control = chord.modifiers.control || modifiers.control;
  chord.modifiers.super = chord.modifiers.super || modifiers.super;

  return chord;
}

/** @param modifiers Existing semantic modifier set.
 * @param modifier Additional semantic tag.
 * @return Merged modifier set for a later chord.
 */
constexpr InputModifiers operator+(InputModifiers modifiers, Modifier modifier) noexcept {
  return (modifiers + (modifier + Chord{})).modifiers;
}

inline constexpr Chord ESCAPE{InputKey::ESCAPE};         ///< ESCAPE shortcut.
inline constexpr Chord ENTER{InputKey::ENTER};           ///< ENTER shortcut.
inline constexpr Chord TAB{InputKey::TAB};               ///< TAB shortcut.
inline constexpr Chord BACKSPACE{InputKey::BACKSPACE};   ///< BACKSPACE shortcut.
inline constexpr Chord UP{InputKey::UP};                 ///< UP shortcut.
inline constexpr Chord DOWN{InputKey::DOWN};             ///< DOWN shortcut.
inline constexpr Chord RIGHT{InputKey::RIGHT};           ///< RIGHT shortcut.
inline constexpr Chord LEFT{InputKey::LEFT};             ///< LEFT shortcut.
inline constexpr Chord HOME{InputKey::HOME};             ///< HOME shortcut.
inline constexpr Chord END{InputKey::END};               ///< END shortcut.
inline constexpr Chord INSERT{InputKey::INSERT};         ///< INSERT shortcut.
inline constexpr Chord DELETE_KEY{InputKey::DELETE_KEY}; ///< DELETE_KEY shortcut.
inline constexpr Chord PAGE_UP{InputKey::PAGE_UP};       ///< PAGE_UP shortcut.
inline constexpr Chord PAGE_DOWN{InputKey::PAGE_DOWN};   ///< PAGE_DOWN shortcut.
inline constexpr Chord F1{InputKey::F1};                 ///< F1 shortcut.
inline constexpr Chord F2{InputKey::F2};                 ///< F2 shortcut.
inline constexpr Chord F3{InputKey::F3};                 ///< F3 shortcut.
inline constexpr Chord F4{InputKey::F4};                 ///< F4 shortcut.
inline constexpr Chord F5{InputKey::F5};                 ///< F5 shortcut.
inline constexpr Chord F6{InputKey::F6};                 ///< F6 shortcut.
inline constexpr Chord F7{InputKey::F7};                 ///< F7 shortcut.
inline constexpr Chord F8{InputKey::F8};                 ///< F8 shortcut.
inline constexpr Chord F9{InputKey::F9};                 ///< F9 shortcut.
inline constexpr Chord F10{InputKey::F10};               ///< F10 shortcut.
inline constexpr Chord F11{InputKey::F11};               ///< F11 shortcut.
inline constexpr Chord F12{InputKey::F12};               ///< F12 shortcut.
inline constexpr Chord A{InputKey::CHARACTER, U'A'};     ///< ASCII A shortcut, case insensitive.
inline constexpr Chord B{InputKey::CHARACTER, U'B'};     ///< ASCII B shortcut, case insensitive.
inline constexpr Chord C{InputKey::CHARACTER, U'C'};     ///< ASCII C shortcut, case insensitive.
inline constexpr Chord D{InputKey::CHARACTER, U'D'};     ///< ASCII D shortcut, case insensitive.
inline constexpr Chord E{InputKey::CHARACTER, U'E'};     ///< ASCII E shortcut, case insensitive.
inline constexpr Chord F{InputKey::CHARACTER, U'F'};     ///< ASCII F shortcut, case insensitive.
inline constexpr Chord G{InputKey::CHARACTER, U'G'};     ///< ASCII G shortcut, case insensitive.
inline constexpr Chord H{InputKey::CHARACTER, U'H'};     ///< ASCII H shortcut, case insensitive.
inline constexpr Chord I{InputKey::CHARACTER, U'I'};     ///< ASCII I shortcut, case insensitive.
inline constexpr Chord J{InputKey::CHARACTER, U'J'};     ///< ASCII J shortcut, case insensitive.
inline constexpr Chord K{InputKey::CHARACTER, U'K'};     ///< ASCII K shortcut, case insensitive.
inline constexpr Chord L{InputKey::CHARACTER, U'L'};     ///< ASCII L shortcut, case insensitive.
inline constexpr Chord M{InputKey::CHARACTER, U'M'};     ///< ASCII M shortcut, case insensitive.
inline constexpr Chord N{InputKey::CHARACTER, U'N'};     ///< ASCII N shortcut, case insensitive.
inline constexpr Chord O{InputKey::CHARACTER, U'O'};     ///< ASCII O shortcut, case insensitive.
inline constexpr Chord P{InputKey::CHARACTER, U'P'};     ///< ASCII P shortcut, case insensitive.
inline constexpr Chord Q{InputKey::CHARACTER, U'Q'};     ///< ASCII Q shortcut, case insensitive.
inline constexpr Chord R{InputKey::CHARACTER, U'R'};     ///< ASCII R shortcut, case insensitive.
inline constexpr Chord S{InputKey::CHARACTER, U'S'};     ///< ASCII S shortcut, case insensitive.
inline constexpr Chord T{InputKey::CHARACTER, U'T'};     ///< ASCII T shortcut, case insensitive.
inline constexpr Chord U{InputKey::CHARACTER, U'U'};     ///< ASCII U shortcut, case insensitive.
inline constexpr Chord V{InputKey::CHARACTER, U'V'};     ///< ASCII V shortcut, case insensitive.
inline constexpr Chord W{InputKey::CHARACTER, U'W'};     ///< ASCII W shortcut, case insensitive.
inline constexpr Chord X{InputKey::CHARACTER, U'X'};     ///< ASCII X shortcut, case insensitive.
inline constexpr Chord Y{InputKey::CHARACTER, U'Y'};     ///< ASCII Y shortcut, case insensitive.
inline constexpr Chord Z{InputKey::CHARACTER, U'Z'};     ///< ASCII Z shortcut, case insensitive.
} // namespace key

/** @brief Native adapters may report repeat/release; VT emits press only. */
enum class KeyAction {
  PRESS,  ///< Key press; the only action emitted by the VT decoder.
  REPEAT, ///< Repeated key press reported by a native adapter.
  RELEASE ///< Key release reported by a native adapter.
};

/** @brief Owning key value without an inferred physical keyboard scan code. */
struct KeyEvent {
  InputKey key{};             ///< Semantic identity.
  char32_t scalar{};          ///< Character for control combinations, otherwise zero.
  InputModifiers modifiers{}; ///< Observed modifiers.
  KeyAction action{};         ///< Observed action; VT does not infer held state.
  /** @brief Compare the complete event value.
   * @param other Event or metadata value to compare, without interpreting its payload.
   * @return True when every stored field is equal; false otherwise.
   */
  bool operator==(const KeyEvent &other) const = default;
};

/** @brief One Unicode scalar, not a grapheme cluster; Alt may be present. */
struct TextInputEvent {
  char32_t scalar{};          ///< Valid Unicode scalar, without normalization.
  InputModifiers modifiers{}; ///< Observed modifiers.
  /** @brief Compare the complete event value.
   * @param other Event or metadata value to compare, without interpreting its payload.
   * @return True when every stored field is equal; false otherwise.
   */
  bool operator==(const TextInputEvent &other) const = default;
};

/** @brief SGR button identity; NO_BUTTON is used for unbuttoned motion. */
enum class MouseButton {
  LEFT,       ///< Left mouse button.
  MIDDLE,     ///< Middle mouse button.
  RIGHT,      ///< Right mouse button.
  NO_BUTTON,  ///< Motion reported without a pressed button.
  WHEEL_UP,   ///< Vertical wheel movement upward.
  WHEEL_DOWN, ///< Vertical wheel movement downward.
  WHEEL_LEFT, ///< Horizontal wheel movement leftward.
  WHEEL_RIGHT ///< Horizontal wheel movement rightward.
};

/** @brief Motion and wheel are separate from button transitions. */
enum class MouseAction {
  PRESS,   ///< Mouse button press.
  RELEASE, ///< Mouse button release.
  MOVE,    ///< Pointer motion, with a reported button or MouseButton::NO_BUTTON.
  SCROLL   ///< Wheel movement in the direction given by MouseInputEvent::button.
};

/** @brief Checked zero-based cell coordinates; no viewport clipping is performed. */
struct MouseInputEvent {
  std::int32_t column{};      ///< Zero-based column.
  std::int32_t row{};         ///< Zero-based row.
  MouseButton button{};       ///< Button or wheel direction.
  MouseAction action{};       ///< Observed action.
  InputModifiers modifiers{}; ///< Observed modifiers.
  /** @brief Compare the complete event value.
   * @param other Event or metadata value to compare, without interpreting its payload.
   * @return True when every stored field is equal; false otherwise.
   */
  bool operator==(const MouseInputEvent &other) const = default;
};

/** @brief Owning raw paste bytes, possibly invalid UTF-8 or containing controls.
 * Never forward these bytes to a terminal writer or interpret them as commands.
 */
struct PasteInputEvent {
  std::string bytes; ///< Complete bounded payload, excluding delimiters.
  /** @brief Compare the complete event value.
   * @param other Event or metadata value to compare, without interpreting its payload.
   * @return True when every stored field is equal; false otherwise.
   */
  bool operator==(const PasteInputEvent &other) const = default;
};

/** @brief Supported syntactic reply kinds, not proof of terminal identity. */
enum class InputProbeKind {
  PRIMARY_DEVICE_ATTRIBUTES, ///< Primary device attributes reply; values contain its numeric parameters.
  CURSOR_POSITION            ///< Cursor position reply; values contain the one-based row followed by column.
};

/** @brief A single-use expected reply window, captured at sequence start. */
struct InputProbeWindow {
  bool device_attributes{};   ///< Allow one primary DA candidate.
  bool cursor_position{};     ///< Allow one CPR candidate.
  std::uint64_t generation{}; ///< Caller correlation value, not authentication.
  InputTime opens_at{};       ///< Inclusive start.
  InputTime deadline{};       ///< Exclusive end.
};

/** @brief Syntactically matching reply within an explicitly expected window. */
struct InputProbeCandidate {
  InputProbeKind kind{};                  ///< Reply grammar.
  std::uint64_t generation{};             ///< Caller-supplied generation.
  std::array<std::uint32_t, 16> values{}; ///< DA parameters, or one-based CPR row/column.
  std::size_t count{};                    ///< Number of valid parameters.
  /** @brief Compare the complete event value.
   * @param other Event or metadata value to compare, without interpreting its payload.
   * @return True when every stored field is equal; false otherwise.
   */
  bool operator==(const InputProbeCandidate &other) const = default;
};

/** @brief Recoverable decode issues; timeout may also leave the decoder failed. */
enum class InputIssueKind {
  INVALID_UTF8,     ///< Invalid UTF-8 byte sequence or a decoded value that is not a Unicode scalar.
  TRUNCATED,        ///< Incomplete input at stream end, or an unfinished UTF-8 scalar at its deadline.
  UNKNOWN_SEQUENCE, ///< Unsupported control sequence, including disabled features or unexpected replies.
  INVALID_SEQUENCE, ///< Malformed control syntax or invalid SGR mouse parameters.
  SEQUENCE_LIMIT,   ///< CSI or SS3 sequence exceeds InputOptions::max_sequence_bytes.
  PARAMETER_LIMIT,  ///< Parameter count or decimal field length exceeds max_parameters or max_digits.
  NUMERIC_OVERFLOW, ///< A decimal parameter cannot be represented as a uint32_t value.
  PASTE_LIMIT,      ///< Paste payload exceeds max_paste_bytes; no partial paste event is delivered.
  TIMEOUT           ///< Control sequence, control string or paste reached its deadline; reset is required.
};

/** @brief Byte offsets describe the consumed rejected prefix, without raw ANSI diagnostics.
 * Subsequent discard bytes are accounted by InputStep::consumed, not another issue.
 */
struct InputIssue {
  InputIssueKind kind{}; ///< Rejection reason.
  std::uint64_t begin{}; ///< Inclusive session byte offset.
  std::uint64_t end{};   ///< Exclusive offset at issue emission.
  /** @brief Compare the complete event value.
   * @param other Event or metadata value to compare, without interpreting its payload.
   * @return True when every stored field is equal; false otherwise.
   */
  bool operator==(const InputIssue &other) const = default;
};

/** @brief Independent owning result, usable beyond decoder reset/destruction. */
using InputEvent =
  std::variant<KeyEvent, TextInputEvent, MouseInputEvent, PasteInputEvent, InputProbeCandidate, InputIssue>;

/** @brief Construction and call errors, separate from events caused by input bytes. */
enum class InputError {
  INVALID_OPTIONS, ///< A construction option is outside the supported storage, work or timeout bounds.
  BACKWARD_TIME,   ///< The supplied timestamp precedes the last accepted next() or finish() timestamp.
  CLOSED,          ///< next() was called after finish() began closing the stream; reset starts a new stream.
  FAILED,          ///< The decoder previously failed and must be reset before decoding can continue.
  OFFSET_OVERFLOW, ///< The maximum prefix this call could consume would overflow the session byte offset.
  INVALID_WINDOW   ///< Reply interval is empty/reversed, or the decoder is not at an open, idle boundary.
};

/** @brief Why a pull stopped; event is delivered before calling again. */
enum class InputStatus {
  EVENT,      ///< One event is ready; deliver it before resubmitting the unconsumed suffix.
  NEED_INPUT, ///< All supplied bytes were consumed without an event; more input, time or finish() is needed.
  WORK_LIMIT, ///< Per-call byte budget was reached without an event; resubmit the unconsumed suffix.
  FINISHED,   ///< End-of-stream flushing is complete; this step may still contain the final event.
  FAILED      ///< This step reports a fatal timeout event; reset is required before decoding can continue.
};

/** @brief One bounded pull result; no hidden output queue. */
struct InputStep {
  std::size_t consumed{};          ///< Prefix of this call's bytes accepted exactly once.
  std::optional<InputEvent> event; ///< At most one independent owning event.
  InputStatus status{};            ///< Stop reason.
};

/** @brief Explicit resource and timing policy, independent of terminal capabilities. */
struct InputOptions {
  std::size_t max_sequence_bytes{64};       ///< CSI/SS3 cap including ESC, introducer and final; 3..64.
  std::size_t max_parameters{16};           ///< Numeric fields per sequence; 1..16.
  std::size_t max_digits{10};               ///< Decimal digits per field; 1..10, with checked uint32 arithmetic.
  std::size_t max_paste_bytes{1024 * 1024}; ///< Maximum complete payload; zero permits empty paste.
  std::size_t work_bytes{4096};             ///< Maximum accepted bytes per pull; must be positive.
  InputTime escape_timeout{30};             ///< ESC/Alt scalar ambiguity interval; must be positive.
  InputTime sequence_timeout{1000};         ///< Whole CSI/SS3/string/ordinary UTF-8 interval; positive.
  InputTime paste_timeout{30000};           ///< Whole paste interval from begin delimiter; positive.
  bool sgr_mouse{true};                     ///< Decode SGR cell mouse reports.
  bool bracketed_paste{true};               ///< Decode bracketed paste delimiters.
};

/**
 * @brief Bounded streaming decoder for a deliberately small 7-bit VT input profile.
 * @details One executor owns an instance; synchronize any transfer between threads.
 * Independent instances may run concurrently. No callbacks, I/O, shared globals or
 * terminal mode changes occur. Events own their data; input views are borrowed only
 * during next(). Storage is O(max_paste_bytes) plus fixed scratch and one pending
 * issue. Caller must bound its own retained events. Allocation failures propagate,
 * leaving the decoder failed until reset; no partial paste is delivered.
 * @note For identical timestamps and feed/finish/window schedules, event order does
 * not depend on chunk boundaries. Preserve timestamps when resubmitting suffixes.
 * Deadlines saturate at UINT64_MAX; now >= deadline expires before accepting bytes.
 * The legacy protocol cannot distinguish typed/forged replies or paste end markers.
 */
class InputDecoder {
public:
  /** @brief Validate options and create an empty decoder. No allocations or I/O.
   * @param options Work, storage and timeout bounds for the lifetime of this stream.
   * @return An empty decoder, or INVALID_OPTIONS when a bound is outside its documented range.
   */
  [[nodiscard]] static std::expected<InputDecoder, InputError> create(InputOptions options = {});
  InputDecoder(const InputDecoder &) = delete;            ///< Stateful streams cannot be copied.
  InputDecoder &operator=(const InputDecoder &) = delete; ///< Stateful streams cannot be copied.
  /** @brief Moved-from object must be reset before use.
   * @param other Stream state and buffered bytes to transfer; reset it before reuse.
   */
  InputDecoder(InputDecoder &&other) noexcept = default;
  /** @brief Reset moved-from object before reuse.
   * @param other Stream state that replaces the current, unfinished stream.
   * @return This decoder after memberwise state transfer; reset a moved-from source before reuse.
   */
  InputDecoder &operator=(InputDecoder &&other) noexcept = default;

  /**
   * @brief Pull at most one event, accepting at most work_bytes from a borrowed view.
   * @param bytes Unconsumed input suffix; never retained as a view.
   * @param now Monotonic arrival timestamp; equal values are allowed.
   * @return Consumed count/event/stop reason, or call error before consuming input.
   * @note Empty input advances time and drains pending events. Repeat with an empty
   * view until NEED_INPUT/failed to drain. Errors do not consume bytes. No invalidation
   * of earlier events. Allocation exceptions require reset (discard session suffix).
   */
  [[nodiscard]] std::expected<InputStep, InputError> next(std::string_view bytes, InputTime now);
  /** @brief End the stream, flushing at most one pending event per call.
   * Repeat until finished/failed. next() then returns closed; reset starts a new session.
   * Monotonic time and allocation/error contracts match next(). No input is consumed.
   * @param now Monotonic timestamp, not earlier than the last accepted call.
   * @return At most one final event and its stop reason, or a call error with no input consumed.
   */
  [[nodiscard]] std::expected<InputStep, InputError> finish(InputTime now);
  /** @brief Return the next decode deadline, or none in ground/closed/failed state.
   * Pending events must be drained before waiting. No allocation or mutation.
   * @return Next absolute decoder timeout, or nullopt when waiting on time cannot advance decoding.
   */
  [[nodiscard]] std::optional<InputTime> deadline() const noexcept;
  /** @brief Replace expected replies; valid only in ground with no pending event.
   * Returns INVALID_WINDOW if deadline <= opens_at or not at a sequence boundary.
   * Does not change time, own a query, send bytes, or authenticate future candidates.
   * @param window Expected single-use reply kinds, correlation generation and half-open time interval.
   * @return Success, or INVALID_WINDOW when the interval or current stream boundary is invalid.
   */
  [[nodiscard]] std::expected<void, InputError> expectReplies(InputProbeWindow window) noexcept;
  /** @brief Drop pending input/window/time/offset and recover at an explicit new session boundary.
   * Options remain. Frees retained paste; does not invalidate already returned events.
   */
  void reset() noexcept;

private:
  enum class State {
    GROUND,
    ESCAPE,
    UTF8,
    CSI,
    SS3,
    DISCARD_CSI,
    STRING,
    PASTE,
    DISCARD_PASTE,
    FAILED
  };

  explicit InputDecoder(InputOptions options) : options_(options) {
  }

  std::expected<InputStep, InputError> pull(std::string_view bytes, InputTime now, bool ending);
  std::optional<InputEvent> byte(unsigned char value, bool &consume, InputTime now);
  std::optional<InputEvent> ground(unsigned char value, bool alt, InputTime now);
  std::optional<InputEvent> control(unsigned char value, InputTime now);
  std::optional<InputEvent> sequence(InputTime now);
  std::optional<InputEvent> paste(unsigned char value);
  std::optional<InputEvent> expire(InputTime now);
  InputIssue issue(InputIssueKind kind) const noexcept;
  void begin(State state, InputTime now, InputTime duration) noexcept;
  void clearSequence() noexcept;

  InputOptions options_;
  State state_{State::GROUND};
  std::optional<InputTime> last_time_;
  InputTime deadline_{};
  InputTime sequence_started_{};
  std::uint64_t offset_{};
  std::uint64_t start_{};
  std::array<char, 64> sequence_{};
  std::size_t length_{};
  bool intermediate_{};
  bool string_osc_{};
  bool string_escape_{};
  char32_t scalar_{};
  char32_t minimum_scalar_{};
  unsigned remaining_{};
  bool alt_{};
  std::string paste_;
  std::size_t marker_{};
  InputProbeWindow window_;
  std::optional<InputEvent> pending_;
  bool ending_{};
  bool finished_{};
};

} // namespace portals
