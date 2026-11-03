// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <iostream>
#include <portals/ui.hpp>
using namespace portals;

int main() {
  auto app = Application::create();
  if (!app)
    return 1;
  auto owner = ManagedScene::create(**app);
  if (!owner)
    return 2;
  ui::Ref<Switch> ref;
  auto property = ref.checked();
  auto mounted = ui::mount(**owner, ui::toggle(ref, "Network", true));
  if (!mounted)
    return 3;
  auto initial = property.get();
  if (!initial || !*initial)
    return 4;
  auto update = property.set(false);
  if (!update || !update->changed || update->snapshot->value)
    return 5;
  auto equal = property.set(false);
  if (!equal || equal->changed || equal->snapshot != update->snapshot)
    return 6;
  owner->reset();
  auto closed = property.get();
  if (closed || std::get<TreeError>(closed.error().cause) != TreeError::CLOSED)
    return 7;
  std::cout << "Installed Switch/property lifecycle passed.\n";
  return 0;
}
