// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/text_value.hpp>

void invalidTextFormat() {
#ifdef PORTALS_DYNAMIC_TEXT_PATTERN
  const std::string pattern = "{}";
  const auto value = portals::text(pattern, 123);
#else
  const auto value = portals::text("{:d}", std::string("invalid"));
#endif

  static_cast<void>(value);
}
