// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/help_view.hpp>
static_assert(std::derived_from<portals::HelpView, portals::Column>);
static_assert(std::is_copy_constructible_v<portals::HelpViewOptions>);
