// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_static.hpp>

namespace {
using namespace portals;

// Static-only consumers retain the dynamic UI API historically supplied by this header.
static_assert(std::same_as<decltype(ui::label("x") | ui::classes("x")), ui::Description<Label>>);
static_assert(std::same_as<decltype(ui::button("x") | ui::chrome(ButtonChrome::NONE)), ui::Description<Button>>);
static_assert(std::same_as<decltype(ui::chart(std::declval<ManagedModel<int>>(),
                                            std::declval<ManagedChartAccessor<int>>())), ui::Description<Chart<int>>>);
static_assert(std::same_as<decltype(ui::dialog("x", ui::label("body"))), ui::Description<Dialog>>);
static_assert(std::same_as<decltype(ui::password()), ui::Description<Password>>);
static_assert(std::same_as<decltype(ui::resource("{}")), ui::Description<Widget>>);
static_assert(std::same_as<decltype(ui::scene(ui::SceneResourceFile{})), ui::Scene>);
} // namespace
