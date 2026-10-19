// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/key_binding.hpp>
static_assert(std::is_default_constructible_v<portals::KeyBinding>);
static_assert(std::is_copy_constructible_v<portals::KeyBindingsSnapshot>);
static_assert(!std::is_default_constructible_v<portals::KeyBindingView>);
static_assert(std::is_default_constructible_v<portals::KeyBindingsSnapshot>);
static_assert(std::same_as<portals::KeyTextResolver,
                           std::function<std::expected<std::string, portals::TextResourceError>(std::string_view)>>);
