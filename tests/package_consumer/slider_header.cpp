// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/slider.hpp>

static_assert(requires(portals::Slider &slider, portals::PaintContext &context) {
  slider.measure({});
  slider.paint(context);
  slider.value();
  slider.range();
  slider.readOnly();
});
