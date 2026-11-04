// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/window_source_model.hpp>

static_assert(std::derived_from<portals::FiniteWindowProvider<int>, portals::WindowProvider<int>>);
