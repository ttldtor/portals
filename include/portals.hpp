// Copyright (c) 2024 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#pragma once

#ifdef WIN32
#  include <windows.h>
#endif

namespace org::ttldtor {

#ifdef WIN32
inline bool enableVirtualTerminal(bool enable) {
  auto handle = GetStdHandle(STD_OUTPUT_HANDLE);

  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
    return false;
  }

  DWORD mode = {};

  if (GetConsoleMode(handle, &mode) == 0) {
    return false;
  }

  auto enabled = (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) == ENABLE_VIRTUAL_TERMINAL_PROCESSING;

  if (enable == enabled) {
    return true;
  }

  if (SetConsoleMode(handle, mode ^ ENABLE_VIRTUAL_TERMINAL_PROCESSING) == 0) {
    return false;
  }

  return true;
}
#else
inline bool enableVirtualTerminal(bool) {
  return true;
}
#endif

}  // namespace org::ttldtor