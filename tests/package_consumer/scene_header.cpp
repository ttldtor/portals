// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_scene.hpp>

// This out-of-line function forces typed lookup instantiation with no recipe header.
portals::ui::Ref<portals::Label> sceneHeaderLookup(portals::ui::Scene &scene) {
  return scene.get<portals::Label>("label");
}

bool sceneHeaderContracts() {
  portals::ui::Ref<portals::Button> button;
  const auto registered = button.onClick(portals::ui::quit);
  const auto registeredAt = button.onClick(portals::ui::quit, std::source_location::current());

  if (registered || registeredAt) {
    return false;
  }

  const auto *cause = std::get_if<portals::TreeError>(&registered.error().cause);
  const auto *causeAt = std::get_if<portals::TreeError>(&registeredAt.error().cause);

  return cause && *cause == portals::TreeError::CLOSED && causeAt && *causeAt == portals::TreeError::CLOSED;
}
