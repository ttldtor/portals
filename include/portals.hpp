// Copyright (c) 2025 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cmath>
#include <concepts>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

#ifdef WIN32
#  include <windows.h>
#endif

namespace org::ttldtor {

namespace detail {
template <typename T, typename U>
using Gt = std::conditional_t<sizeof(T) >= sizeof(U), T, U>;

template <typename T, typename...>
struct MaxImpl {
  using Type = T;
};

template <typename T, typename U, typename... Ts>
struct MaxImpl<T, U, Ts...> {
  using Type = typename MaxImpl<Gt<T, U>, Ts...>::Type;
};
}  // namespace detail

/**
 * Returns max type by size (first is better)
 */
template <typename... Ts>
using Max = typename detail::MaxImpl<Ts...>::Type;

namespace bit_ops {

template <std::unsigned_integral T>
static constexpr T bitsAreSet(T sourceBits, T bitMaskToCheck) {
  return (sourceBits & bitMaskToCheck) != 0;
}

template <std::integral SB, std::integral M>
static constexpr SB bitsAreSet(SB sourceBits, M bitMaskToCheck) {
  using MaxType = Max<SB, M>;

  if constexpr (std::is_signed_v<SB> || std::is_signed_v<M>) {
    using U = std::make_unsigned_t<MaxType>;

    return static_cast<SB>(bitsAreSet(static_cast<U>(sourceBits), static_cast<U>(bitMaskToCheck)));
  } else {
    return static_cast<SB>(bitsAreSet(static_cast<MaxType>(sourceBits), static_cast<MaxType>(bitMaskToCheck)));
  }
}

template <std::unsigned_integral T>
static constexpr T setBits(T sourceBits, T bitMaskToSet) {
  return sourceBits | bitMaskToSet;
}

template <std::integral SB, std::integral M>
static constexpr SB setBits(SB sourceBits, M bitMaskToSet) {
  using MaxType = Max<SB, M>;

  if constexpr (std::is_signed_v<SB> || std::is_signed_v<M>) {
    using U = std::make_unsigned_t<MaxType>;

    return static_cast<SB>(setBits(static_cast<U>(sourceBits), static_cast<U>(bitMaskToSet)));
  } else {
    return static_cast<SB>(setBits(static_cast<MaxType>(sourceBits), static_cast<MaxType>(bitMaskToSet)));
  }
}

template <std::unsigned_integral T>
static constexpr T resetBits(T sourceBits, T bitMaskToReset) {
  return sourceBits & ~bitMaskToReset;
}

template <std::integral SB, std::integral M>
// ReSharper disable once CppDFAConstantParameter
static constexpr SB resetBits(SB sourceBits, M bitMaskToReset) {
  using MaxType = Max<SB, M>;

  if constexpr (std::is_signed_v<SB> || std::is_signed_v<M>) {
    using U = std::make_unsigned_t<MaxType>;

    return static_cast<SB>(resetBits(static_cast<U>(sourceBits), static_cast<U>(bitMaskToReset)));
  } else {
    return static_cast<SB>(resetBits(static_cast<MaxType>(sourceBits), static_cast<MaxType>(bitMaskToReset)));
  }
}

}  // namespace bit_ops

// Windows-specific structure for terminal management.
struct VirtualTerminal {
#ifdef WIN32
  static bool enable() {
    const auto handle = GetStdHandle(STD_OUTPUT_HANDLE);

    // Check the validity of the handle.
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
      return false;
    }

    DWORD mode{};

    if (!GetConsoleMode(handle, &mode)) {
      return false;
    }

    if (bit_ops::bitsAreSet(mode, ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
      return true;  // The mode is already set.
    }

    if (!SetConsoleMode(handle, bit_ops::setBits(mode, ENABLE_VIRTUAL_TERMINAL_PROCESSING))) {
      return false;
    }

    return true;
  }

  static bool disable() {
    const auto handle = GetStdHandle(STD_OUTPUT_HANDLE);

    // Check the validity of the handle.
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
      return false;
    }

    DWORD mode{};

    if (!GetConsoleMode(handle, &mode)) {
      return false;
    }

    if (const auto isEnabled = bit_ops::bitsAreSet(mode, ENABLE_VIRTUAL_TERMINAL_PROCESSING); !isEnabled) {
      return true;  // The mode is already set.
    }

    if (!SetConsoleMode(handle, bit_ops::resetBits(mode, ENABLE_VIRTUAL_TERMINAL_PROCESSING))) {
      return false;
    }

    return true;
  }

#else
  inline static bool enable() {
    return true;
  }

  inline static bool disable() {
    return true;
  }
#endif
};

namespace portals {
static constexpr const char* ESC = "\x1b";
static constexpr const char* SAVE_CURSOR_POS = "[s";

enum class Color : int {
  RESET = 0,
  BLACK = 30,
  RED = 31,
  GREEN = 32,
  YELLOW = 33,
  BLUE = 34,
  MAGENTA = 35,
  CYAN = 36,
  WHITE = 37,
  DEFAULT = 39,
  BRIGHT_BLACK = 90,
  BRIGHT_RED = 91,
  BRIGHT_GREEN = 92,
  BRIGHT_YELLOW = 93,
  BRIGHT_BLUE = 94,
  BRIGHT_MAGENTA = 95,
  BRIGHT_CYAN = 96,
  BRIGHT_WHITE = 90,
};

enum class BgColor : int {
  RESET = 0,
  BLACK = 40,
  RED = 41,
  GREEN = 42,
  YELLOW = 43,
  BLUE = 44,
  MAGENTA = 45,
  CYAN = 46,
  WHITE = 47,
  DEFAULT = 49,
  BRIGHT_BLACK = 100,
  BRIGHT_RED = 101,
  BRIGHT_GREEN = 102,
  BRIGHT_YELLOW = 103,
  BRIGHT_BLUE = 104,
  BRIGHT_MAGENTA = 105,
  BRIGHT_CYAN = 106,
  BRIGHT_WHITE = 107,
};

static std::string saveCursorPos() {
  return std::string(ESC) + SAVE_CURSOR_POS;
}

static std::string restoreCursorPos() {
  return std::string{} + ESC + "[u";
}

static std::string hideCursor() {
  return std::string{} + ESC + "[?25l";
}

static std::string showCursor() {
  return std::string{} + ESC + "[?25h";
}

static std::string moveToColumn(std::size_t column) {
  return std::string{} + ESC + "[" + std::to_string(column) + "G";
}

static std::string moveToLineBegin() {
  return moveToColumn(0);
}

static std::string moveTo(std::size_t line, std::size_t column) {
  return std::string{} + ESC + "[" + std::to_string(line) + ";" + std::to_string(column) + "H";
}

static std::string writeTo(std::size_t line, std::size_t column, const std::string& text) {
  return moveTo(line, column) + text;
}

static std::string setColor(Color color) {
  return std::string{} + ESC + "[1;" + std::to_string(static_cast<int>(color)) + "m";
}

static std::string setBgColor(BgColor bgColor) {
  return std::string{} + ESC + "[1;" + std::to_string(static_cast<int>(bgColor)) + "m";
}

static std::string setColor(Color color, BgColor bgColor) {
  return std::string{} + ESC + "[1;" + std::to_string(static_cast<int>(color)) + ";" +
         std::to_string(static_cast<int>(bgColor)) + "m";
}

static std::string setColor(std::uint8_t colorIndex) {
  return std::string{} + ESC + "[38;5;" + std::to_string(colorIndex) + "m";
}

static std::string setBgColor(std::uint8_t bgColorIndex) {
  return std::string{} + ESC + "[48;5;" + std::to_string(bgColorIndex) + "m";
}

static std::string setColor(std::uint8_t colorIndex, std::uint8_t bgColorIndex) {
  return setColor(colorIndex) + setBgColor(bgColorIndex);
}

static std::string setColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
  return std::string{} + ESC + "[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
}

static std::string setBgColor(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
  return std::string{} + ESC + "[48;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
}

static std::string setColor(const std::tuple<std::uint8_t, std::uint8_t, std::uint8_t>& colorRgb,
                            const std::tuple<std::uint8_t, std::uint8_t, std::uint8_t>& bgColorRgb) {
  return setColor(std::get<0>(colorRgb), std::get<1>(colorRgb), std::get<2>(colorRgb)) +
         setBgColor(std::get<0>(bgColorRgb), std::get<1>(bgColorRgb), std::get<2>(bgColorRgb));
}

static std::string resetFormat() {
  return std::string{} + ESC + "[0m";
}

}  // namespace portals

}  // namespace org::ttldtor