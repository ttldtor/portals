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
  auto mounted = ui::mount(
    **scene,
    ui::viewer(reader, first).fastScroll().scrollStep(1).pageOverlap(1).lineNumbers().framed("rounded", "Document"));
  if (!mounted || !first.byteSource() || !reader.scrollRows(0) || !reader.scrollPages(0) || !reader.goHome() ||
      !reader.goEnd() || !reader.home() || !reader.end() || !reader.scroll(0) || !reader.page(0) ||
      !reader.seekByte(3)) {
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
  if (!reader.setHexOptions({.bytesPerRow = 8}) || !reader.setMode(DocumentViewMode::HEX) ||
      !reader.hexOptions({.bytesPerRow = 16}) || !reader.mode(DocumentViewMode::TEXT)) {
    return 6;
  }
  auto hexRecipe = ui::viewer("missing-file.txt").hex({.bytesPerRow = 8, .autoFit = true});
  (void)hexRecipe;
  return (*scene)->close() ? 0 : 7;
}
