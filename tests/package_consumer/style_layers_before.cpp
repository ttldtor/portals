// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "style_layers_support.hpp"

using namespace styleLayersConsumer;

struct Controller {
  ManagedScene *scene;
  StyleSheet application{applicationSheet()};
  StyleSheet user{userSheet()};
  RgbColor background{24, 28, 36};
  std::int32_t minimumWidth{2};

  std::expected<void, ManagedError> install(StyleSheet sheet) {
    // Compose all retained overrides for every legacy whole-sheet replacement.
    sheet.rules.insert(sheet.rules.end(), application.rules.begin(), application.rules.end());
    sheet.rules.insert(sheet.rules.end(), user.rules.begin(), user.rules.end());

    return scene->theme(std::move(sheet));
  }

  auto switchTheme() {
    return install(themeFromTokens(staticStylesConsumer::SECOND));
  }

  auto attachLive() {
    return install({{{{}, {.background = background}}, {{.id = "action"}, {.minWidth = minimumWidth}}}});
  }

  std::expected<void, ManagedError> width(std::int32_t value) {
    const auto installed = install({{{{}, {.background = background}}, {{.id = "action"}, {.minWidth = value}}}});

    if (installed) {
      minimumWidth = value;
    }

    return installed;
  }
};

int main(int argc, char **argv) {
  return run(argc, argv, [](ManagedScene &scene) -> std::expected<Controller, ManagedError> {
    Controller result{&scene};
    const auto installed = result.attachLive();

    return installed ? std::expected<Controller, ManagedError>(result) : std::unexpected(installed.error());
  });
}
