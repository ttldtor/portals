// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "static_scene_support.hpp"
#include <portals/ui_static.hpp>

using namespace staticConsumer;

constexpr auto FORM = ui::ct::scene(
  ui::ct::column<"form">(FRAME, ui::ct::label("Name", CAPTION_STYLE),
                         ui::ct::row(ui::ct::lineEdit<"field">("Portals", EditMode::IMMEDIATE, {}, FIELD_SIZE),
                                     ui::ct::button<"apply">("Apply")),
                         ui::ct::label<"status">("Ready")));

static_assert(FORM.root().NODE_COUNT == 6);
static_assert(std::get<0>(FORM.root().children()).text() == "Name");
static_assert(std::get<0>(std::get<1>(FORM.root().children()).children()).sizePolicy().width.weight == 2);

int main(int argc, char **argv) {
  return runScene(argc, argv, [](ui::Ref<LineEdit> field, ui::Ref<Label> status, auto action) {
    return ui::ct::describe(FORM, ui::ct::reference<"field">(field), ui::ct::reference<"status">(status),
                            ui::ct::onClick<"apply">(std::move(action)));
  });
}
