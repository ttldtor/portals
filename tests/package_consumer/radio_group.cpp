// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Exercise installed radio declarations and state without starting terminal I/O. */
int main() {
  using namespace portals;

  ui::Ref<RadioGroup> mode;
  auto scene = ui::scene([=](ManagedScene &owner) {
    return ui::radioGroup(mode, owner.radioState({"Fast", "Balanced", "Quiet"}));
  });

  if (!scene.prepareChecked()) {
    return 1;
  }

  const auto value = mode.value();
  if (!value || value->selectedText() != "Fast") {
    return 2;
  }

  const auto cleared = mode.select(std::nullopt);
  if (!cleared || !cleared->changed || cleared->snapshot->value.selectedId()) {
    return 3;
  }

  if (!scene.close()) {
    return 4;
  }

  return mode.value() ? 5 : 0;
}
