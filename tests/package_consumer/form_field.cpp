// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Verify field composition and a cold text observation through the installed SDK. */
int main() {
  using namespace portals;

  auto app = Application::create();

  if (!app) {
    return 1;
  }

  auto owner = ManagedScene::create(**app);

  if (!owner) {
    return 2;
  }

  ui::Ref<FormField> field;
  ui::Ref<Label> mirror;
  auto text = field.text();
  auto mounted =
    ui::mount(**owner, ui::vbox(ui::formField(field, "Name", "", {.required = true}), ui::label(mirror, text)));

  if (!mounted) {
    return 3;
  }

  auto empty = field.validate();

  if (!empty || *empty) {
    return 4;
  }

  if (!(text = "Alex").result() || !(*owner)->prepare(**app, {30, 6})) {
    return 5;
  }

  auto valid = field.validate();
  auto caption = mirror.text().get();

  if (!valid || !*valid || !caption || *caption != "Alex") {
    return 6;
  }

  if (!field.focus()) {
    return 7;
  }

  auto focused = (*app)->router().focused();

  return focused && *focused == field.editor().node() ? 0 : 8;
}
