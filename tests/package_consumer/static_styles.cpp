// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "static_styles_support.hpp"
#include <portals/style_static.hpp>

using namespace staticStylesConsumer;
namespace sc = style::ct;
constexpr auto THEME =
  sc::themeFromTokens(FIRST, sc::parameters(sc::parameter<"width">(std::int32_t{2})),
                      sc::rule(sc::selector(StyleState::SELECTED),
                               sc::background(sc::mix(sc::token<"background">(), sc::rgb(255, 255, 255), 20))),
                      sc::rule(sc::selector<"", "action">(), sc::minWidth(sc::token<"width">())));

struct Controller {
  sc::LiveTheme<std::remove_cv_t<decltype(THEME)>> handle;

  auto palette(ThemeTokens values) {
    return handle.apply(sc::change<"foreground">(values.foreground), sc::change<"background">(values.background),
                        sc::change<"accent">(values.accent), sc::change<"muted">(values.muted),
                        sc::change<"heading">(values.heading));
  }

  auto background(RgbColor value) {
    return handle.set<"background">(value);
  }

  auto width(std::int32_t value) {
    return handle.set<"width">(value);
  }
};

int main(int argc, char **argv) {
  return run(argc, argv, [](ManagedScene &scene) -> std::expected<Controller, ManagedError> {
    auto handle = sc::live(scene, THEME);

    return handle ? std::expected<Controller, ManagedError>(Controller{*handle}) : std::unexpected(handle.error());
  });
}
