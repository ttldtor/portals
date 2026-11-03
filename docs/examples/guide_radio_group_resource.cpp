// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Bind code-owned radio options to a loaded scene and observe them through the same named Ref.
 * @return Standard runner status after checked registry/setup reporting and terminal restoration.
 */
int main() {
  using namespace portals;

  // clang-format off
  return []() -> std::expected<ui::Scene, ManagedError> {
    ui::SceneResourceOptions options;

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    auto registered = options.registry.add<RadioGroup>("Modes", [](ui::SceneResourceArguments) {
      return ui::radioGroup({"Fast", "Balanced", "Quiet"});
    });

    if (!registered) {
      ManagedError failure{ManagedPhase::MOUNT, {}, TreeError::INVALID_ARGUMENT, {}, {}};

      failure.sceneResource = std::make_shared<SceneResourceError>(
        SceneResourceError{.code = registered.error(), .line = 0, .column = 0});

      return std::unexpected(failure);
    }

    auto scene = ui::scene(R"scene(
      scene {
        VBox {
          Label { "Arrows move | Space/Enter selects | F10 closes" }
          Modes#mode { radioInitial.index=1 radioSelectionMode=ON_ACTIVATION classes="settings" width=24 }
          Label#selected { "Balanced" }
        }
      }
    )scene", options);
    auto mode = scene.get<RadioGroup>("mode");
    auto selected = scene.get<Label>("selected");

    return std::move(scene)
      | ui::onChange(mode, [selected](const RadioGroupValue &value) {
          selected.text() = value.selectedText();
        })
      | ui::onKey(key::F10, ui::quit);
  } | ui::runApp();
  // clang-format on
}
