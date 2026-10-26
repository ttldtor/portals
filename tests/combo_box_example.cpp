// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/ui_style.hpp>

#include "support/choice_frame.hpp"

#include <chrono>
#include <thread>

namespace {
using namespace portals;
using namespace std::chrono_literals;
std::function<int(ui::Scene &)> scenario;

struct HeadlessRun {
  friend int operator|(ui::Scene &scene, HeadlessRun) {
    return scenario(scene);
  }
};
} // namespace

namespace portals::ui {
// The actual shipping source is compiled below; only its final terminal adapter is replaced in this test TU.
inline HeadlessRun choiceHeadlessRun() {
  return {};
}
} // namespace portals::ui

#define main choicesShippingMain
#define runApp choiceHeadlessRun
#include "../examples/choices/main.cpp"
#undef runApp
#undef main

TEST_CASE("the complete shipping choices program paints browse and indexed rows inside its framed themed popup") {
  using choice_test::take;
  auto pool = take(BackgroundPool::create());
  bool visited{};
  scenario = [&](ui::Scene &scene) {
    visited = true;
    scene.applicationOptions([&] {
      return ApplicationOptions{.backgroundExecutor = pool.get()};
    });
    take(scene.prepareChecked());
    auto &managed = take(scene.managed()).get();
    auto &app = ui::detail::SceneAccess::application(managed);
    constexpr CellSize VIEWPORT{100, 30};
    const auto pump = [&] {
      choice_test::pump(app, managed, *pool, VIEWPORT);
    };
    const auto until = [&](auto predicate) {
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      do {
        pump();
        if (predicate()) {
          return true;
        }
        std::this_thread::sleep_for(1ms);
      } while (std::chrono::steady_clock::now() < deadline);
      return predicate();
    };
    pump();
    ui::Ref<ComboBox> city;
    std::vector<ui::Ref<DropDown>> choices;
    std::vector<Node<Widget>> pending{managed.root()};
    while (!pending.empty()) {
      const auto node = pending.back();
      pending.pop_back();
      bool editable{};
      take(app.tree().read(node, [&](const Widget &widget) {
        editable = typeid(widget) == typeid(ComboBox);
      }));
      if (editable) {
        take(ui::detail::NamedAccess::publish(city, managed, node));
      }
      bool readOnly{};
      take(app.tree().read(node, [&](const Widget &widget) {
        readOnly = typeid(widget) == typeid(DropDown);
      }));
      if (readOnly) {
        ui::Ref<DropDown> choice;
        take(ui::detail::NamedAccess::publish(choice, managed, node));
        choices.push_back(choice);
      }
      auto snapshot = take(app.tree().inspect(node));
      pending.insert(pending.end(), snapshot.children.begin(), snapshot.children.end());
    }
    REQUIRE(city.node() != Node<ComboBox>{});
    const auto checkBackdrop = [&](CellStyle expected) {
      const auto frame = choice_test::paint(app, VIEWPORT);
      const auto control = take(app.tree().inspect(city.node())).geometry.surface;
      const std::array points{CellPoint{control.right() + 1, control.origin().y}, CellPoint{VIEWPORT.width - 1, 5},
                              CellPoint{50, VIEWPORT.height - 2}, CellPoint{VIEWPORT.width - 1, VIEWPORT.height - 1}};
      for (const auto point : points) {
        CAPTURE(point.x);
        CAPTURE(point.y);
        const auto cell = take(frame.at(point));
        CHECK(cell.glyph == " ");
        CHECK(cell.style == expected);
      }
    };
    checkBackdrop(resolveStyle(nordTheme(), StyleTarget{.type = "Scene"}).normal);
    take(city.open());
    REQUIRE(until([&] {
      return take(city.opened());
    }));
    REQUIRE(until([&] {
      const auto frame = choice_test::text(choice_test::paint(app, VIEWPORT));
      return frame.contains("Amsterdam") && frame.contains("Athens");
    }));
    take(city.text().set("Pra").result());
    REQUIRE(until([&] {
      const auto frame = choice_test::text(choice_test::paint(app, VIEWPORT));
      return frame.contains("Prague") && !frame.contains("Auckland");
    }));
    take(city.editor().focus());
    const auto input = [&](KeyEvent key) {
      if (!take(managed.input(app, key))) {
        take(app.router().dispatch(key));
      }
      pump();
    };
    input({InputKey::DOWN});
    input({InputKey::ENTER});
    REQUIRE(until([&] {
      return bool(take(city.selection()));
    }));
    CHECK(take(city.selection()).text() == "Prague");
    CHECK(choice_test::text(choice_test::paint(app, VIEWPORT)).contains("City: Prague"));
    REQUIRE(choices.size() == 2);
    auto delivery = take(choices[0].selection()).text() == "Courier" ? choices[0] : choices[1];
    auto palette = delivery.node() == choices[0].node() ? choices[1] : choices[0];
    input({InputKey::TAB});
    CHECK(take(app.router().focused()) == delivery.editor().node());
    input({InputKey::DOWN});
    REQUIRE(until([&] {
      return take(delivery.opened());
    }));
    input({InputKey::DOWN});
    input({InputKey::ENTER});
    REQUIRE(until([&] {
      return take(delivery.selection()).text() == "Pickup point";
    }));
    CHECK(choice_test::text(choice_test::paint(app, VIEWPORT)).contains("Delivery: Pickup point"));
    input({InputKey::TAB});
    CHECK(take(app.router().focused()) == palette.editor().node());
    input({InputKey::DOWN});
    REQUIRE(until([&] {
      return take(palette.opened());
    }));
    REQUIRE(until([&] {
      bool listFocus{};
      take(app.tree().read(take(app.router().focused()), [&](const Widget &widget) {
        if (auto list = dynamic_cast<const VirtualList<SearchItem> *>(&widget)) {
          listFocus = bool(list->selection());
        }
      }));
      return listFocus;
    }));
    input({InputKey::DOWN});
    pump();
    input({InputKey::ENTER});
    REQUIRE(until([&] {
      return take(palette.selection()).text() == "Gruvbox";
    }));
    checkBackdrop(resolveStyle(gruvboxTheme(), StyleTarget{.type = "Scene"}).normal);
    input({InputKey::DOWN});
    REQUIRE(until([&] {
      return take(palette.opened());
    }));
    input({InputKey::DOWN});
    input({InputKey::ENTER});
    REQUIRE(until([&] {
      return take(palette.selection()).text() == "Catppuccin Latte";
    }));
    checkBackdrop(resolveStyle(catppuccinLatteTheme(), StyleTarget{.type = "Scene"}).normal);

    scene | ui::style("Scene { background: #112233; }");
    pump();
    auto expected = resolveStyle(catppuccinLatteTheme(), StyleTarget{.type = "Scene"}).normal;
    expected.background = RgbColor{0x11, 0x22, 0x33};
    checkBackdrop(expected);
    scene.theme(nordTheme());
    pump();
    expected = resolveStyle(nordTheme(), StyleTarget{.type = "Scene"}).normal;
    expected.background = RgbColor{0x11, 0x22, 0x33};
    checkBackdrop(expected);
    const auto frame = choice_test::paint(app, VIEWPORT);
    const auto heading = scene.get<Label>("heading");
    const auto origin = take(app.tree().inspect(heading.node())).geometry.surface.origin();
    const auto cell = take(frame.at(origin));
    CHECK(cell.glyph == "C");
    CHECK(cell.style.bold);
    CHECK(cell.style.background != expected.background);
    return 0;
  };
  CHECK(choicesShippingMain() == 0);
  CHECK(visited);
  scenario = {};
  pool->requestStop();
  pool->join();
}
