// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_ref.hpp>
#include <portals/ui_scene.hpp>
#include <portals/message_box.hpp>
#include <portals/theme_selector.hpp>
#include <portals/ui_controls.hpp>
#include <portals/ui_virtual.hpp>
#include <portals/ui_layout.hpp>
#include <portals/ui_primitives.hpp>
#include <portals/ui_modifiers.hpp>
#include <portals/ui_dialogs.hpp>
#include <portals/ui_scene_pipeline.hpp>
#include <portals/ui.hpp>

bool refHeaderOrderBeforeContracts() {
  using namespace portals;
  using namespace portals::ui;
  ui::Ref<MessageDialog> message;
  ui::Ref<ui::ThemeSelectorRef> selector;
  ui::Ref<Label> label;
  ui::Ref<Button> button;
  auto description = ui::column(ui::label({}), ui::button({}), ui::label(label, {}),
                                ui::button(button, {}, [] {}));

  static_assert(std::derived_from<decltype(message), ui::MessageBox>);
  static_assert(std::derived_from<decltype(selector), ui::ThemeSelectorRef>);
  static_assert(std::same_as<decltype(ui::label({})), ui::Description<Label>>);
  static_assert(std::same_as<decltype(ui::button({})), ui::Description<Button>>);
  static_assert(std::same_as<decltype(ui::column(ui::button("OK") | ui::width(8))), ui::Description<Column>>);
  static_assert(std::same_as<decltype(ui::mode(ChartMode::CANDLES)), ui::ChartModifier>);
  static_assert(std::same_as<decltype(ui::mode(DialogMode::MODAL)), ui::DialogModifier>);
  (void)description;

  return label.node() == Node<Label>{} && button.node() == Node<Button>{};
}
