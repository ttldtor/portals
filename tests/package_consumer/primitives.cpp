// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  static_assert(std::same_as<decltype(ui::gauge({}, {})), ui::Description<Gauge>>);
  static_assert(std::same_as<decltype(ui::gauge(.25)), ui::Description<Gauge>>);
  static_assert(std::same_as<decltype(ui::gauge({}, {}, {}, {})), ui::Description<Gauge>>);
  static_assert(std::same_as<decltype(ui::gauge(.25, GaugeOptions{}, ControlStyle{}, SizePolicy{})),
                             ui::Description<Gauge>>);
  static_assert(std::same_as<decltype(ui::gauge(std::declval<ui::Ref<Gauge> &>(), .25)), ui::Description<Gauge>>);
  static_assert(std::same_as<decltype(ui::gauge(std::declval<ui::Ref<Gauge>>(), .25)), ui::Description<Gauge>>);

  auto app = Application::create();

  if (!app) {
    return 1;
  }

  auto scene = ManagedScene::create(**app);

  if (!scene) {
    return 2;
  }

  ui::Ref<Gauge> load;
  ui::Ref<Button> exit;
  auto recipe = ui::hbox(
    ui::vbox(ui::label("Transfer"), ui::gauge(load, .65) | ui::width(10), ui::plainButton(exit, "!")) |
    ui::border({PrimitiveGlyphs::ASCII, 16384}) | ui::width(14) | ui::height(5));
  const auto mounted = ui::mount(**scene, std::move(recipe));

  if (!mounted || !(*scene)->prepare(**app, {20, 8})) {
    return 3;
  }

  const auto viewport = CellRect::from({}, {20, 8});
  const auto root = (*app)->tree().root();
  std::expected<void, TreeError> layout;

  if (!viewport || !root || !(*app)->tree().frame([&] {
        layout = layoutTree((*app)->tree(), *root, *viewport, *viewport);
      }) ||
      !layout) {
    return 4;
  }

  const auto gauge = (*app)->tree().inspect(load.node());
  int calls{};

  if (!gauge || gauge->geometry.local.size().width != 10 ||
      !(*scene)->onClick(exit.node(),
                         [&] {
                           ++calls;
                         }) ||
      !(*app)->router().invoke(exit.node(), {Action::ACTIVATE}) || calls != 1 ||
      !load.value(.75) || !load.adjust(-.25)) {
    return 5;
  }

  return (*scene)->close() ? 0 : 6;
}
