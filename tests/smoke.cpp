// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0

#include <portals/version.hpp>

#include <cstdlib>
#include <iostream>

int main() {
  const portals::Version actual = portals::version();

  if (actual.major != 0 || actual.minor != 1 || actual.patch != 0) {
    std::cerr << "Expected Portals 0.1.0, got " << actual.major << '.' << actual.minor << '.' << actual.patch << '\n';

    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
