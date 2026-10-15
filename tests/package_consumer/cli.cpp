// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_lyra.hpp>

int main(int argc, char **argv) {
  namespace ui = portals::ui;
  int count{12};
  bool constructed{};
  auto parser = ::lyra::cli() | ::lyra::opt(count, "count")["-n"]["--count"]("Row count.");
  auto scene = ui::scene([&](portals::ManagedScene &) {
    constructed = true;

    return ui::label("CLI consumer");
  });
  const auto result = scene | ui::runApp(argc, argv, ui::lyra::cli(parser, {.version = "installed-cli 1.0"}));

  return result == 0 && !constructed && count == 12 ? 0 : 1;
}
