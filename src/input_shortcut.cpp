// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/shortcut.hpp>

#include <algorithm>
#include <array>
#include <new>

namespace portals {
namespace {
constexpr unsigned char actionBit(KeyAction action) noexcept {
  switch (action) {
  case KeyAction::PRESS:
    return static_cast<unsigned char>(detail::ShortcutActions::PRESS);
  case KeyAction::REPEAT:
    return static_cast<unsigned char>(detail::ShortcutActions::REPEAT);
  case KeyAction::RELEASE:
    return static_cast<unsigned char>(detail::ShortcutActions::RELEASE);
  }

  return 0;
}

constexpr bool asciiSpace(char value) noexcept {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' || value == '\v';
}

constexpr char32_t asciiUpper(char32_t value) noexcept {
  return value >= U'a' && value <= U'z' ? value - (U'a' - U'A') : value;
}

bool equalName(std::string_view token, std::string_view name) noexcept {
  return token.size() == name.size() && std::ranges::equal(token, name, [](char left, char right) {
           return asciiUpper(static_cast<unsigned char>(left)) == static_cast<unsigned char>(right);
         });
}

std::optional<key::Chord> namedKey(std::string_view token) noexcept {
  struct Named {
    std::string_view name;
    InputKey value;
  };

  static constexpr std::array NAMES{Named{"ESCAPE", InputKey::ESCAPE},
                                    Named{"ESC", InputKey::ESCAPE},
                                    Named{"ENTER", InputKey::ENTER},
                                    Named{"TAB", InputKey::TAB},
                                    Named{"BACKSPACE", InputKey::BACKSPACE},
                                    Named{"UP", InputKey::UP},
                                    Named{"DOWN", InputKey::DOWN},
                                    Named{"RIGHT", InputKey::RIGHT},
                                    Named{"LEFT", InputKey::LEFT},
                                    Named{"HOME", InputKey::HOME},
                                    Named{"END", InputKey::END},
                                    Named{"INSERT", InputKey::INSERT},
                                    Named{"DELETE", InputKey::DELETE_KEY},
                                    Named{"DELETE_KEY", InputKey::DELETE_KEY},
                                    Named{"PAGEUP", InputKey::PAGE_UP},
                                    Named{"PAGE_UP", InputKey::PAGE_UP},
                                    Named{"PAGEDOWN", InputKey::PAGE_DOWN},
                                    Named{"PAGE_DOWN", InputKey::PAGE_DOWN},
                                    Named{"F1", InputKey::F1},
                                    Named{"F2", InputKey::F2},
                                    Named{"F3", InputKey::F3},
                                    Named{"F4", InputKey::F4},
                                    Named{"F5", InputKey::F5},
                                    Named{"F6", InputKey::F6},
                                    Named{"F7", InputKey::F7},
                                    Named{"F8", InputKey::F8},
                                    Named{"F9", InputKey::F9},
                                    Named{"F10", InputKey::F10},
                                    Named{"F11", InputKey::F11},
                                    Named{"F12", InputKey::F12}};

  if (token.size() == 1) {
    const auto scalar = asciiUpper(static_cast<unsigned char>(token.front()));

    if ((scalar >= U'A' && scalar <= U'Z') || (scalar >= U'0' && scalar <= U'9')) {
      return key::Chord{InputKey::CHARACTER, scalar};
    }
  }

  for (const auto &name : NAMES) {
    if (equalName(token, name.name)) {
      return key::Chord{name.value};
    }
  }

  return {};
}

bool validOptions(ShortcutOptions options) noexcept {
  return options.maxSourceBytes && options.maxAlternatives && options.maxStrokes;
}

std::expected<key::Chord, ShortcutError> parseAlternative(std::string_view source, std::size_t begin, std::size_t end,
                                                          ShortcutOptions options) {
  std::size_t cursor = begin;
  std::size_t strokes{};
  std::size_t secondStroke{};
  InputModifiers modifiers;
  key::Chord first;
  bool commaNeedsKey{};

  while (cursor < end && asciiSpace(source[cursor])) {
    ++cursor;
  }

  if (cursor == end) {
    return std::unexpected(ShortcutError{ShortcutErrorKind::EMPTY_ALTERNATIVE, cursor});
  }

  while (true) {
    while (cursor < end && asciiSpace(source[cursor])) {
      ++cursor;
    }

    const auto tokenStart = cursor;

    while (cursor < end && source[cursor] != '+' && source[cursor] != ',') {
      ++cursor;
    }

    auto tokenEnd = cursor;

    while (tokenEnd > tokenStart && asciiSpace(source[tokenEnd - 1])) {
      --tokenEnd;
    }

    if (tokenStart == tokenEnd) {
      return std::unexpected(ShortcutError{ShortcutErrorKind::EMPTY_TOKEN, tokenStart});
    }

    const auto token = source.substr(tokenStart, tokenEnd - tokenStart);
    bool *modifier{};

    if (equalName(token, "SHIFT")) {
      modifier = &modifiers.shift;
    } else if (equalName(token, "ALT")) {
      modifier = &modifiers.alt;
    } else if (equalName(token, "CTRL") || equalName(token, "CONTROL")) {
      modifier = &modifiers.control;
    }

    if (modifier) {
      if (*modifier) {
        return std::unexpected(ShortcutError{ShortcutErrorKind::DUPLICATE_MODIFIER, tokenStart});
      }

      *modifier = true;
    } else {
      auto chord = namedKey(token);

      if (!chord) {
        return std::unexpected(ShortcutError{ShortcutErrorKind::UNKNOWN_KEY, tokenStart});
      }

      ++strokes;
      commaNeedsKey = false;

      if (strokes > options.maxStrokes) {
        return std::unexpected(ShortcutError{ShortcutErrorKind::STROKE_LIMIT, tokenStart});
      }

      if (strokes == 1) {
        first = *chord;
      } else if (strokes == 2) {
        secondStroke = tokenStart;
      }
    }

    if (cursor == end) {
      break;
    }

    if (source[cursor] == ',') {
      if (!strokes || commaNeedsKey) {
        return std::unexpected(ShortcutError{ShortcutErrorKind::EXPECTED_KEY, cursor});
      }

      if (strokes == 1) {
        first.modifiers = modifiers;
      }

      modifiers = {};
      commaNeedsKey = true;
    }

    ++cursor;
  }

  if (!strokes || commaNeedsKey) {
    return std::unexpected(ShortcutError{ShortcutErrorKind::EXPECTED_KEY, end});
  }

  if (strokes > 1) {
    return std::unexpected(ShortcutError{ShortcutErrorKind::UNSUPPORTED_SEQUENCE, secondStroke});
  }

  first.modifiers = modifiers;

  return first;
}
} // namespace

std::expected<void, ShortcutError> validateShortcut(const Shortcut &shortcut, ShortcutOptions options) noexcept {
  if (!validOptions(options)) {
    return std::unexpected(ShortcutError{ShortcutErrorKind::INVALID_OPTIONS, 0});
  }

  if (shortcut.alternatives.empty()) {
    return std::unexpected(ShortcutError{ShortcutErrorKind::EMPTY_ALTERNATIVE, 0});
  }

  if (shortcut.alternatives.size() > options.maxAlternatives) {
    return std::unexpected(ShortcutError{ShortcutErrorKind::ALTERNATIVE_LIMIT, options.maxAlternatives});
  }

  for (std::size_t index{}; index < shortcut.alternatives.size(); ++index) {
    const auto &chord = shortcut.alternatives[index];
    const bool validCharacter = chord.scalar >= U' ' && chord.scalar <= U'~';
    const bool validNamed = chord.key >= InputKey::ESCAPE && chord.key <= InputKey::F12 && chord.scalar == 0;

    if (chord.key == InputKey::CHARACTER ? !validCharacter : !validNamed) {
      return std::unexpected(ShortcutError{ShortcutErrorKind::INVALID_CHORD, index});
    }
  }

  return {};
}

std::expected<Shortcut, ShortcutError> parseShortcut(std::string_view source, ShortcutOptions options) {
  if (!validOptions(options)) {
    return std::unexpected(ShortcutError{ShortcutErrorKind::INVALID_OPTIONS, 0});
  }

  if (source.size() > options.maxSourceBytes) {
    return std::unexpected(ShortcutError{ShortcutErrorKind::SOURCE_LIMIT, options.maxSourceBytes});
  }

  try {
    Shortcut result;
    std::size_t begin{};

    while (true) {
      if (result.alternatives.size() >= options.maxAlternatives) {
        return std::unexpected(ShortcutError{ShortcutErrorKind::ALTERNATIVE_LIMIT, begin});
      }

      const auto separator = source.find('|', begin);
      const auto end = separator == source.npos ? source.size() : separator;
      auto chord = parseAlternative(source, begin, end, options);

      if (!chord) {
        return std::unexpected(chord.error());
      }

      result.alternatives.push_back(*chord);

      if (separator == source.npos) {
        break;
      }

      begin = separator + 1;
    }

    return result;
  } catch (const std::bad_alloc &) {
    return std::unexpected(ShortcutError{ShortcutErrorKind::NO_MEMORY, 0});
  }
}

std::optional<KeyEvent> matchShortcut(const Shortcut &shortcut, const InputEvent &event,
                                      ShortcutBehavior behavior) noexcept {
  return detail::matchShortcutActions(
    shortcut, event, behavior.repeat ? detail::ShortcutActions::PRESS_REPEAT : detail::ShortcutActions::PRESS);
}

std::optional<KeyEvent> detail::matchShortcutActions(const Shortcut &shortcut, const InputEvent &event,
                                                     detail::ShortcutActions actions) noexcept {
  KeyEvent value;

  if (const auto *key = std::get_if<KeyEvent>(&event)) {
    value = *key;
  } else if (const auto *text = std::get_if<TextInputEvent>(&event)) {
    value = {InputKey::CHARACTER, text->scalar, text->modifiers, KeyAction::PRESS};
  } else {
    return {};
  }

  const auto bit = actionBit(value.action);

  if ((static_cast<unsigned>(actions) & bit) == 0) {
    return {};
  }

  for (const auto &chord : shortcut.alternatives) {
    if (value.key == chord.key && value.modifiers == chord.modifiers &&
        (value.key == InputKey::CHARACTER ? asciiUpper(value.scalar) == asciiUpper(chord.scalar)
                                          : value.scalar == chord.scalar)) {
      return value;
    }
  }

  return {};
}

namespace detail {
KeySelection KeySelectionAccess::make(std::expected<Shortcut, ShortcutError> &&shortcut,
                                      std::initializer_list<KeyAction> actions, ShortcutOptions options) {
  unsigned char mask{};
  std::size_t index{};

  for (auto action : actions) {
    const auto bit = actionBit(action);

    if (bit == 0) {
      return KeySelection(std::unexpected(ShortcutError{ShortcutErrorKind::INVALID_ACTION_SET, index}),
                          ShortcutActions::PRESS, options);
    }

    mask |= bit;

    ++index;
  }

  if (mask == 0) {
    return KeySelection(std::unexpected(ShortcutError{ShortcutErrorKind::INVALID_ACTION_SET, 0}),
                        ShortcutActions::PRESS, options);
  }

  auto retained = retainShortcut(std::move(shortcut));

  if (!retained) {
    return KeySelection(std::unexpected(retained.error()), static_cast<ShortcutActions>(mask), options);
  }

  return KeySelection(std::shared_ptr<const Shortcut>(std::move(*retained)), static_cast<ShortcutActions>(mask),
                      options);
}

std::expected<Shortcut, ShortcutError> prepareShortcut(key::Chord chord, ShortcutOptions options) {
  return prepareShortcut({chord}, options);
}

std::expected<Shortcut, ShortcutError> prepareShortcut(std::initializer_list<key::Chord> chords,
                                                       ShortcutOptions options) {
  if (!validOptions(options)) {
    return std::unexpected(ShortcutError{ShortcutErrorKind::INVALID_OPTIONS, 0});
  }

  if (chords.size() > options.maxAlternatives) {
    return std::unexpected(ShortcutError{ShortcutErrorKind::ALTERNATIVE_LIMIT, options.maxAlternatives});
  }

  try {
    return prepareShortcut(Shortcut{chords}, options);
  } catch (const std::bad_alloc &) {
    return std::unexpected(ShortcutError{ShortcutErrorKind::NO_MEMORY, 0});
  }
}

template <class Value>
std::expected<Shortcut, ShortcutError> copyOrMoveShortcut(Value &&shortcut, ShortcutOptions options) {
  auto valid = validateShortcut(shortcut, options);

  if (!valid) {
    return std::unexpected(valid.error());
  }

  try {
    return std::expected<Shortcut, ShortcutError>{std::in_place, std::forward<Value>(shortcut)};
  } catch (const std::bad_alloc &) {
    return std::unexpected(ShortcutError{ShortcutErrorKind::NO_MEMORY, 0});
  }
}

std::expected<Shortcut, ShortcutError> prepareShortcut(const Shortcut &shortcut, ShortcutOptions options) {
  return copyOrMoveShortcut(shortcut, options);
}

std::expected<Shortcut, ShortcutError> prepareShortcut(Shortcut &&shortcut, ShortcutOptions options) {
  return copyOrMoveShortcut(std::move(shortcut), options);
}

std::expected<std::shared_ptr<Shortcut>, ShortcutError>
retainShortcut(std::expected<Shortcut, ShortcutError> &&shortcut) {
  if (!shortcut) {
    return std::unexpected(shortcut.error());
  }

  try {
    return std::make_shared<Shortcut>(std::move(*shortcut));
  } catch (const std::bad_alloc &) {
    return std::unexpected(ShortcutError{ShortcutErrorKind::NO_MEMORY, 0});
  }
}
} // namespace detail
} // namespace portals
