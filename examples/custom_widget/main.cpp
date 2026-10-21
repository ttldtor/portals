// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../gallery/options.hpp"
#include "fluent_meter.hpp"

/** @brief Parse CLI before mounting the custom widget; the runner reports checked errors. */
int main(int argc, char **argv) {
  using namespace portals;

  example::Options options;
  auto lyraCli = example::commandLine(options);

  // clang-format off
  return [&] {
    return meter_example::meter(ui::opts(options));
  }
    | ui::runApp(argc, argv, ui::lyra::cli(lyraCli));
  // clang-format on
}
