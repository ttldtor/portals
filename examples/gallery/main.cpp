// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "fluent_form.hpp"
#include "options.hpp"

/** @brief Parse CLI before mounting the interactive gallery; the runner reports checked errors. */
int main(int argc, char **argv) {
  using namespace portals;

  example::Options options;
  auto lyraCli = example::commandLine(options);

  return gallery::form(ui::opts(options))
    | ui::reportOnError()
    | ui::runApp(argc, argv, ui::lyra::cli(lyraCli));
}
