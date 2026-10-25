// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <portals/text.hpp>

#include <memory>

namespace portals::detail {
/// @cond INTERNAL
struct PreparedListRun {
  GraphemeText text;
  std::int32_t column{};
  bool highlighted{};
};

struct PreparedListText {
  explicit PreparedListText(GraphemeText value) : text(std::move(value)) {
  }

  GraphemeText text;
  std::vector<PreparedListRun> runs = std::vector<PreparedListRun>(std::initializer_list<PreparedListRun>{});
};

/// @endcond
} // namespace portals::detail
