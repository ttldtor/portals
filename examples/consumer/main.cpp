// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include <portals/version.hpp>

#include <iostream>

int main() {
  const portals::Version libraryVersion = portals::version();

  std::cout << "Portals " << libraryVersion.major << '.' << libraryVersion.minor << '.' << libraryVersion.patch << '\n';

  return 0;
}
