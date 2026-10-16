// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#pragma once

#include <portals/ui_style.hpp>

namespace scene_resource_example {
/** @brief Load a form resource and connect ordinary managed actions through named refs.
 * @param options Existing application/managed policies; defaults select the native clock.
 * @return Lazy form; Add Hello appends text, Exit/F10/Ctrl+C quit through the existing runner.
 */
inline portals::ui::Scene form(portals::ui::SceneOptions options = {}) {
  using namespace portals;

  auto scene = ui::scene(R"(VBox{
    Label{"Resource form | F10 quit"}
    LineEdit#message{""}
    Button#add{"Add Hello"}
    Button#exit{"Exit"}
  })",
                         {}, options);
  auto message = scene.get<LineEdit>("message");
  auto add = scene.get<Button>("add");
  auto exit = scene.get<Button>("exit");

  // clang-format off
  return std::move(scene)
    | ui::style(add, "*:focused { bold: true; }")
    | ui::onClick(add, [message] {
           message.text() += "Hello!";
         })
    | ui::onClick(exit, ui::quit)
    | ui::onKey("F10 | Ctrl+C", ui::quit);
  // clang-format on
}
} // namespace scene_resource_example
