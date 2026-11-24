// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>
#include <portals/ui_model_form.hpp>

#include <array>
#include <iostream>
#include <stdexcept>

namespace {
using namespace portals;

struct City {
  std::string name;
  int orders{};
  std::string region;
  bool operator==(const City &) const = default;
};

template <class T, class E> T checked(std::expected<T, E> value) {
  if (!value) {
    throw std::runtime_error("Unexpected checked form failure");
  }
  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

void require(bool condition, const char *message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

struct Sink final : PaintContext {
  std::expected<void, TreeError> submit(std::span<const RenderCommand>) override {
    return {};
  }
};
} // namespace

int main() try {
  auto app = checked(Application::create());
  auto scene = checked(ManagedScene::create(*app));
  auto cities = ui::model(std::array{City{"Prague", 10, "Europe"}, City{"Tokyo", 20, "Asia"}});
  auto navigation = ui::navigation(cities);
  auto edit = ui::edit(navigation, {.mode = EditMode::EXPLICIT});
  ui::Ref<FormField> orders;
  ui::Ref<FormField> region;
  checked(ui::mount(*scene, ui::form(edit, ui::field("City", &City::name).required(),
                                     ui::field("Orders", &City::orders).reference(orders),
                                     ui::field("Region", &City::region).readOnly().reference(region))));
  auto refresh = [&] {
    checked(scene->prepare(*app, {60, 20}));
    const auto bounds = checked(CellRect::from({}, {60, 20}));
    checked(app->tree().frame([&] {
      checked(layoutTree(app->tree(), app->root(), bounds, bounds));
      Sink sink;
      checked(app->tree().paint(orders.editor().node(), sink));
      checked(app->tree().paint(region.editor().node(), sink));
    }));
  };
  auto current = checked(navigation.managed());
  checked(current.navigate({1, 1}));
  refresh();
  require(checked(orders.text().get()) == "10", "Numeric field did not follow current");
  checked((orders.text() = "17").result());
  checked(edit.commit().result());
  require(*checked(edit.field(&City::orders).snapshot()).accepted == 17, "Numeric draft did not publish");
  checked(app->router().setFocus(region.editor().node()));
  checked(app->router().dispatch(TextInputEvent{U'X'}));
  require(checked(region.text().get()) == "Europe", "Readonly field accepted user input");
  checked(current.navigate({2, 1}));
  refresh();
  require(checked(orders.text().get()) == "20", "Form did not switch records");
  checked((orders.text() = "bad").result());
  require(!edit.commit().result(), "Malformed numeric draft published");
  checked(edit.cancel().result());
  require(checked(orders.text().get()) == "20", "Cancel did not restore numeric accepted value");
  checked(scene->close());

  return 0;
} catch (const std::exception &error) {
  std::cerr << error.what() << '\n';

  return 1;
}
