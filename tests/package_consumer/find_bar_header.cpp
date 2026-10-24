// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/find_bar.hpp>

static_assert(std::derived_from<portals::FindBar, portals::Column>);
static_assert(std::is_nothrow_copy_constructible_v<portals::FindMatch>);
