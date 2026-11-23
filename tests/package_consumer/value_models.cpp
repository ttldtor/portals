// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

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

  return 0;
}
