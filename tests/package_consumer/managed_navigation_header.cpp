// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/managed_navigation.hpp>

bool managedNavigationHeaderProbe() {
  portals::ManagedNavigation navigation;

  return navigation.clearCurrent().error() == portals::TreeError::CLOSED &&
         portals::NavigationOptions{}.maxSelectedRows == 1024;
}
