// Copyright (c) 2024 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#pragma once

#include <cmath>
#include <concepts>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

#ifdef WIN32
#  include <windows.h>
#endif

namespace org::ttldtor {

namespace detail {
template <typename...>
struct MaxImpl;

template <typename T>
struct MaxImpl<T> {
  using Type = T;
};

template <typename T, typename U>
struct MaxImpl<T, U> {
  using Type = std::conditional_t<sizeof(T) >= sizeof(U), T, U>;
};

template <typename T, typename U, typename V, typename... Ws>
struct MaxImpl<T, U, V, Ws...> {
  using Type = typename MaxImpl<T, typename MaxImpl<U, typename MaxImpl<V, Ws...>::Type>::Type>::Type;
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
  inline static bool enable() {
    auto handle = GetStdHandle(STD_OUTPUT_HANDLE);

    // Check the validity of the handle.
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
      return false;
    }

    DWORD mode{};

    if (!GetConsoleMode(handle, &mode)) {
      return false;
    }

    auto isEnabled = bit_ops::bitsAreSet(mode, ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    if (isEnabled) {
      return true;  // The mode is already set.
    }

    if (!SetConsoleMode(handle, bit_ops::setBits(mode, ENABLE_VIRTUAL_TERMINAL_PROCESSING))) {
      return false;
    }

    return true;
  }

  inline static bool disable() {
    auto handle = GetStdHandle(STD_OUTPUT_HANDLE);

    // Check the validity of the handle.
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
      return false;
    }

    DWORD mode{};

    if (!GetConsoleMode(handle, &mode)) {
      return false;
    }

    auto isEnabled = bit_ops::bitsAreSet(mode, ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    if (!isEnabled) {
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

}  // namespace org::ttldtor