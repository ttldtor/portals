// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include "examples/replay/terminal.hpp"
#include <portals/input_match.hpp>
#include <portals/ui_style.hpp>

#include <iostream>
#include <thread>

namespace stylePssConsumer {
using namespace portals;
namespace sc = style::ct;
inline constexpr ThemeTokens SECOND{{255, 255, 85}, {0, 0, 170}, {0, 170, 170}, {170, 170, 170}, {}, {255, 255, 255}};
inline constexpr auto APPLICATION = sc::pss("#action { bold: true; foreground: #55aaff; }", "application.pss");
inline constexpr auto USER = sc::pss("#action { bold: false; foreground: #ffd246; }", "user.pss");
inline constexpr auto THEME =
  sc::sheet(sc::parameters(sc::parameter<"background">(RgbColor{24, 28, 36}), sc::parameter<"width">(std::int32_t{2})),
            sc::rule(sc::selector(), sc::background(sc::token<"background">())),
            sc::rule(sc::selector<"", "action">(), sc::minWidth(sc::token<"width">())));
using Theme = std::remove_cv_t<decltype(THEME)>;

/** @param error Original managed envelope.
 * @param cause Required underlying tree boundary failure.
 * @return Whether the original cause matches without conversion.
 */
inline bool treeCause(const ManagedError &error, TreeError cause) {
  const auto *value = std::get_if<TreeError>(&error.cause);

  return value && *value == cause;
}

// This complete replay/check harness is included in both measured client footprints.
/** @tparam Client Concrete before/after setup and live observation, defined in its own source file.
 * @param argc Number of arguments including the executable name.
 * @param argv Accepts only the optional intentional negative-width setup scenario.
 * @return Zero after matching UI, exact errors, thread affinity and cleanup checks.
 */
template <class Client> int run(int argc, char **argv) {
  const bool failure = argc == 2 && std::string_view(argv[1]) == "--failure";

  if (argc > 2 || (argc == 2 && !failure)) {
    std::cerr << "Usage: style PSS consumer [--failure]\n";

    return 1;
  }

  example::ReplayTerminal terminal({{50, "\r", {}}, {100, "\r", {}}, {150, "\r", CellSize{38, 8}}}, 200, {40, 10});
  ui::SceneOptions options;
  ui::Ref<Button> button;
  Client client;
  std::size_t changes{};
  bool checks = true;

  options.clock = terminal.clock();

  // The checked no-argument description factory does not capture a moving Scene.
  auto factory = [&]() -> std::expected<ui::Description<Column>, ManagedError> {
    return ui::column({.gap = 1, .padding = {1, 1, 1, 1}}, ui::label("Style PSS"),
                      ui::button("Change").id("action").reference(button));
  };
  auto scene = ui::scene(
    [&](ManagedScene &) {
      return factory();
    },
    options);

  client.install(scene);
  scene.configure([&](ManagedScene &owner) -> std::expected<void, ManagedError> {
    if (failure) {
      return client.width(-1).transform([](bool) {
      });
    }

    const auto initial = client.value();
    const auto equal = client.width(2);
    bool wrongThread{};

    std::thread worker([&] {
      const auto value = client.value();

      wrongThread = !value && treeCause(value.error(), TreeError::WRONG_THREAD);
    });
    worker.join();
    checks = initial && *initial == 2 && equal && !*equal && wrongThread && terminal.output.empty();

    return owner.onInput([&](const InputEvent &event) -> std::expected<bool, ManagedError> {
      if (!keyPressed(event, InputKey::ENTER)) {
        return false;
      }

      if (changes == 0) {
        return client.width(12).transform([&](bool changed) {
          checks = checks && changed;
          ++changes;

          return true;
        });
      }

      if (changes == 1) {
        return owner.theme(themeFromTokens(SECOND)).transform([&] {
          const auto stale = client.value();

          checks = checks && !stale && std::holds_alternative<StyleError>(stale.error().cause);
          ++changes;

          return true;
        });
      }

      // A theme handoff revokes the previous lease. Reattachment is explicit and uses a fresh binding.
      return client.attach(scene).transform([&] {
        const auto reset = client.value();
        const auto equal = client.width(2);

        checks = checks && reset && *reset == 2 && equal && !*equal;
        ++changes;

        return true;
      });
    });
  });

  const auto result = scene.run(terminal);
  const auto closed = client.value();

  if (failure) {
    const auto *cause = result ? nullptr : std::get_if<StyleError>(&result.error().cause);

    if (!cause || cause->cause != TreeError::INVALID_ARGUMENT || result.error().phase != ManagedPhase::STYLE ||
        button.text().get() || closed || !treeCause(closed.error(), TreeError::CLOSED)) {
      std::cerr << "Negative-width typed cause or cleanup differs\n";

      return 1;
    }

    std::cout << "negative-width phase=STYLE cause=INVALID_ARGUMENT cleaned=1\n";

    return 0;
  }

  if (!result || !checks || !terminal.closed || changes != 3 || button.text().get() || closed ||
      !treeCause(closed.error(), TreeError::CLOSED)) {
    std::cerr << "Replay, live authority, affinity or cleanup differs\n";

    return 1;
  }

  std::cout << "frames=" << result->frames << " changes=" << changes << '\n' << terminal.output;

  return 0;
}
} // namespace stylePssConsumer
