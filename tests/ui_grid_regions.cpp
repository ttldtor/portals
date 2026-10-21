// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "support/examples/replay/terminal.hpp"
#include <portals/ui.hpp>
#include <source_location>

using namespace portals;

namespace {
template <class T, class E> T take(std::expected<T, E> result,
                                  const std::source_location location = std::source_location::current()) {
  INFO("checked call at line " << location.line());

  if constexpr (std::same_as<E, TreeError>) {
    if (!result) {
      INFO("TreeError " << static_cast<int>(result.error()));
      REQUIRE(result);
    }
  }

  REQUIRE(result);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

void prepareRegion(ManagedScene &scene, Application &app, CellSize size) {
  const auto bounds = take(CellRect::from({}, size));
  constexpr int PREPARATION_PASSES = 3;

  for (int pass = 0; pass < PREPARATION_PASSES; ++pass) {
    take(app.tree().frame([&] {
      take(layoutTree(app.tree(), app.root(), bounds, bounds));
    }));
    const auto snapshot = take(LayoutSnapshot::capture(app.tree(), app.root(), LayoutSnapshotProfile::GEOMETRY));
    take(scene.prepareViews(app, snapshot));
  }
}
}

TEST_CASE("regional grid converges in the existing runner and does not redraw while idle") {
  for (const bool scrolling : {false, true}) {
    CAPTURE(scrolling);
    example::ReplayTerminal terminal({}, 100, {24, 6});
    ui::SceneOptions options;
    options.clock = terminal.clock();
    ui::Ref<Button> heading;
    ui::Ref<LineEdit> editor;
    auto grid = ui::grid(ui::header(ui::button(heading, "Heading")).sticky(),
                         ui::body(ui::lineEdit(editor, "body"))).columns(1);

    if (scrolling) {
      grid = std::move(grid).scroll();
    }

    int observations{};
    auto scene = ui::scene(std::move(grid), options)
      | ui::onLayout(heading, [&](const NodeGeometry &geometry) {
          ++observations;
          CHECK(geometry.valid);
          CHECK(geometry.local.origin().y == 0);
        });

    const auto result = scene.run(terminal);

    REQUIRE(result);
    CHECK(result->frames == 1);
    CHECK(observations == 1);
    CHECK(terminal.closed);
  }
}

TEST_CASE("zero body region keeps header focus eligible and closes body input without hiding it") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  ui::Ref<Grid> grid;
  ui::Ref<Button> heading;
  ui::Ref<LineEdit> editor;

  take(ui::mount(*scene, ui::grid(ui::header(ui::button(heading, "Heading")).sticky(),
                                 ui::body(ui::lineEdit(editor, "body")))
                           .columns(1).scroll().reference(grid)));

  const auto bounds = take(CellRect::from({}, {20, 1}));
  take(scene->prepare(*app, bounds.size()));
  const auto snapshot = [&] {
    take(app->tree().frame([&] {
      take(layoutTree(app->tree(), app->root(), bounds, bounds));
    }));

    return take(LayoutSnapshot::capture(app->tree(), app->root(), LayoutSnapshotProfile::GEOMETRY));
  }();
  take(scene->prepareViews(*app, snapshot));

  CHECK(take(app->tree().inputEligible(heading.node())));
  CHECK_FALSE(take(app->tree().inputEligible(editor.node())));
  CHECK(take(app->tree().inspect(editor.node())).visible);
  CHECK(take(app->tree().inspect(editor.node())).enabled);
}

TEST_CASE("regional focus recovery refreshes presentation and reopening restores eligibility") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  ui::Ref<Button> heading;
  ui::Ref<Button> body;
  take(ui::mount(*scene, ui::grid(ui::header(ui::button(heading, "Heading", [] {})).sticky(),
                                 ui::body(ui::button(body, "Body", [] {}))).columns(1).scroll()));
  take(app->router().setFocus(body.node()));
  take(scene->prepare(*app, {20, 1}));
  prepareRegion(*scene, *app, {20, 1});

  CHECK(take(app->router().focused()) == heading.node());
  take(app->tree().read(heading.node(), [](const Button &widget) {
    CHECK(widget.focused());
    CHECK(widget.enabled());
  }));
  take(app->tree().read(body.node(), [](const Button &widget) {
    CHECK_FALSE(widget.focused());
    CHECK_FALSE(widget.enabled());
  }));

  prepareRegion(*scene, *app, {20, 5});
  take(app->tree().read(body.node(), [](const Button &widget) {
    CHECK(widget.enabled());
  }));
}

TEST_CASE("regional focus loss may remove its owning grid without leaving stale preparation") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  ui::Ref<Grid> grid;
  ui::Ref<LineEdit> editor;
  ui::LineEditConfig config;
  config.options.editMode = EditMode::ON_COMPLETION;
  take(ui::mount(*scene, ui::grid(ui::header(ui::button("Heading")).sticky(),
                                 ui::body(ui::lineEdit(editor, "body", config)))
                           .columns(1).scroll().reference(grid)));
  int removed{};
  take(scene->onEditingFinished(editor.node(), [&]() -> std::expected<void, TreeError> {
    ++removed;
    return app->tree().remove(grid.node());
  }));
  take(app->router().setFocus(editor.node()));
  take(scene->prepare(*app, {20, 1}));
  prepareRegion(*scene, *app, {20, 1});

  CHECK(removed == 1);
  CHECK_FALSE(app->tree().inspect(grid.node()));
}

TEST_CASE("sticky header reveal changes the shared horizontal offset without moving the body vertically") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  auto controller = take(ScrollController::create());
  GridScrollOptions scroll;
  scroll.controller = controller;
  scroll.area.horizontal = ScrollBarPolicy::NEVER;
  scroll.area.vertical = ScrollBarPolicy::NEVER;
  ui::Ref<Grid> grid;
  ui::Ref<Label> heading;

  take(ui::mount(*scene,
    ui::grid(ui::header(ui::label("First").width(18), ui::label(heading, "Second").width(8)).sticky(),
             ui::body(ui::label("a"), ui::label("b"), ui::label("c"), ui::label("d"),
                      ui::label("e"), ui::label("f"), ui::label("g"), ui::label("h")))
      .columns(2).columnGap(2).scroll(controller).scrollOptions(scroll.area).reference(grid)));
  take(scene->prepare(*app, {12, 3}));
  prepareRegion(*scene, *app, {12, 3});
  take(scene->setScrollOffset(grid.node(), ScrollAxis::VERTICAL, {1, 0}));
  take(scene->ensureVisible(heading.node()));
  prepareRegion(*scene, *app, {12, 3});

  const auto state = take(controller->snapshot());

  CHECK(state.horizontal.offset.raster > 0);
  CHECK(state.vertical.offset.raster == 1);
}
