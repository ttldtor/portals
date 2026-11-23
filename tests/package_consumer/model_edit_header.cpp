// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/model_edit.hpp>

#include <concepts>
#include <expected>

static_assert(std::same_as<decltype(portals::EditFieldKey::create("city.name")),
                           std::expected<portals::EditFieldKey, portals::TreeError>>);
