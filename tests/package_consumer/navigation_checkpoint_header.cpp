// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/navigation_checkpoint.hpp>

#include <type_traits>

static_assert(portals::NavigationCheckpoint::FORMAT_VERSION == 1);
static_assert(std::is_copy_constructible_v<portals::NavigationCheckpoint>);
