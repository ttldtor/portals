// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/window_source.hpp>

static_assert(std::is_nothrow_copy_constructible_v<portals::WindowPage<int>>);
static_assert(std::is_nothrow_copy_constructible_v<portals::WindowPin<int>>);
static_assert(std::is_nothrow_copy_constructible_v<portals::WindowRequest<int>>);
static_assert(std::is_nothrow_copy_constructible_v<portals::SourceReadReply<int>>);
static_assert(std::is_nothrow_copy_constructible_v<portals::WindowNotifier<int>>);
static_assert(sizeof(portals::SourceCursor) == 32);
