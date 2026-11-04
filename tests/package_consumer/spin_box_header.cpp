// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/spin_box.hpp>
static_assert(requires(portals::SpinBox &spin) {
  spin.editor();
  spin.sizePolicy();
});
