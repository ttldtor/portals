// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_slider.hpp>

static_assert(requires(portals::ui::Ref<portals::Slider> reference) {
  portals::ui::slider();
  portals::ui::slider(reference, {});
  portals::ui::slider(reference, 0, {});
  portals::ui::slider(0).range(-1, 1).step(0.1);
  reference.value().get();
  reference.value().set(0).result();
});
