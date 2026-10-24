// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_style.hpp>

/** @brief Load required fields and bind validation actions in C++.
 * @return Standard runner status after diagnostics and terminal cleanup.
 */
int main() {
  using namespace portals;

  auto scene = ui::scene(R"scene(
    scene {
      VBox {
        Label { "Profile drafts | Tab moves focus | F10 closes" }
        FormField#name {
          label="Display name"
          text="Alex"
          help="Shown next to your posts"
          required=true
        }
        FormField#team {
          label="Team"
          text="Portals"
          help="Enter confirms; the Validate button only checks the draft"
          required=true
          requiredMessage="Enter a team name"
          editMode=EXPLICIT
        }
        Button#check { "Validate" }
        Label#status { "No draft check requested" }
      }
    }
  )scene",
                         {.format = ui::SceneResourceFormat::CONFIGURATION});

  auto name = scene.get<FormField>("name");
  auto team = scene.get<FormField>("team");
  auto check = scene.get<Button>("check");
  auto status = scene.get<Label>("status");

  // clang-format off
  return scene
    | ui::style("Button:focused { bold: true; }")
    | ui::onClick(check, [name, team, status] {
        if (!name.valid()) {
          static_cast<void>(name.focus());

          return;
        }

        if (!team.valid()) {
          static_cast<void>(team.focus());

          return;
        }

        status.text() = "Both drafts meet their required rule";
      })
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
  // clang-format on
}
