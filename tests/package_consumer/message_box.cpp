// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/message_box.hpp>
#include <portals/ui.hpp>

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
  ui::MessageBox box;
  ui::Ref<Column> owner;
  auto mounted = ui::mount(**scene, ui::overlayHost(ui::column(ui::label("Document")).reference(owner),
                                                    ui::unsavedChangesDialog(box, "Unsaved", "Save before closing?")));
  if (!mounted) {
    return 3;
  }
  int callbacks{};
  if (!box.open(owner, [&callbacks](const MessageResult &result) {
        if (result && *result == MessageChoice::CANCEL) {
          ++callbacks;
        }
      })) {
    return 4;
  }
  if (!box.cancel() || callbacks) {
    return 5;
  }
  auto answer = box.lastResult();
  if (!answer || !**answer || ***answer != MessageChoice::CANCEL) {
    return 6;
  }
  if (!(**scene).afterDispatch(**app) || !(**app).scheduler().dispatch()) {
    return 7;
  }
  if (callbacks != 1 || !(**scene).afterDispatch(**app)) {
    return 8;
  }
  if (!box.open([] {
      })) {
    return 9;
  }
  return 0;
}
