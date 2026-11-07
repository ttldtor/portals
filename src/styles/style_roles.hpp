// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once
#include <portals/style.hpp>

namespace portals::detail {
// The consumer owns this record; style transactions pin it only while staging.
// No owner callback or borrowed widget address is retained across publication.
struct StyleRoleState {
  const std::thread::id owner = std::this_thread::get_id();
  Node<Widget> node;
  std::weak_ptr<const void> engine;
  std::weak_ptr<const void> participation;
  std::vector<std::string> names = std::vector<std::string>(0);
  std::shared_ptr<const StyleRolePalette> palette;
  std::uint64_t revision{};
  bool active{true};
};
} // namespace portals::detail
