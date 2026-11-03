// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_radio_group.hpp>

static_assert(requires(portals::ui::Ref<portals::RadioGroup> ref) {
  portals::ui::radioGroup(ref, {"One", "Two"});
  ref.value();
  ref.select(std::nullopt);
});
