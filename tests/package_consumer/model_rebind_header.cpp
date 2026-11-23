// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/managed_navigation.hpp>

bool modelRebindHeaderProbe() {
  const portals::ModelRebindOptions options;
  const portals::NavigationSelection selection;

  return options.restore == portals::ModelRestorePolicy::REMEMBER && selection.rows().empty() &&
         selection.resolution() == portals::PositionResolution::EMPTY;
}
