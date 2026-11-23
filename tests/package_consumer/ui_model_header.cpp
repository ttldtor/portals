// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_model.hpp>

#include <array>
#include <concepts>

static_assert(std::same_as<decltype(portals::ui::model(std::array{1, 2, 3})), portals::ui::Model<int>>);
