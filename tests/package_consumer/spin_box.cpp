// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_mount.hpp>
#include <portals/ui_spin_box.hpp>
#include <stdexcept>

int main() {
  using namespace portals;
  auto app = Application::create().value();
  auto scene = ManagedScene::create(*app).value();
  auto model = scene->numericState(2).value();
  ui::Ref<SpinBox> amount;
  if (!ui::mount(*scene, ui::spinBox(amount, model).editMode(EditMode::EXPLICIT))) {
    return 1;
  }
  model.set(4).value(); // No dispatch: the setter must adopt the latest clean base.
  auto assignment = amount.value() = 3;
  auto observed = assignment.result();
  if (!observed || observed->commit || amount.value().get() != 3 || model.snapshot().value()->value.value() != 4) {
    return 2;
  }
  if (!app->router().invoke(amount.lineEdit().node(), ActionEvent{Action::ACTIVATE})) {
    return 3;
  }
  if (model.snapshot().value()->value.value() != 3) {
    return 4;
  }
  const auto removed = scene->remove(amount.lineEdit().node());
  if (removed || !std::get_if<TreeError>(&removed.error().cause) ||
      std::get<TreeError>(removed.error().cause) != TreeError::INVALID_ARGUMENT) {
    return 5;
  }
  return scene->prepare(*app, {40, 5}) ? 0 : 6;
}
