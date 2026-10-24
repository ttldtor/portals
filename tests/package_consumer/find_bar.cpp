// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Exercise retained search, snapshots and lazy query refs against the installed SDK. */
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
  ui::Ref<FindBar> find;
  ui::Ref<Label> mirror;
  auto query = find.text();
  auto mounted = ui::mount(**scene, ui::vbox(ui::findBar(find, "alpha beta alpha", "alpha"), ui::label(mirror, query)));
  if (!mounted) {
    return 3;
  }
  auto first = find.match();
  if (!first || !*first || first->storedCount() != 2 || first->begin() != 0 || !find.next()) {
    return 4;
  }
  auto second = find.match();
  if (!second || second->begin() != 11) {
    return 5;
  }
  if (!(query = "beta").result() || !(*scene)->prepare(**app, {40, 8})) {
    return 6;
  }
  auto selected = find.match();
  auto caption = mirror.text().get();
  if (!selected || selected->text() != "beta" || !caption || *caption != "beta") {
    return 7;
  }
  if (!find.document("beta beta")) {
    return 8;
  }
  auto current = find.current(*first);
  if (!current || *current || first->text() != "alpha") {
    return 9;
  }
  if (!find.clear()) {
    return 10;
  }
  auto empty = find.match();
  return empty && !*empty ? 0 : 11;
}
