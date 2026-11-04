// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_spin_box.hpp>
static_assert(requires(portals::ui::Ref<portals::SpinBox> spin) {
  portals::ui::spinBox(spin, 2).range(0, 10).step(0.5).pageSteps(4).editMode(portals::EditMode::EXPLICIT);
  spin.lineEdit();
  spin.text();
  spin.value().get();
  spin.value().set(2).result();
});
