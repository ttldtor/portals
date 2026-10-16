// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <array>

int main() {
  using namespace portals;

  auto app = Application::create();

  if (!app) {
    return 1;
  }

  auto scene = ManagedScene::create(**app);

  if (!scene) {
    return 2;
  }

  const std::array<std::string_view, 3> captions{"Mercury", "Venus", "Earth"};
  auto recipe = ui::vbox(captions | std::views::transform([](std::string_view text) {
                           return ui::label(text);
                         }));
  const auto mounted = ui::mount(**scene, std::move(recipe));

  if (!mounted) {
    return 3;
  }

  const auto snapshot = (*app)->tree().inspect(*mounted);

  if (!snapshot || snapshot->children.size() != captions.size()) {
    return 4;
  }

  return (*scene)->close() ? 0 : 5;
}
