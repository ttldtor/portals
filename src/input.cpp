// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include <portals/input.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace portals {
namespace {
constexpr unsigned char escape_byte = 0x1b;
constexpr std::string_view paste_end = "\x1b[201~";

InputTime after(InputTime now, InputTime interval) noexcept {
  return now > std::numeric_limits<InputTime>::max() - interval ? std::numeric_limits<InputTime>::max()
                                                                : now + interval;
}

InputModifiers modifiers(std::uint32_t bits) noexcept {
  return {(bits & 1) != 0, (bits & 2) != 0, (bits & 4) != 0, false};
}

std::optional<InputKey> letter_key(char final) noexcept {
  switch (final) {
  case 'A':
    return InputKey::up;
  case 'B':
    return InputKey::down;
  case 'C':
    return InputKey::right;
  case 'D':
    return InputKey::left;
  case 'H':
    return InputKey::home;
  case 'F':
    return InputKey::end;
  case 'P':
    return InputKey::f1;
  case 'Q':
    return InputKey::f2;
  case 'R':
    return InputKey::f3;
  case 'S':
    return InputKey::f4;
  default:
    return {};
  }
}

std::optional<InputKey> tilde_key(std::uint32_t number) noexcept {
  switch (number) {
  case 1:
  case 7:
    return InputKey::home;
  case 2:
    return InputKey::insert;
  case 3:
    return InputKey::delete_key;
  case 4:
  case 8:
    return InputKey::end;
  case 5:
    return InputKey::page_up;
  case 6:
    return InputKey::page_down;
  case 11:
    return InputKey::f1;
  case 12:
    return InputKey::f2;
  case 13:
    return InputKey::f3;
  case 14:
    return InputKey::f4;
  case 15:
    return InputKey::f5;
  case 17:
    return InputKey::f6;
  case 18:
    return InputKey::f7;
  case 19:
    return InputKey::f8;
  case 20:
    return InputKey::f9;
  case 21:
    return InputKey::f10;
  case 23:
    return InputKey::f11;
  case 24:
    return InputKey::f12;
  default:
    return {};
  }
}
} // namespace

std::expected<InputDecoder, InputError> InputDecoder::create(InputOptions options) {
  if (options.max_sequence_bytes < 3 || options.max_sequence_bytes > 64 || options.max_parameters == 0 ||
      options.max_parameters > 16 || options.max_digits == 0 || options.max_digits > 10 ||
      options.max_paste_bytes > std::string{}.max_size() || options.work_bytes == 0 || options.escape_timeout == 0 ||
      options.sequence_timeout == 0 || options.paste_timeout == 0) {
    return std::unexpected(InputError::invalid_options);
  }

  return InputDecoder{options};
}

void InputDecoder::reset() noexcept {
  state_ = State::ground;
  last_time_.reset();
  deadline_ = sequence_started_ = offset_ = start_ = 0;
  length_ = marker_ = 0;
  intermediate_ = string_osc_ = string_escape_ = alt_ = ending_ = finished_ = false;
  remaining_ = 0;
  scalar_ = minimum_scalar_ = 0;
  std::string{}.swap(paste_);
  window_ = {};
  pending_.reset();
}

std::optional<InputTime> InputDecoder::deadline() const noexcept {
  if (state_ == State::ground || state_ == State::failed || finished_) {
    return {};
  }

  return deadline_;
}

std::expected<void, InputError> InputDecoder::expect_replies(InputProbeWindow window) noexcept {
  if (state_ != State::ground || pending_ || ending_ || window.deadline <= window.opens_at) {
    return std::unexpected(InputError::invalid_window);
  }

  window_ = window;

  return {};
}

InputIssue InputDecoder::issue(InputIssueKind kind) const noexcept {
  return {kind, start_, offset_};
}

void InputDecoder::begin(State state, InputTime now, InputTime duration) noexcept {
  state_ = state;
  sequence_started_ = now;
  deadline_ = after(now, duration);
}

void InputDecoder::clear_sequence() noexcept {
  state_ = State::ground;
  length_ = 0;
  intermediate_ = false;
  remaining_ = 0;
  alt_ = false;
}

std::optional<InputEvent> InputDecoder::expire(InputTime now) {
  if (!deadline() || now < deadline_) {
    return {};
  }

  if (state_ == State::escape) {
    clear_sequence();

    return KeyEvent{InputKey::escape};
  }

  if (state_ == State::utf8) {
    const auto was_alt = alt_;
    auto error = issue(InputIssueKind::truncated);
    error.begin += was_alt ? 1 : 0;
    clear_sequence();

    if (was_alt) {
      pending_ = error;

      return KeyEvent{InputKey::escape};
    }

    return error;
  }

  // Without a terminator, accepting a suffix as keys would cross the data boundary.
  state_ = State::failed;
  std::string{}.swap(paste_);

  return issue(InputIssueKind::timeout);
}

std::optional<InputEvent> InputDecoder::ground(unsigned char value, bool alt, InputTime now) {
  if (!alt) {
    start_ = offset_ - 1;
  }

  if (value == escape_byte) {
    begin(State::escape, now, options_.escape_timeout);

    return {};
  }

  const InputModifiers observed{false, alt, false, false};

  if (value < 0x20 || value == 0x7f) {
    InputKey key = InputKey::character;

    switch (value) {
    case 9:
      key = InputKey::tab;
      break;
    case 13:
      key = InputKey::enter;
      break;
    case 8:
    case 0x7f:
      key = InputKey::backspace;
      break;
    default:
      break;
    }

    auto mods = observed;
    mods.control = key == InputKey::character;

    return KeyEvent{key, key == InputKey::character ? static_cast<char32_t>(value + 0x40) : U'\0', mods};
  }

  if (value < 0x80) {
    return TextInputEvent{static_cast<char32_t>(value), observed};
  }

  if (value >= 0xc2 && value <= 0xf4) {
    remaining_ = value < 0xe0 ? 1 : value < 0xf0 ? 2 : 3;
    scalar_ = value & (remaining_ == 1 ? 0x1f : remaining_ == 2 ? 0x0f : 0x07);
    minimum_scalar_ = remaining_ == 1 ? 0x80 : remaining_ == 2 ? 0x800 : 0x10000;
    alt_ = alt;

    if (alt) {
      state_ = State::utf8;
    } else {
      begin(State::utf8, now, options_.sequence_timeout);
    }

    return {};
  }

  return issue(InputIssueKind::invalid_utf8);
}

std::optional<InputEvent> InputDecoder::control(unsigned char value, InputTime now) {
  if (value < 0x20 || value > 0x7e) {
    clear_sequence();

    return issue(InputIssueKind::invalid_sequence);
  }

  const auto final = value >= 0x40;

  if (state_ == State::discard_csi) {
    if (final) {
      clear_sequence();
    }

    return {};
  }

  if (length_ == options_.max_sequence_bytes) {
    state_ = final ? State::ground : State::discard_csi;

    return issue(InputIssueKind::sequence_limit);
  }

  sequence_[length_++] = static_cast<char>(value);

  if (final) {
    auto event = sequence(now);

    if (state_ != State::paste) {
      clear_sequence();
    }

    return event;
  }

  if (value < 0x30) {
    intermediate_ = true;
  } else if (intermediate_) {
    state_ = State::discard_csi;

    return issue(InputIssueKind::invalid_sequence);
  }

  return {};
}

// Parse the bounded numeric grammar before interpreting the final byte. A syntactic
// reply is only a candidate inside the caller-owned window, never terminal authentication.
std::optional<InputEvent> InputDecoder::sequence(InputTime now) {
  const bool ss3 = state_ == State::ss3;
  const char final = sequence_[length_ - 1];
  auto body = std::string_view{sequence_.data() + 2, length_ - 3};
  char prefix{};

  if (!body.empty() && (body.front() == '<' || body.front() == '?')) {
    prefix = body.front();
    body.remove_prefix(1);
  }

  if (intermediate_) {
    return issue(InputIssueKind::unknown_sequence);
  }

  std::array<std::uint32_t, 16> values{};
  std::size_t count = 0;

  while (!body.empty()) {
    if (count == options_.max_parameters) {
      return issue(InputIssueKind::parameter_limit);
    }

    std::uint32_t number = 0;
    std::size_t digits = 0;

    while (!body.empty() && body.front() >= '0' && body.front() <= '9') {
      const auto digit = static_cast<std::uint32_t>(body.front() - '0');

      if (++digits > options_.max_digits) {
        return issue(InputIssueKind::parameter_limit);
      }

      if (number > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) {
        return issue(InputIssueKind::numeric_overflow);
      }

      number = number * 10 + digit;
      body.remove_prefix(1);
    }

    if (digits == 0) {
      return issue(InputIssueKind::invalid_sequence);
    }

    values[count++] = number;

    if (!body.empty()) {
      if (body.front() != ';' || body.size() == 1) {
        return issue(InputIssueKind::invalid_sequence);
      }

      body.remove_prefix(1);
    }
  }

  const bool in_window = sequence_started_ >= window_.opens_at && now < window_.deadline;

  if (!ss3 && prefix == '?' && final == 'c' && count > 0 && in_window && window_.device_attributes) {
    window_.device_attributes = false;

    return InputProbeCandidate{InputProbeKind::primary_device_attributes, window_.generation, values, count};
  }

  if (!ss3 && prefix == 0 && final == 'R' && count == 2 && values[0] > 0 && values[1] > 0 && in_window &&
      window_.cursor_position) {
    window_.cursor_position = false;

    return InputProbeCandidate{InputProbeKind::cursor_position, window_.generation, values, count};
  }

  if (!ss3 && prefix == '<' && (final == 'M' || final == 'm') && options_.sgr_mouse) {
    constexpr std::uint32_t max_one_based = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()) + 1;

    if (count != 3 || values[0] > 127 || values[1] == 0 || values[2] == 0 || values[1] > max_one_based ||
        values[2] > max_one_based) {
      return issue(InputIssueKind::invalid_sequence);
    }

    const auto code = values[0];
    const auto button = code & 3;
    const bool motion = (code & 32) != 0;
    const bool wheel = (code & 64) != 0;

    if ((wheel && (motion || final == 'm')) || (!wheel && button == 3 && (!motion || final == 'm')) ||
        (motion && final == 'm')) {
      return issue(InputIssueKind::invalid_sequence);
    }

    const auto identity = wheel ? static_cast<MouseButton>(static_cast<unsigned>(MouseButton::wheel_up) + button)
                                : static_cast<MouseButton>(button);
    const auto action = wheel          ? MouseAction::scroll
                        : motion       ? MouseAction::move
                        : final == 'm' ? MouseAction::release
                                       : MouseAction::press;

    return MouseInputEvent{static_cast<std::int32_t>(values[1] - 1), static_cast<std::int32_t>(values[2] - 1), identity,
                           action, modifiers((code >> 2) & 7)};
  }

  if (prefix != 0) {
    return issue(InputIssueKind::unknown_sequence);
  }

  if (!ss3 && final == '~' && count == 1 && values[0] == 200 && options_.bracketed_paste) {
    paste_.clear();
    marker_ = 0;
    begin(State::paste, now, options_.paste_timeout);

    return {};
  }

  if (!ss3 && final == 'Z' && count == 0) {
    return KeyEvent{InputKey::tab, 0, {true, false, false, false}};
  }

  std::optional<InputKey> key;
  std::uint32_t modifier = 1;

  if (!ss3 && final == '~' && (count == 1 || count == 2)) {
    key = tilde_key(values[0]);
    modifier = count == 2 ? values[1] : 1;
  } else if (count == 0 || (count == 1 && values[0] == 1) || (count == 2 && values[0] == 1)) {
    key = letter_key(final);
    modifier = count == 2 ? values[1] : 1;
  }

  if (key && modifier >= 1 && modifier <= 8) {
    return KeyEvent{*key, 0, modifiers(modifier - 1)};
  }

  return issue(InputIssueKind::unknown_sequence);
}

std::optional<InputEvent> InputDecoder::paste(unsigned char value) {
  // Only the exact six-byte end marker has meaning inside a paste.
  if (value == static_cast<unsigned char>(paste_end[marker_])) {
    if (++marker_ == paste_end.size()) {
      marker_ = 0;
      const auto discarded = state_ == State::discard_paste;
      clear_sequence();

      if (!discarded) {
        std::string payload;
        payload.swap(paste_);

        return PasteInputEvent{std::move(payload)};
      }
    }

    return {};
  }

  // A failed terminator prefix is literal paste data. Keep a new leading ESC as the
  // next candidate instead of interpreting any payload byte as an ordinary key.
  const auto held = marker_;
  marker_ = value == escape_byte ? 1 : 0;
  const auto extra = held + (marker_ == 0 ? 1 : 0);

  if (state_ == State::discard_paste) {
    return {};
  }

  if (extra > options_.max_paste_bytes - paste_.size()) {
    std::string{}.swap(paste_);
    state_ = State::discard_paste;

    return issue(InputIssueKind::paste_limit);
  }

  paste_.append(paste_end.substr(0, held));

  if (marker_ == 0) {
    paste_ += static_cast<char>(value);
  }

  return {};
}

// consume=false emits an event for the preceding prefix while leaving this byte for
// the next pull. The caller rolls back its offset, preserving chunk-independent decoding.
std::optional<InputEvent> InputDecoder::byte(unsigned char value, bool &consume, InputTime now) {
  switch (state_) {
  case State::ground:
    return ground(value, false, now);
  case State::escape:
    if (value == escape_byte) {
      consume = false;
      clear_sequence();

      return KeyEvent{InputKey::escape};
    }

    if (value == '[' || value == 'O') {
      state_ = value == '[' ? State::csi : State::ss3;
      deadline_ = after(sequence_started_, options_.sequence_timeout);
      sequence_[0] = static_cast<char>(escape_byte);
      sequence_[1] = static_cast<char>(value);
      length_ = 2;

      return {};
    }

    if (value == ']' || value == 'P' || value == 'X' || value == '^' || value == '_') {
      state_ = State::string;
      deadline_ = after(sequence_started_, options_.sequence_timeout);
      string_osc_ = value == ']';
      string_escape_ = false;

      return issue(InputIssueKind::unknown_sequence);
    }

    state_ = State::ground;

    return ground(value, true, now);
  case State::utf8:
    if ((value & 0xc0) != 0x80) {
      consume = false;
      auto error = issue(InputIssueKind::invalid_utf8);
      --error.end;
      clear_sequence();

      return error;
    }

    scalar_ = (scalar_ << 6) | (value & 0x3f);

    if (--remaining_ == 0) {
      const auto scalar = scalar_;
      const auto alt = alt_;
      const auto valid = scalar >= minimum_scalar_ && scalar <= 0x10ffff && !(scalar >= 0xd800 && scalar <= 0xdfff);
      clear_sequence();

      if (!valid) {
        return issue(InputIssueKind::invalid_utf8);
      }

      return TextInputEvent{scalar, {false, alt, false, false}};
    }

    return {};
  case State::csi:
  case State::ss3:
  case State::discard_csi:
    return control(value, now);
  case State::string:
    if ((string_escape_ && value == '\\') || (string_osc_ && value == 7)) {
      clear_sequence();
    }

    string_escape_ = value == escape_byte;

    return {};
  case State::paste:
  case State::discard_paste:
    return paste(value);
  case State::failed:
    return {};
  }

  return {};
}

std::expected<InputStep, InputError> InputDecoder::pull(std::string_view bytes, InputTime now, bool ending) {
  if (last_time_ && now < *last_time_) {
    return std::unexpected(InputError::backward_time);
  }

  if (!ending && ending_) {
    return std::unexpected(InputError::closed);
  }

  if (state_ == State::failed) {
    return std::unexpected(InputError::failed);
  }

  // Preflight the maximum prefix this call could consume, before any mutation.
  if (std::min(bytes.size(), options_.work_bytes) > std::numeric_limits<std::uint64_t>::max() - offset_) {
    return std::unexpected(InputError::offset_overflow);
  }

  last_time_ = now;
  ending_ = ending_ || ending;
  InputStep result;

  // Deliver already-determined events and expire old prefixes before accepting bytes
  // with this timestamp. Waiting or chunking must not turn a timed-out ESC into Alt.
  if (pending_) {
    result.event = std::move(pending_);
    pending_.reset();
  } else {
    result.event = expire(now);
  }

  if (result.event) {
    result.status = state_ == State::failed ? InputStatus::failed : InputStatus::event;

    return result;
  }

  if (ending) {
    if (!finished_ && state_ != State::ground) {
      if (state_ == State::utf8 && alt_) {
        auto error = issue(InputIssueKind::truncated);
        ++error.begin;
        pending_ = error;
        clear_sequence();
        result.event = KeyEvent{InputKey::escape};
        result.status = InputStatus::event;

        return result;
      } else if (state_ == State::escape) {
        result.event = KeyEvent{InputKey::escape};
      } else {
        result.event = issue(InputIssueKind::truncated);
      }

      clear_sequence();
      std::string{}.swap(paste_);
    }

    finished_ = true;
    result.status = InputStatus::finished;

    return result;
  }

  try {
    while (result.consumed < bytes.size() && result.consumed < options_.work_bytes) {
      bool consume = true;
      ++offset_;
      result.event = byte(static_cast<unsigned char>(bytes[result.consumed]), consume, now);

      if (consume) {
        ++result.consumed;
      } else {
        --offset_;
      }

      if (!result.event) {
        result.event = expire(now);
      }

      if (result.event) {
        result.status = state_ == State::failed ? InputStatus::failed : InputStatus::event;

        return result;
      }
    }
  } catch (...) {
    state_ = State::failed;
    std::string{}.swap(paste_);
    throw;
  }

  result.status = result.consumed < bytes.size() ? InputStatus::work_limit : InputStatus::need_input;

  return result;
}

std::expected<InputStep, InputError> InputDecoder::next(std::string_view bytes, InputTime now) {
  return pull(bytes, now, false);
}

std::expected<InputStep, InputError> InputDecoder::finish(InputTime now) {
  return pull({}, now, true);
}

} // namespace portals
