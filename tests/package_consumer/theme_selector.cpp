// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/theme_selector.hpp>

#include <portals/ui.hpp>

#include "tests/support/examples/replay/terminal.hpp"

#include <iostream>

namespace {
/** @return True when failed post-commit delivery preserves its owning result and closes the live view. */
bool callbackFailure() {
  using namespace portals;
  example::ReplayTerminal terminal(
    {{100, "\x0b", {}}, {200, "\x14", {}}, {300, "\x1b[B", {}}, {400, "\r", {}}, {500, "\x1b[21~", {}}}, 1500,
    {80, 24});
  ui::SceneOptions runner;
  runner.clock = terminal.clock();
  ui::ThemeSelectionRef themes;
  ui::ThemeSelectorRef selector;
  ui::Ref<Dialog> view;
  std::size_t callbacks{}, reports{};
  bool closedBeforeQuit{};
  ManagedError failure{
    ManagedPhase::STYLE, {}, StyleError{.message = "persistence rejected"}, {}, TreeError::STALE_NODE};
  failure.operation = "consumer persistence";
  ui::ThemeSelectorOptions options;
  options.onCommitted = [&](std::string_view) -> std::expected<void, ManagedError> {
    ++callbacks;
    return std::unexpected(failure);
  };
  auto scene = ui::scene(ui::overlayHost(ui::button("Themes",
                                                    [selector] {
                                                      return selector.open();
                                                    }),
                                         ui::themeSelector(selector, themes, options).reference(view)),
                         runner);
  // clang-format off
  scene
    | ui::themeSelection(themes, {{"nord", "Nord 🙂é界", nordTheme}})
    | ui::onKey("F10", ui::quit);
  // clang-format on
  scene.onError([&](const ManagedDiagnostic &diagnostic) {
    ++reports;
    const auto opened = view.opened();
    closedBeforeQuit = opened && !*opened && diagnostic.error.phase == failure.phase &&
                       diagnostic.error.cleanup == failure.cleanup && diagnostic.error.operation == failure.operation;
  });

  const auto ran = scene.run(terminal);
  const auto applied = selector.lastApply();
  const auto error = applied && *applied ? (*applied)->callbackError : std::optional<ManagedError>{};
  const auto cause = error ? std::get_if<StyleError>(&error->cause) : nullptr;

  return ran && terminal.closed && callbacks == 1 && reports == 1 && closedBeforeQuit && applied && *applied &&
         (*applied)->committedId == "nord" &&
         (*applied)->closeDisposition == ui::ThemeSelectorCloseDisposition::CLOSED && !(*applied)->closeError &&
         error && error->phase == failure.phase && error->cleanup == failure.cleanup &&
         error->operation == failure.operation && cause && cause->message == "persistence rejected" && !selector.open();
}
} // namespace

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
  // clang-format off
  scene
    | ui::themeSelection(themes, {{"nord", "Nord 🙂é界",
                                 [&] {
                                   ++factories;
                                   return nordTheme();
                                 }},
                                {"latte", "Latte", catppuccinLatteTheme}})
    | ui::onKey("F10", ui::quit);
  // clang-format on
  const auto ran = scene.run(terminal);
  const auto applied = selector.lastApply();
  const auto closed = selector.open();
  if (!ran || !terminal.closed || factories != 2 || callbacks != 1 || !applied || !*applied ||
      (*applied)->committedId != "nord" || (*applied)->callbackError || (*applied)->closeError ||
      (*applied)->closeDisposition != ui::ThemeSelectorCloseDisposition::CLOSED || closed) {
    std::cerr << "installed theme selector routing/result/cleanup contract failed\n";
    return 1;
  }
  if (!callbackFailure()) {
    std::cerr << "installed theme selector callback-failure outcome/close contract failed\n";
    return 1;
  }
  return 0;
} catch (const std::exception &failure) {
  std::cerr << failure.what() << '\n';
  return 1;
}
