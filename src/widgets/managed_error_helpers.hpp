// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/managed_diagnostics.hpp>

namespace portals {
namespace {
inline bool retired(TreeError error) {
  return error == TreeError::STALE_NODE || error == TreeError::CLOSED;
}

inline TreeError bridge(const ManagedError &error) {
  if (const auto *tree = std::get_if<TreeError>(&error.cause)) {
    return *tree;
  }

  if (const auto *style = std::get_if<StyleError>(&error.cause)) {
    return style->cause;
  }

  return TreeError::CALLBACK_FAILED;
}

} // namespace
} // namespace portals
