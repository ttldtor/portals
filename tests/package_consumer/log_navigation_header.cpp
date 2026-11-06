// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/log_navigation.hpp>

bool logNavigationHeaderIsIndependent() {
  portals::LogNavigation navigation;
  const auto status = navigation.status();

  return !status && status.error() == portals::SourceError::CLOSED;
}
