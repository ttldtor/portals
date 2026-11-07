// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
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
  ui::Ref<DocumentView> reader;
  auto first = ui::document(std::make_shared<const std::string>("first\nsecond"));
  auto second = ui::document(std::make_shared<const std::string>("another document"));
  auto mounted = ui::mount(**scene, ui::viewer(reader, first).lineNumbers().framed("rounded", "Document"));
  if (!mounted || !first.byteSource() || !reader.seekByte(3)) {
    return 3;
  }
  auto before = reader.status();
  if (!before || before->byteOffset != 3 || !reader.setModel(second) || !reader.setModel(first)) {
    return 4;
  }
  auto restored = reader.status();
  if (!restored || restored->byteOffset != 3 || !reader.refresh()) {
    return 5;
  }
  return (*scene)->close() ? 0 : 6;
}
