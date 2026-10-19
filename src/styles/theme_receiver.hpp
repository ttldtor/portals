// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/widget.hpp>

namespace portals::detail {
// One weak engine receiver, present only for a retained library selector. Never calls application code.
struct StyleThemeReceiver {
  Node<Widget> node;
  bool visible{};
  bool dirty{true};
  bool retired{};
};
} // namespace portals::detail
