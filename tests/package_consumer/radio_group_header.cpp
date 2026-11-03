// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/editor_command.hpp>
#include <portals/radio_group.hpp>

static_assert(std::equality_comparable<portals::RadioGroupValue>);
static_assert(std::is_nothrow_copy_constructible_v<portals::RadioGroupValue>);
static_assert(!std::is_move_constructible_v<portals::RadioGroup>);

static_assert(sizeof(portals::RadioGroupBinding) == sizeof(portals::CheckBoxBinding));
static_assert(
  sizeof(std::function<portals::EditorCommandResult(const portals::PropertyUpdate<portals::RadioGroupValue> &)>) ==
  sizeof(std::function<portals::EditorCommandResult(const portals::PropertyUpdate<bool> &)>));
