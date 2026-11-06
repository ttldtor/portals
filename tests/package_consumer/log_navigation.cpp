// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/log_file.hpp>
#include <portals/log_navigation.hpp>
#include <portals/managed.hpp>

bool logNavigationHeaderIsIndependent();

int main() {
  const auto model = portals::FileLogModel::create("not-opened-by-construction.log");

  if (!model || !logNavigationHeaderIsIndependent()) {
    return 1;
  }

  const auto resource = (*model)->resource();
  portals::LogNavigation navigation;
  const auto checkpoint = navigation.checkpoint(resource);
  const auto reset = navigation.reset(resource);

  return !checkpoint && checkpoint.error() == portals::SourceError::CLOSED && !reset &&
             reset.error() == portals::SourceError::CLOSED
           ? 0
           : 2;
}
