// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/navigation_checkpoint.hpp>

#include <array>

int main() {
  const std::array<std::byte, 1> corrupt{};
  auto value = portals::NavigationCheckpoint::parse(corrupt);
  if (value || value.error() != portals::NavigationCheckpointError::CORRUPT_INPUT) {
    return 1;
  }

  std::stop_source cancelled;
  cancelled.request_stop();
  auto stopped = portals::NavigationCheckpoint::parse(corrupt, {}, cancelled.get_token());

  return !stopped && stopped.error() == portals::NavigationCheckpointError::CANCELLED ? 0 : 2;
}
