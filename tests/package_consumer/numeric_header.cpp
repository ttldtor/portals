// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/numeric.hpp>

static_assert(requires(const portals::NumericValue &value) {
  portals::NumericValue::create(0);
  value.value();
  value.range();
  value.withValue(0.25);
});
