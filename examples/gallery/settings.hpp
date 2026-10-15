// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <string>

namespace example {
/** @brief Application settings, populated before lazy scene preparation. */
struct Options {
  std::string theme{"modern"}; ///< Initial preset name; defaults to Modern.
  bool ansi16{};               ///< Use sixteen ANSI colors rather than true color.
};
} // namespace example
