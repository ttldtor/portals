// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/status_bar.hpp>

#include <type_traits>

static_assert(std::is_base_of_v<portals::Row, portals::StatusBar>);
static_assert(!std::is_base_of_v<portals::Control, portals::StatusBar>);

void statusBarHeaderConsumer() {
  portals::StatusBar bar;
  (void)bar.sizePolicy();
}
