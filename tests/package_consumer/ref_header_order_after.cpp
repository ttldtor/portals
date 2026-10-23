// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/message_box.hpp>
#include <portals/theme_selector.hpp>
#include <portals/ui.hpp>
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
  (void)message;
  (void)description;

  return label.node() == Node<Label>{};
}
