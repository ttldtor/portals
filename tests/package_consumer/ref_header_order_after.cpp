// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/message_box.hpp>
#include <portals/theme_selector.hpp>
#include <portals/ui.hpp>
#include <portals/ui_scene_pipeline.hpp>
#include <portals/ui_dialogs.hpp>
#include <portals/ui_modifiers.hpp>
#include <portals/ui_primitives.hpp>
#include <portals/ui_layout.hpp>
#include <portals/ui_virtual.hpp>
#include <portals/ui_controls.hpp>
#include <portals/ui_scene.hpp>
#include <portals/ui_ref.hpp>

bool refHeaderOrderAfterContracts() {
  using namespace portals;
  using namespace portals::ui;
  ui::MessageBox legacy;
  ui::Ref<MessageDialog> message(legacy);
  ui::ThemeSelectorRef legacySelector;
  ui::Ref<ui::ThemeSelectorRef> selector(legacySelector);
  ui::Ref<Label> label;
  auto description = ui::label(label, {});

  static_assert(std::same_as<ui::Ref<MessageDialog>, ui::Ref<ui::MessageBox>>);
  static_assert(std::derived_from<decltype(selector), ui::ThemeSelectorRef>);
  static_assert(std::same_as<decltype(ui::row(ui::label("x")) | ui::framed()), ui::Description<Row>>);
  static_assert(std::same_as<decltype(ui::placement(DialogPlacement::BELOW_START)), ui::DialogModifier>);
  static_assert(std::same_as<decltype(ui::placement(ScrollPlacement::RESERVED)), ui::ScrollAreaModifier>);
  (void)message;
  (void)description;

  return label.node() == Node<Label>{};
}
