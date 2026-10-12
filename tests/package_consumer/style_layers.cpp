// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "style_layers_support.hpp"

using namespace styleLayersConsumer;

struct Controller {
  ManagedScene *scene;
  sc::LiveTheme<std::remove_cv_t<decltype(THEME)>> live;

  auto switchTheme() {
    return scene->theme(themeFromTokens(staticStylesConsumer::SECOND));
  }

  std::expected<void, ManagedError> attachLive() {
    auto result = sc::live(*scene, THEME);

    if (!result) {
      return std::unexpected(result.error());
    }

    live = *result;

    return {};
  }

  std::expected<void, ManagedError> width(std::int32_t value) {
    const auto result = live.set<"width">(value);

    return result ? std::expected<void, ManagedError>{} : std::unexpected(result.error());
  }
};

int main(int argc, char **argv) {
  return run(
    argc, argv,
    [](ManagedScene &scene) -> std::expected<Controller, ManagedError> {
      Controller result{&scene, {}};
      const auto attached = result.attachLive();

      return attached ? std::expected<Controller, ManagedError>(result) : std::unexpected(attached.error());
    },
    true);
}
