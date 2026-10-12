// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "static_scene_support.hpp"

using namespace staticConsumer;

int main(int argc, char **argv) {
  return runScene(argc, argv, [](ui::Ref<LineEdit> field, ui::Ref<Label> status, auto action) {
    return ui::column(FRAME, ui::label("Name", CAPTION_STYLE),
                      ui::row(ui::lineEdit(field, "Portals", {.policy = FIELD_SIZE}).id("field"),
                              ui::button("Apply", std::move(action)).id("apply")),
                      ui::label(status, "Ready").id("status"))
      .id("form");
  });
}
