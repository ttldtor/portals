// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_scene_resource.hpp>
#include <portals/ui_style.hpp>

#include "tests/support/examples/replay/terminal.hpp"

namespace {
class Custom final : public portals::SizedWidget {};
} // namespace

int main() {
  using namespace portals;

  ui::SceneResourceOptions resource;

  resource.control<Custom>("Custom", [](ui::SceneResourceArguments) {
    return ui::custom<Custom>();
  });

  example::ReplayTerminal terminal({}, 100, {32, 5});
  ui::SceneOptions options;

  options.clock = terminal.clock();

  auto scene = ui::scene(R"scene(
    VBox {
      Custom { fitWidth = true, minWidth = 4, maxWidth = 8 }
      LineEdit#message {
        text = "initial", fitWidth = true, minWidth = 10, maxWidth = 28,
        fitHeight = true, maxHeight = 1
      }
      Button#add { "Add" }
    }
  )scene",
                         resource, options);
  auto message = scene.get<LineEdit>("message");
  bool configured{};

  scene.configure([&](ManagedScene &) {
    const auto value = message.text().get();

    configured = value && *value == "initial";
  });

  auto result =
    std::move(scene) | ui::style("Button { bold: true; width: fit-content; max-width: 12; }") | ui::run(terminal);

  if (!result || !configured || !terminal.closed || result->frames != 1 || message.text().get()) {
    return 2;
  }

  auto invalid = ui::scene("VBox{\n  Missing{}\n}");
  const auto failure = invalid.prepareChecked();

  if (failure || !failure.error().sceneResource || failure.error().sceneResource->line != 2) {
    return 3;
  }

  auto rejectedOptions = ui::SceneResourceOptions{}.control<Custom>("Button", [](ui::SceneResourceArguments) {
    return ui::custom<Custom>();
  });
  auto rejected =
    ui::scene(ui::SceneResourceFile{"not-read-after-registration-failure.scene"}, std::move(rejectedOptions), options);
  int reports{};

  rejected.onError([&](const ManagedDiagnostic &diagnostic) {
    const auto &error = diagnostic.error;
    if (error.sceneResource && error.sceneResource->code == SceneResourceErrorCode::DUPLICATE_TYPE &&
        error.sceneResource->line == 0 && error.sceneResource->column == 0) {
      ++reports;
    }
  });

  return rejected.runApp(terminal) == 1 && reports == 1 ? 0 : 4;
}
