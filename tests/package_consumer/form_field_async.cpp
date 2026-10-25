// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <stop_token>

int main() {
  using namespace portals;

  static_assert(std::is_aggregate_v<ui::FormFieldConfig>);
  static_assert(std::is_move_constructible_v<LineEdit>);
  static_assert(std::is_move_assignable_v<LineEdit>);
  ui::FormFieldConfig legacy{"Help",
                             true,
                             [](std::string_view) {
                               return FieldValidation::valid();
                             },
                             "Required",
                             {},
                             {}};
  auto scene = ui::scene(ui::vbox(ui::formField("Legacy", "value", std::move(legacy)),
                                  ui::formField("Name", "alice",
                                                {.editor = {.options = {.editMode = EditMode::EXPLICIT}},
                                                 .validateAsync = [](std::string_view value, std::stop_token) {
                                                   return value.empty() ? FieldValidation::invalid("Required")
                                                                        : FieldValidation::valid();
                                                 }})));

  return scene.prepareChecked() && scene.close() ? 0 : 1;
}
