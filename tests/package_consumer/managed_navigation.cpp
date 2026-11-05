// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/managed.hpp>

bool managedNavigationHeaderProbe();

int main() {
  using namespace portals;

  auto app = Application::create();
  if (!app || !managedNavigationHeaderProbe()) {
    return 1;
  }

  auto scene = ManagedScene::create(**app);
  auto resource = ModelResource::create();
  if (!scene || !resource) {
    return 2;
  }

  auto navigation = (*scene)->navigation(*resource, resource->identity);
  auto bookmark = ModelBookmark::create(*resource, {PositionKind::ITEM, {7, 1}, 3, 1});
  if (!navigation || !bookmark || !navigation->setCurrent(*bookmark)) {
    return 3;
  }

  auto channel = navigation->current();
  auto position = navigation->bookmark();
  if (!channel || !position || !*position || (**position).position().row != RowId{7, 1}) {
    return 4;
  }

  auto value = channel->snapshot();
  if (!value || (*value)->value.row != RowId{7, 1} || (*value)->value.offset != 0) {
    return 5;
  }
  if (!(*scene)->close()) {
    return 6;
  }

  return navigation->clearCurrent().error() == TreeError::CLOSED && bookmark->position().row == RowId{7, 1} ? 0 : 7;
}
