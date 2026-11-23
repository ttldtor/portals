// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

#include <array>

using namespace portals;

struct Record {
  int count;
  bool enabled;

  bool operator==(const Record &) const = default;
};

int main() {
  auto app = Application::create();
  if (!app) {
    return 1;
  }
  auto scene = ManagedScene::create(**app);
  if (!scene) {
    return 2;
  }

  auto first = ui::model(Record{1, true});
  auto second = ui::model(Record{7, false});
  auto raw = ui::model(5);
  ui::Ref<Label> caption;
  ui::Ref<Button> button;
  ui::Ref<Border> frame;
  ui::Ref<Label> rawLabel;
  ui::Ref<Button> rawButton;
  auto mounted = ui::mount(**scene, ui::vbox(ui::label(caption, first, &Record::count),
                                             ui::button(button, text("Count {}", first.field(&Record::count))),
                                             ui::label(rawLabel, raw.value()),
                                             ui::button(rawButton, raw.value(),
                                                        [] {
                                                        }))
                                      .framed(frame, "ascii", text("Count {}", first.field(&Record::count))));
  if (!mounted) {
    return 3;
  }

  const auto changed = (first.field(&Record::count) += 1).result();
  raw.value() = 6;
  if (!changed || !changed->snapshot->value.enabled) {
    return 4;
  }
  if (!(*scene)->prepare(**app, {40, 10}) || !(*scene)->prepare(**app, {40, 10})) {
    return 5;
  }
  auto displayed = caption.text().get();
  auto buttonText = button.text().get();
  if (!displayed || *displayed != "2" || !buttonText || *buttonText != "Count 2") {
    return 6;
  }
  auto rawLabelText = rawLabel.text().get();
  auto rawButtonText = rawButton.text().get();
  if (!rawLabelText || *rawLabelText != "6" || !rawButtonText || *rawButtonText != "6") {
    return 6;
  }
  if (!caption.setModel(second)) {
    return 7;
  }

  displayed = caption.text().get();
  if (!displayed || *displayed != "7") {
    return 8;
  }
  if (!frame.setModel(text("Other {}", second.field(&Record::count)))) {
    return 9;
  }
  if (!(*scene)->close()) {
    return 10;
  }

  auto rowsApp = Application::create();
  if (!rowsApp) {
    return 20;
  }
  auto rowsScene = ManagedScene::create(**rowsApp);
  if (!rowsScene) {
    return 21;
  }
  auto rows = ui::model(std::array{4, 5});
  auto navigation = ui::navigation(rows);
  if (!ui::mount(**rowsScene, ui::virtualList(navigation))) {
    return 22;
  }
  auto current = navigation.currentRow();
  if (!current || *current) {
    return 23;
  }
  if (!(*rowsScene)->prepare(**rowsApp, {40, 10})) {
    return 24;
  }
  auto handle = rows.managed();
  if (!handle) {
    return 25;
  }
  auto before = detail::ManagedModelAccess::snapshot(*handle, (*rowsScene)->root().id());
  if (!before) {
    return 26;
  }
  const auto id = (*before)->read({0, 1}).rows[0].id;
  if (!rows.update(id, 7) || !rows.row(id, 9) ||
      !(*rowsScene)->prepare(**rowsApp, {40, 10})) {
    return 27;
  }
  auto after = detail::ManagedModelAccess::snapshot(*handle, (*rowsScene)->root().id());
  if (!after || (*after)->read({0, 1}).rows[0].id != id ||
      (*after)->read({0, 1}).rows[0].value != 9 ||
      (*before)->read({0, 1}).rows[0].value != 4) {
    return 28;
  }
  if (!(*rowsScene)->close() || navigation.currentRow()) {
    return 29;
  }

  return 0;
}
