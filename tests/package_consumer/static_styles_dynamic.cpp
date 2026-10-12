// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "static_styles_support.hpp"

using namespace staticStylesConsumer;

struct Controller {
  ManagedScene *scene;
  ThemeTokens colors{FIRST};
  std::int32_t minimum{2};

  auto install(ThemeTokens values, std::int32_t width) -> std::expected<bool, ManagedError> {
    try {
      const auto lighten = [](std::uint8_t value) {
        return static_cast<std::uint8_t>((value * 80 + 255 * 20) / 100);
      };
      values.selection = {lighten(values.background.red), lighten(values.background.green),
                          lighten(values.background.blue)};
      auto sheet = themeFromTokens(values);
      StyleRule metric;
      metric.selector.id = "action";
      metric.declarations.minWidth = width;
      sheet.rules.push_back(metric);
      auto installed = scene->theme(std::move(sheet));

      if (!installed) {
        return std::unexpected(installed.error());
      }

      colors = values;
      minimum = width;

      return true;
    } catch (const std::bad_alloc &) {
      return std::unexpected(ManagedError{ManagedPhase::STYLE, scene->root().id(), TreeError::NO_MEMORY, {}, {}});
    }
  }

  auto palette(ThemeTokens values) {
    return install(values, minimum);
  }

  auto background(RgbColor value) {
    auto candidate = colors;
    candidate.background = value;

    return install(candidate, minimum);
  }

  auto width(std::int32_t value) {
    return install(colors, value);
  }
};

int main(int argc, char **argv) {
  return run(argc, argv, [](ManagedScene &scene) -> std::expected<Controller, ManagedError> {
    Controller result{&scene};
    auto installed = result.install(FIRST, 2);

    return installed ? std::expected<Controller, ManagedError>(result) : std::unexpected(installed.error());
  });
}
