// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/theme_selector.hpp>

#include <portals/ui.hpp>

#include "tests/support/examples/replay/terminal.hpp"

#include <iostream>

int main() try {
  using namespace portals;
  example::ReplayTerminal terminal({{100, "\x0b", {}},
                                    {200, "\x14", {}},
                                    {300, "\x1b[B", {}},
                                    {400, "\r", {}},
                                    {500, "\x0b", {}},
                                    {600, "\x14", {}},
                                    {700, "\x1b[B", {}},
                                    {800, "\x1b", {}},
                                    {900, "\x1b[21~", {}}},
                                   2000, {80, 24});
  ui::SceneOptions runner;
  runner.clock = terminal.clock();
  ui::ThemeSelectionRef themes;
  ui::ThemeSelectorRef selector;
  std::size_t factories{};
  std::size_t callbacks{};
  ui::ThemeSelectorOptions options;
  options.onCommitted = [&](std::string_view id) -> std::expected<void, ManagedError> {
    if (id != "nord") {
      return std::unexpected(ManagedError{ManagedPhase::INPUT, {}, TreeError::INVALID_ARGUMENT, {}, {}});
    }
    ++callbacks;
    return {};
  };
  auto scene = ui::scene(ui::overlayHost(ui::button("Themes",
                                                    [selector] {
                                                      return selector.open();
                                                    }),
                                         ui::themeSelector(selector, themes, options)),
                         runner);
  scene |
    ui::themeSelection(themes, {{"nord", "Nord 🙂é界",
                                 [&] {
                                   ++factories;
                                   return nordTheme();
                                 }},
                                {"latte", "Latte", catppuccinLatteTheme}}) |
    ui::onKey("F10", ui::quit);
  const auto ran = scene.run(terminal);
  const auto applied = selector.lastApply();
  const auto closed = selector.open();
  if (!ran || !terminal.closed || factories != 2 || callbacks != 1 || !applied || !*applied ||
      (*applied)->committedId != "nord" || (*applied)->callbackError || (*applied)->closeError ||
      (*applied)->closeDisposition != ui::ThemeSelectorCloseDisposition::CLOSED || closed) {
    std::cerr << "installed theme selector routing/result/cleanup contract failed\n";
    return 1;
  }
  return 0;
} catch (const std::exception &failure) {
  std::cerr << failure.what() << '\n';
  return 1;
}
