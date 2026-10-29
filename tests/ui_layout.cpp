// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "support/examples/gallery/gallery.hpp"
#include "support/examples/replay/terminal.hpp"
#include <portals/ui.hpp>

#include <memory>
#include <new>
#include <source_location>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace portals;

namespace {
template <class T, class E>
T take(std::expected<T, E> result, std::source_location origin = std::source_location::current()) {
  if (!result) {
    MESSAGE(std::string_view{origin.file_name()} << ':' << origin.line());
    if constexpr (std::same_as<E, TreeError>) {
      MESSAGE("TreeError code: ", static_cast<int>(result.error()));
    }
  }
  if constexpr (std::same_as<E, ManagedError>) {
    if (!result) {
      ui::SceneReport report;
      report.failure = result.error();
      ui::reportToStderr(report);
    }
  }
  REQUIRE(result);
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

bool cause(const ManagedError &error, TreeError expected) {
  const auto *value = std::get_if<TreeError>(&error.cause);
  return value && *value == expected;
}

struct Fixture {
  std::unique_ptr<Application> app;
  std::unique_ptr<ManagedScene> scene;

  explicit Fixture(ApplicationOptions appOptions = {}, ManagedOptions sceneOptions = {})
      : app(take(Application::create(appOptions))), scene(take(ManagedScene::create(*app, sceneOptions))) {
  }

  void layout(CellSize size = {30, 6}, bool publish = true) {
    take(scene->prepare(*app, size));
    const auto bounds = take(CellRect::from({}, size));
    take(app->tree().frame([&] {
      take(layoutTree(app->tree(), app->root(), bounds, bounds));
    }));
    take(app->tree().consumeFrameRequest());
    if (publish) {
      take(scene->completedLayout(*app));
    }
  }

  SchedulerDispatch dispatch() {
    const auto result = take(app->scheduler().dispatch());
    take(scene->afterDispatch(*app));
    return result;
  }
};

CellBuffer render(ManagedScene &scene, Application &app, CellSize size) {
  take(scene.prepare(app, size));
  take(app.tree().consumeFrameRequest());
  const auto bounds = take(CellRect::from({}, size));
  std::optional<CellBuffer> result;
  take(app.tree().frame([&] {
    take(layoutTree(app.tree(), app.root(), bounds, bounds));
    std::vector<RenderCommand> commands;
    auto collect = [&](auto &self, Node<Widget> node) -> void {
      const auto info = take(app.tree().inspect(node));
      if (!info.visible) {
        return;
      }
      gallery::SurfaceSink sink(info.geometry, commands);
      take(app.tree().paint(node, sink));
      for (auto child : info.children) {
        self(self, child);
      }
    };
    collect(collect, scene.root());
    result.emplace(take(renderFullFrame(size, commands, {})));
  }));
  take(scene.completedLayout(app));
  take(app.tree().consumeFrameRequest());
  return std::move(*result);
}

template <class T>
concept HasOverlayAlignment = requires(T &&recipe) {
  std::move(recipe).horizontalAlign(LayoutAlign::END);
  std::move(recipe) | ui::padding(LayoutPadding{});
};
static_assert(HasOverlayAlignment<ui::Description<Overlay>>);
static_assert(!HasOverlayAlignment<ui::Description<OverlayHost>>);
static_assert(!HasOverlayAlignment<ui::Description<Grid>>);
} // namespace

TEST_CASE("fit and independent bounds agree between methods and pipe modifiers") {
  for (const auto pipes : {false, true}) {
    Fixture fixture;
    ui::Ref<Label> child;
    auto recipe = ui::label(child, "abcdefgh");

    if (pipes) {
      recipe = std::move(recipe) | ui::fitWidth() | ui::fitHeight() | ui::minWidth(3) | ui::maxWidth(6) |
               ui::minHeight(2) | ui::maxHeight(4);
    } else {
      recipe = std::move(recipe).fitWidth().fitHeight().minWidth(3).maxWidth(6).minHeight(2).maxHeight(4);
    }

    take(ui::mount(*fixture.scene, ui::vbox(std::move(recipe))));
    fixture.layout({20, 10});
    CHECK(take(child.geometry()).local.size() == CellSize{6, 2});
    fixture.layout({2, 1});
    CHECK(take(child.geometry()).local.size() == CellSize{3, 2});
    CHECK(take(child.geometry()).clip.size() == CellSize{2, 1});
    take(fixture.app->tree().read(child.node(), [](const Label &widget) {
      const auto policy = widget.sizePolicy();

      CHECK(policy.width.mode == SizeMode::CONTENT);
      CHECK(policy.height.mode == SizeMode::CONTENT);
    }));
  }
}

TEST_CASE("cold sizing order preserves exact bounds and validates the final sparse result") {
  for (int order = 0; order != 5; ++order) {
    CAPTURE(order);
    Fixture fixture;
    ui::Ref<Label> child;
    auto recipe = ui::label(child, "abcdefgh").height(3);

    if (order == 0) {
      recipe = std::move(recipe).width(20).fitWidth();
    } else if (order == 1) {
      recipe = std::move(recipe).width(20).minWidth(12).maxWidth(32).fitWidth();
    } else if (order == 2) {
      recipe = std::move(recipe).fitWidth().width(20);
    } else if (order == 3) {
      recipe = std::move(recipe).minWidth(24).maxWidth(12).maxWidth(32).fitWidth();
    } else {
      recipe = std::move(recipe).width(20).fitWidth().width(6);
    }

    take(ui::mount(*fixture.scene, ui::vbox(std::move(recipe))));
    fixture.layout({40, 8});

    const auto expectedWidth = order == 1 ? 12 : order == 3 ? 24 : order == 4 ? 6 : 20;

    CHECK(take(child.geometry()).local.size() == CellSize{expectedWidth, 3});
    take(fixture.app->tree().read(child.node(), [&](const Label &widget) {
      const auto policy = widget.sizePolicy();

      CHECK(policy.width.mode == (order == 2 || order == 4 ? SizeMode::FIXED : SizeMode::CONTENT));
      CHECK(policy.height.minimum == 3);
      CHECK(policy.height.maximum == 3);
      CHECK(policy.height.mode == SizeMode::FIXED);
    }));
  }
}

TEST_CASE("framed menu keeps intrinsic preference and targets outer constraints in either recipe order") {
  for (int order = 0; order != 4; ++order) {
    CAPTURE(order);
    Fixture fixture;
    ui::ActionSet actions;
    const auto open = actions.action("open", "Open", [](const ui::ActionInvocation &) {
    });
    const auto close = actions.action("close", "Close", [](const ui::ActionInvocation &) {
    });
    ui::Ref<Menu> menu;
    ui::Ref<Border> outer;
    auto recipe = ui::menu(menu, {open, close});

    if (order == 1) {
      recipe = std::move(recipe).fitWidth().fitHeight().minWidth(14).maxWidth(18).maxHeight(4).framed(outer, "ascii");
    } else if (order == 2) {
      recipe = std::move(recipe).framed(outer, "ascii").fitWidth().fitHeight().minWidth(14).maxWidth(18).maxHeight(4);
    } else if (order == 3) {
      recipe = std::move(recipe).framed(outer, "ascii").width(16).height(6);
    } else {
      recipe = std::move(recipe).framed(outer, "ascii");
    }

    take(ui::mount(*fixture.scene, ui::vbox(std::move(recipe))));
    fixture.layout({30, 10});

    const auto expected = order == 0 ? CellSize{11, 4} : order == 3 ? CellSize{16, 6} : CellSize{14, 4};

    CHECK(take(outer.geometry()).local.size() == expected);
    CHECK(take(menu.geometry()).local.origin() == CellPoint{1, 1});
    CHECK(take(menu.geometry()).local.size() == CellSize{9, 2});
    CHECK(take(fixture.app->tree().inspect(menu.node())).parent == outer.node());
    take(fixture.app->tree().read(menu.node(), [](const Menu &widget) {
      const auto policy = widget.sizePolicy();

      CHECK(policy.width.minimum == 0);
      CHECK(policy.height.minimum == 0);
      CHECK(policy.width.mode == SizeMode::CONTENT);
      CHECK(policy.height.mode == SizeMode::CONTENT);
    }));
    take(fixture.app->tree().read(outer.node(), [&](const Border &widget) {
      const auto policy = widget.sizePolicy();

      CHECK(policy.width.mode == (order == 3 ? SizeMode::FIXED : SizeMode::CONTENT));
      CHECK(policy.height.mode == (order == 3 ? SizeMode::FIXED : SizeMode::CONTENT));
    }));
  }
}

TEST_CASE("invalid final cold sizing rolls back framed descendants and unpublished references") {
  Fixture fixture;
  ui::Ref<Label> retained;

  take(ui::mount(*fixture.scene, ui::label(retained, "retained")));

  const auto before = take(fixture.app->tree().inspect(fixture.scene->root())).children.size();
  ui::Ref<Column> parent;
  ui::Ref<Label> early;
  ui::Ref<Label> invalid;
  ui::Ref<Border> outer;
  auto capture = std::make_shared<int>(7);
  const std::weak_ptr<int> lifetime = capture;
  auto recipe = ui::vbox(ui::label(early, "early"),
                         ui::label(invalid, "invalid")
                           .framed(outer, "ascii")
                           .width(20)
                           .minWidth(24)
                           .setup([keep = capture](ManagedScene &, Node<Label>) -> std::expected<void, ManagedError> {
                             CHECK(*keep == 7);

                             return {};
                           }))
                  .reference(parent);

  capture.reset();

  const auto failed = ui::mount(*fixture.scene, std::move(recipe));

  REQUIRE_FALSE(failed);
  CHECK(cause(failed.error(), TreeError::INVALID_ARGUMENT));
  CHECK(parent.node() == Node<Column>{});
  CHECK(early.node() == Node<Label>{});
  CHECK(invalid.node() == Node<Label>{});
  CHECK(outer.node() == Node<Border>{});
  CHECK(lifetime.expired());
  CHECK(take(fixture.app->tree().inspect(fixture.scene->root())).children.size() == before);
  CHECK(take(retained.text().get()) == "retained");

  ui::Ref<Label> recovered;

  take(ui::mount(*fixture.scene, ui::label(recovered, "valid").fitWidth().minWidth(2).maxWidth(8)));
  CHECK(recovered.node() != Node<Label>{});
}

TEST_CASE("ordinary Overlay methods pipes and configuration preserve actual layout paint and hit order") {
  for (int mode = 0; mode != 3; ++mode) {
    INFO("recipe mode: ", mode);
    auto recipe = [&]() -> ui::Description<Widget> {
      if (mode == 0) {
        return ui::detail::Access::erase(ui::overlay(ui::button("abc").chrome(ButtonChrome::NONE).id("lower"),
                                                     ui::button("XYZ").chrome(ButtonChrome::NONE).id("upper"))
                                           .padding({1, 1, 1, 1})
                                           .horizontalAlign(LayoutAlign::END)
                                           .verticalAlign(LayoutAlign::CENTER)
                                           .width(10)
                                           .height(6)
                                           .id("overlay"));
      }
      if (mode == 1) {
        auto description = ui::overlay(ui::button("abc").chrome(ButtonChrome::NONE).id("lower"),
                                       ui::button("XYZ").chrome(ButtonChrome::NONE).id("upper"))
                             .width(10)
                             .height(6)
                             .id("overlay") |
                           ui::padding({1, 1, 1, 1}) | ui::horizontalAlign(LayoutAlign::END) |
                           ui::verticalAlign(LayoutAlign::CENTER);
        return ui::detail::Access::erase(std::move(description));
      }
      ui::SceneResourceOptions options;
      options.format = ui::SceneResourceFormat::CONFIGURATION;
      return ui::resource("scene{Overlay#overlay{width=10 height=6 padding{left=1 top=1 right=1 bottom=1} "
                          "horizontalAlign=END verticalAlign=CENTER Button#lower{text=\"abc\" chrome=NONE} "
                          "Button#upper{text=\"XYZ\" chrome=NONE}}}",
                          options);
    }();
    auto owner = ui::scene(std::move(recipe));
    auto overlay = owner.get<Overlay>("overlay");
    auto lower = owner.get<Button>("lower");
    auto upper = owner.get<Button>("upper");
    Application *app{};
    ManagedScene *scene{};
    owner.configure([&](ManagedScene &managed, Application &application) {
      scene = &managed;
      app = &application;
      take(managed.onClick(lower.node(), [] {
      }));
      take(managed.onClick(upper.node(), [] {
      }));
    });
    take(owner.prepareChecked());
    const auto image = render(*scene, *app, {10, 6});
    const auto expected = take(CellRect::from({6, 2}, {3, 1}));
    CHECK(take(lower.geometry()).local == expected);
    CHECK(take(upper.geometry()).local == expected);
    const auto children = take(app->tree().inspect(overlay.node())).children;
    REQUIRE(children.size() == 2);
    CHECK(children[0].id() == lower.node().id());
    CHECK(children[1].id() == upper.node().id());
    CHECK(take(image.at({6, 2})).glyph == "X");
    CHECK(take(image.at({8, 2})).glyph == "Z");
    CHECK(take(app->router().hitTest({6, 2})).id() == upper.node().id());
    take(scene->prepare(*app, {10, 6}));
    CHECK(take(app->tree().consumeFrameRequest()) == Invalidation::NONE);
    take(owner.close());
    CHECK_FALSE(upper.geometry());
  }
}

TEST_CASE("ordinary Overlay preserves actual options size policy precedence and shortage insets") {
  Fixture fixture;
  ui::Ref<Overlay> overlay;
  ui::Ref<Label> child;
  const OverlayOptions options{{2, 1, 1, 1}, LayoutAlign::END, LayoutAlign::END};
  const SizePolicy policy{{10, 10, 10, SizeMode::FIXED}, {6, 6, 6, SizeMode::FIXED}};
  ui::ControlContext controls;
  controls.defaults("Overlay", {.verticalAlign = LayoutAlign::CENTER})
    .define("Layer", "Overlay", {.horizontalAlign = LayoutAlign::CENTER});
  take(ui::mount(*fixture.scene,
                 ui::row(ui::overlay(overlay, options, policy, ui::label(child, "abc"))
                           .variant("Layer")
                           .horizontalAlign(LayoutAlign::START)),
                 controls));
  fixture.layout({20, 10});
  CHECK(take(overlay.geometry()).local.size() == CellSize{10, 6});
  CHECK(take(child.geometry()).local == take(CellRect::from({2, 2}, {3, 1})));
  take(fixture.app->tree().read(overlay.node(), [&](const Overlay &widget) {
    CHECK(widget.options().padding.left == 2);
    CHECK(widget.options().padding.right == 1);
    CHECK(widget.options().horizontal == LayoutAlign::START);
    CHECK(widget.options().vertical == LayoutAlign::CENTER);
    CHECK(widget.sizePolicy().width.maximum == 10);
  }));
  ui::Ref<Overlay> small;
  ui::Ref<Label> clipped;
  Fixture shortage;
  take(ui::mount(
    *shortage.scene,
    ui::row(ui::overlay(small, OverlayOptions{{3, 2, 4, 3}}, ui::label(clipped, "content")).width(2).height(1))));
  shortage.layout({20, 10});
  CHECK(take(clipped.geometry()).local.origin() == CellPoint{3, 2});
  CHECK(take(clipped.geometry()).local.size() == CellSize{0, 0});
  for (const auto invalidOptions :
       {OverlayOptions{{-1, 0, 0, 0}}, OverlayOptions{{}, static_cast<LayoutAlign>(9), LayoutAlign::START},
        OverlayOptions{{}, LayoutAlign::START, static_cast<LayoutAlign>(9)}}) {
    ui::Ref<Overlay> unpublished;
    int effects{};
    const auto invalid = ui::mount(
      *fixture.scene,
      ui::overlay(unpublished, invalidOptions,
                  ui::label("late").setup([&](ManagedScene &, Node<Label>) -> std::expected<void, ManagedError> {
                    ++effects;
                    return {};
                  })));
    REQUIRE_FALSE(invalid);
    CHECK(unpublished.node() == Node<Overlay>{});
    CHECK(effects == 0);
  }
  CHECK_FALSE(ui::mount(*fixture.scene, ui::custom<Overlay>().padding({})));
  ui::ControlContext caller;
  caller.define("Overlay", "Label");
  CHECK_FALSE(ui::mount(*fixture.scene, ui::overlay(), caller));
}

TEST_CASE("ordinary Overlay child failure rolls back all unpublished parent and sibling refs") {
  Fixture fixture;
  ui::Ref<Overlay> parent;
  ui::Ref<Label> first;
  const auto count = take(fixture.app->tree().inspect(fixture.scene->root())).children.size();
  const auto failed = ui::mount(
    *fixture.scene,
    ui::overlay(parent, ui::label(first, "early"),
                ui::label("late").setup([](ManagedScene &, Node<Label> node) -> std::expected<void, ManagedError> {
                  return std::unexpected(
                    ManagedError{ManagedPhase::MOUNT, node.id(), TreeError::INVALID_ARGUMENT, {}, {}});
                })));
  REQUIRE_FALSE(failed);
  CHECK(parent.node() == Node<Overlay>{});
  CHECK(first.node() == Node<Label>{});
  CHECK(take(fixture.app->tree().inspect(fixture.scene->root())).children.size() == count);
}

TEST_CASE("notification Overlay consumes ordinary layout modifiers without posting service messages") {
  Fixture fixture;
  ui::NotificationCenter center;
  ui::Ref<Overlay> wrapper;
  ui::Ref<Label> content;
  take(ui::mount(*fixture.scene, ui::notifications(wrapper, center, ui::label(content, "body"))
                                   .padding({2, 1, 1, 1})
                                   .horizontalAlign(LayoutAlign::END)
                                   .verticalAlign(LayoutAlign::CENTER)
                                   .width(10)
                                   .height(6)));
  fixture.layout({10, 6});
  CHECK(take(content.geometry()).local == take(CellRect::from({5, 2}, {4, 1})));
  take(fixture.app->tree().read(wrapper.node(), [](const Overlay &widget) {
    CHECK(widget.options().padding.left == 2);
    CHECK(widget.options().horizontal == LayoutAlign::END);
    CHECK(widget.options().vertical == LayoutAlign::CENTER);
  }));
  const auto snapshot = take(center.snapshot());
  CHECK(snapshot->items.empty());
  take(fixture.scene->remove(wrapper.node()));
  CHECK_FALSE(center.snapshot());
}

TEST_CASE("existing notification wrapper ignores unrelated caller Overlay declarations while consuming local layout") {
  for (const bool modified : {false, true}) {
    Fixture fixture;
    ui::NotificationCenter center;
    ui::Ref<Overlay> wrapper;
    ui::Ref<Label> content;
    ui::ControlContext caller;
    caller.define("Overlay", "Label", {.text = text("unrelated")});
    auto recipe = ui::notifications(wrapper, center, ui::label(content, "body")).width(10).height(6);
    if (modified) {
      (void)std::move(recipe)
        .padding({2, 1, 1, 1})
        .horizontalAlign(LayoutAlign::END)
        .verticalAlign(LayoutAlign::CENTER);
    }
    take(ui::mount(*fixture.scene, std::move(recipe), caller));
    fixture.layout({10, 6});
    CHECK(take(content.geometry()).local ==
          (modified ? take(CellRect::from({5, 2}, {4, 1})) : take(CellRect::from({0, 0}, {10, 6}))));
    take(fixture.app->tree().read(wrapper.node(), [&](const Overlay &widget) {
      CHECK(widget.options().padding.left == (modified ? 2 : 0));
      CHECK(widget.options().horizontal == (modified ? LayoutAlign::END : LayoutAlign::STRETCH));
      CHECK(widget.options().vertical == (modified ? LayoutAlign::CENTER : LayoutAlign::STRETCH));
    }));
    CHECK(take(content.text().get()) == "body");
    CHECK(take(center.snapshot())->items.empty());
  }
}

TEST_CASE("declarative grid mounts compact columns without client identities") {
  Fixture fixture;
  ui::Ref<Grid> grid;
  ui::Ref<Label> name;
  ui::Ref<Label> value;

  take(ui::mount(*fixture.scene,
                 ui::grid(ui::label(name, "Name"), ui::label(value, "Ada")).columns(2).columnGap(2).reference(grid)));
  fixture.layout();

  CHECK(take(name.geometry()).local.origin() == CellPoint{0, 0});
  CHECK(take(value.geometry()).local.origin() == CellPoint{6, 0});
  take(fixture.app->tree().read(grid.node(), [&](const Grid &widget) {
    const auto options = widget.options();

    REQUIRE(options.cells.size() == 2);
    CHECK(options.columns.size() == 2);
    CHECK(options.rows.size() == 1);
    CHECK(options.cells[0].node == name.node().id());
    CHECK(options.cells[1].node == value.node().id());
  }));
}

TEST_CASE("grid sections flatten controls and share horizontal tracks") {
  Fixture fixture;
  ui::Ref<Grid> grid;
  ui::Ref<Label> heading, value, name;

  take(ui::mount(*fixture.scene, ui::grid(ui::header(ui::label(heading, "Names").columnSpan(2)).sticky(),
                                          ui::body(ui::label(name, "Name"), ui::label(value, "Ada")))
                                   .columns(2)
                                   .columnGap(2)
                                   .reference(grid)));
  fixture.layout();

  CHECK(take(heading.geometry()).local == take(CellRect::from({}, {9, 1})));
  CHECK(take(name.geometry()).local.origin() == CellPoint{0, 1});
  CHECK(take(value.geometry()).local.origin() == CellPoint{6, 1});
  CHECK(take(fixture.app->tree().inspect(grid.node())).children.size() == 3);
  take(fixture.app->tree().read(grid.node(), [&](const Grid &widget) {
    const auto options = widget.options();

    CHECK(widget.usesRegions());
    CHECK(options.header_rows.size() == 1);
    CHECK(options.rows.size() == 1);
    CHECK(options.cells[0].row == 0);
    CHECK(options.cells[1].row == 0);
    CHECK(options.cells[0].region == GridRegion::HEADER);
    CHECK(options.cells[1].region == GridRegion::BODY);
  }));
}

TEST_CASE("grid section validation happens before ancestor setup") {
  Fixture fixture;
  int effects{};
  auto checked = [&](auto recipe) {
    const auto failed = ui::mount(
      *fixture.scene,
      ui::column(std::move(recipe)).setup([&](ManagedScene &, Node<Column>) -> std::expected<void, ManagedError> {
        ++effects;
        return {};
      }));

    REQUIRE_FALSE(failed);
    CHECK(cause(failed.error(), TreeError::INVALID_ARGUMENT));
    CHECK(effects == 0);
  };

  checked(ui::grid(ui::header(ui::label("a")), ui::header(ui::label("b"))).columns(1));
  checked(ui::grid(ui::body(ui::label("a")), ui::label("direct")).columns(1));
  checked(ui::grid(ui::header(ui::label("requires columns"))));
  checked(ui::grid(ui::body(ui::label("invalid sticky")).sticky()).columns(1));

  auto source = ui::header(ui::label("consumed"));
  auto consumed = std::move(source);

  checked(ui::grid(std::move(source)).columns(1));
}

TEST_CASE("advanced grid track policies are owning and row policies apply only to body") {
  Fixture fixture;
  ui::Ref<Label> heading, cell;
  std::vector<AxisSize> columns{{6, 6, 6, SizeMode::FIXED}};
  std::vector<AxisSize> rows{{3, 3, 3, SizeMode::FIXED}};
  auto recipe = ui::grid(ui::header(ui::label(heading, "head")), ui::body(ui::label(cell, "body")))
                  .columns(1)
                  .columnSizes(columns)
                  .rowSizes(rows);

  columns[0] = {10, 10, 10, SizeMode::FIXED};
  rows[0] = {8, 8, 8, SizeMode::FIXED};
  take(ui::mount(*fixture.scene, std::move(recipe)));
  fixture.layout();

  CHECK(take(heading.geometry()).local.size() == CellSize{6, 1});
  CHECK(take(cell.geometry()).local == take(CellRect::from({0, 1}, {6, 3})));
}

TEST_CASE("advanced grid track errors are rejected before ancestor setup") {
  Fixture fixture;
  int effects{};
  const auto checked = [&](auto recipe) {
    const auto failed = ui::mount(
      *fixture.scene,
      ui::column(std::move(recipe)).setup([&](ManagedScene &, Node<Column>) -> std::expected<void, ManagedError> {
        ++effects;
        return {};
      }));

    REQUIRE_FALSE(failed);
    CHECK(cause(failed.error(), TreeError::INVALID_ARGUMENT));
    CHECK(effects == 0);
  };

  checked(ui::grid(ui::label("wrong column count")).columns(2).columnSizes({AxisSize{}}));
  checked(ui::grid(ui::label("wrong body row count")).columns(1).rowSizes({AxisSize{}, AxisSize{}}));
  checked(ui::grid(ui::label("invalid track")).columns(1).columnSizes({AxisSize{.minimum = -1}}));
}

TEST_CASE("declarative grid settings preserve sparse defaults and explicit false") {
  Fixture fixture;
  ui::ControlContext controls;
  controls.defaults("Grid", {.grid = {.columns = 2, .columnGap = 4, .rowGap = 3, .scroll = true}})
    .define("NamedGrid", "Grid", {.grid = {.columnGap = 2}});
  ui::Ref<Grid> grid;
  int childEffects{};
  take(
    ui::mount(*fixture.scene,
              ui::grid(ui::label("left").setup([&](ManagedScene &, Node<Label>) -> std::expected<void, ManagedError> {
                ++childEffects;
                return {};
              }),
                       ui::label("right"))
                .variant("NamedGrid")
                .settings({.grid = {.rowGap = 0, .scroll = false}})
                .reference(grid),
              controls));
  take(fixture.app->tree().read(grid.node(), [](const Grid &widget) {
    const auto options = widget.options();

    CHECK(options.columns.size() == 2);
    CHECK(options.column_gap == 2);
    CHECK(options.row_gap == 0);
  }));
  CHECK(childEffects == 1);
  const auto noScroll = setScrollOffset(fixture.app->tree(), grid.node(), ScrollAxis::VERTICAL, {1, 0});
  CHECK_FALSE(noScroll);
  CHECK(noScroll.error() == TreeError::INVALID_ARGUMENT);
  CHECK_FALSE(controls.resolve("Grid"));
}

TEST_CASE("scene factories validate the effective Grid graph before calling user code") {
  ui::ControlContext controls;
  controls.define("NamedGrid", "Grid", {.grid = {.columns = 2, .columnGap = 3}});
  int calls{};
  ui::Ref<Grid> grid;
  auto owner = ui::scene(
    [&](ManagedScene &) {
      ++calls;
      return ui::grid(ui::label("Name"), ui::label("Ada"))
        .variant("NamedGrid")
        .settings({.grid = {.columnGap = 2}})
        .reference(grid);
    },
    {.controls = controls});
  take(owner.prepareChecked());
  CHECK(calls == 1);
  auto managed = take(owner.managed());
  auto &application = ui::detail::SceneAccess::application(managed.get());
  take(application.tree().read(grid.node(), [](const Grid &widget) {
    CHECK(widget.options().columns.size() == 2);
    CHECK(widget.options().column_gap == 2);
  }));
  CHECK_FALSE(controls.resolve("NamedGrid"));
  take(owner.close());

  controls.define("Broken", "Absent");
  auto failed = ui::scene(
    [&](ManagedScene &) {
      ++calls;
      return ui::grid(ui::label("unreachable")).columns(1);
    },
    {.controls = controls});
  CHECK_FALSE(failed.prepareChecked());
  CHECK(calls == 1);
}

TEST_CASE("real declarative Grid rejects a custom terminal before child construction") {
  Fixture fixture;
  ui::ControlContext controls;
  controls.define("Grid", "Label");
  int effects{};
  const auto failed = ui::mount(
    *fixture.scene,
    ui::grid(ui::label("not mounted").setup([&](ManagedScene &, Node<Label>) -> std::expected<void, ManagedError> {
      ++effects;
      return {};
    }))
      .columns(1),
    controls);

  REQUIRE_FALSE(failed);
  CHECK(failed.error().sceneResource);
  CHECK(effects == 0);
  CHECK(take(controls.resolve("Grid")).base == "Label");
}

TEST_CASE("grid preflight rejects nested conflicts before ancestor setup and child factories") {
  Fixture fixture;
  ui::Ref<Grid> grid;
  ui::Ref<Label> first;
  int effects{};

  auto result =
    ui::mount(*fixture.scene,
              ui::column(ui::grid(ui::label(first, "first").row(0).column(0).setup([&](ManagedScene &, Node<Label>) {
                           ++effects;

                           return std::expected<void, ManagedError>{};
                         }),
                                  ui::label("conflict").row(0).column(0))
                           .columns(2)
                           .reference(grid))
                .setup([&](ManagedScene &, Node<Column>) {
                  ++effects;

                  return std::expected<void, ManagedError>{};
                }));

  REQUIRE_FALSE(result);
  CHECK(cause(result.error(), TreeError::INVALID_ARGUMENT));
  CHECK(effects == 0);
  CHECK(grid.node() == Node<Grid>{});
  CHECK(first.node() == Node<Label>{});
  CHECK(take(fixture.app->tree().inspect(fixture.scene->root())).children.empty());
}

TEST_CASE("grid relationship modifiers require an immediate declarative grid parent") {
  Fixture fixture;
  int effects{};

  auto misplaced =
    ui::mount(*fixture.scene, ui::row(ui::label("misplaced").columnSpan(2)).setup([&](ManagedScene &, Node<Row>) {
      ++effects;

      return std::expected<void, ManagedError>{};
    }));

  REQUIRE_FALSE(misplaced);
  CHECK(cause(misplaced.error(), TreeError::INVALID_ARGUMENT));
  CHECK(effects == 0);

  auto partial = ui::mount(*fixture.scene, ui::grid(ui::label("partial").row(0)).columns(2));

  REQUIRE_FALSE(partial);
  CHECK(cause(partial.error(), TreeError::INVALID_ARGUMENT));

  auto overflow = ui::mount(*fixture.scene, ui::grid(ui::label("gap overflow"))
                                              .columns(3)
                                              .columnGap(std::numeric_limits<std::int32_t>::max())
                                              .setup([&](ManagedScene &, Node<Grid>) {
                                                ++effects;

                                                return std::expected<void, ManagedError>{};
                                              }));

  REQUIRE_FALSE(overflow);
  CHECK(cause(overflow.error(), TreeError::INVALID_GEOMETRY));
  CHECK(effects == 0);

  auto legacy = ui::mount(*fixture.scene, ui::grid(ui::label("explicit parent").columnSpan(2)));

  REQUIRE_FALSE(legacy);
  CHECK(cause(legacy.error(), TreeError::INVALID_ARGUMENT));
}

TEST_CASE("grid column-major flow and future explicit spans preserve structural order") {
  Fixture fixture;
  ui::Ref<Grid> grid;
  ui::Ref<Label> first;
  ui::Ref<Label> second;
  ui::Ref<Label> explicitCell;

  take(ui::mount(*fixture.scene, ui::grid(ui::label(first, "a").rowSpan(2), ui::label(second, "b"),
                                          ui::label(explicitCell, "c").row(0).column(0))
                                   .rows(3)
                                   .flow(GridFlow::COLUMN_MAJOR)
                                   .rowGap(1)
                                   .reference(grid)));
  take(fixture.app->tree().read(grid.node(), [&](const Grid &widget) {
    const auto options = widget.options();

    REQUIRE(options.cells.size() == 3);
    CHECK(options.rows.size() == 3);
    CHECK(options.columns.size() == 2);
    CHECK(options.row_gap == 1);
    CHECK(options.cells[0].row == 1);
    CHECK(options.cells[0].column == 0);
    CHECK(options.cells[1].row == 0);
    CHECK(options.cells[1].column == 1);
    CHECK(options.cells[2].node == explicitCell.node().id());
  }));
  fixture.layout();
}

TEST_CASE("grid hidden cells keep occupancy and removed identities never bind replacements") {
  Fixture fixture;
  ui::Ref<Grid> grid;
  ui::Ref<Label> first;
  ui::Ref<Label> second;

  take(ui::mount(*fixture.scene, ui::grid(ui::label(first, "hidden").setup([&](ManagedScene &, Node<Label> node) {
                                   auto result = fixture.app->tree().setVisible(node, false);

                                   return result ? std::expected<void, ManagedError>{}
                                                 : std::unexpected(ui::detail::error(node, result.error()));
                                 }),
                                          ui::label(second, "visible"))
                                   .columns(2)
                                   .reference(grid)));
  fixture.layout();
  take(fixture.app->tree().read(grid.node(), [&](const Grid &widget) {
    const auto options = widget.options();

    CHECK(options.cells[0].node == first.node().id());
    CHECK(options.cells[1].row == 0);
    CHECK(options.cells[1].column == 1);
  }));
  take(fixture.app->tree().setVisible(first.node(), true));
  fixture.layout();
  CHECK(take(second.geometry()).local.origin().x > 0);

  const auto old = first.node();

  take(fixture.scene->remove(old));

  const auto text = take(GraphemeText::from("replacement", fixture.scene->widthPolicy()));
  const auto replacement = take(fixture.scene->emplace<Label>(grid.node(), text));

  CHECK(replacement.id() != old.id());

  const auto bounds = take(CellRect::from({}, {30, 6}));
  std::expected<void, TreeError> arranged;

  take(fixture.app->tree().frame([&] {
    arranged = layoutTree(fixture.app->tree(), fixture.app->root(), bounds, bounds);
  }));

  REQUIRE_FALSE(arranged);
  CHECK(arranged.error() == TreeError::INVALID_ARGUMENT);
}

TEST_CASE("grid work exhaustion and child mount failure leave refs unpublished") {
  Fixture fixture;
  ui::Ref<Grid> grid;
  ui::Ref<Label> first;
  int effects{};
  DeclarativeGridOptions options;

  options.columns = 1;
  options.maxPlacementWork = 2;

  auto limited = ui::mount(*fixture.scene, ui::grid(ui::label(first, "limited").setup([&](ManagedScene &, Node<Label>) {
                                             ++effects;

                                             return std::expected<void, ManagedError>{};
                                           }))
                                             .gridOptions(options)
                                             .reference(grid));

  REQUIRE_FALSE(limited);
  CHECK(cause(limited.error(), TreeError::NODE_LIMIT));
  CHECK(effects == 0);
  CHECK(grid.node() == Node<Grid>{});
  CHECK(first.node() == Node<Label>{});

  auto failed = ui::mount(
    *fixture.scene,
    ui::grid(ui::label(first, "reserved"), ui::label("failed").setup([](ManagedScene &, Node<Label> node) {
      return std::expected<void, ManagedError>(std::unexpected(ui::detail::error(node, TreeError::CALLBACK_FAILED)));
    }))
      .columns(2)
      .reference(grid));

  REQUIRE_FALSE(failed);
  CHECK(cause(failed.error(), TreeError::CALLBACK_FAILED));
  CHECK(grid.node() == Node<Grid>{});
  CHECK(first.node() == Node<Label>{});
  CHECK(take(fixture.app->tree().inspect(fixture.scene->root())).children.empty());
}

TEST_CASE("checked Ref geometry is an owning final value and never performs layout") {
  ui::Ref<Label> label;
  REQUIRE_FALSE(label.geometry());
  CHECK(cause(label.geometry().error(), TreeError::CLOSED));
  Fixture fixture;
  take(ui::mount(*fixture.scene, ui::label(label, "geometry")));
  CHECK(cause(label.geometry().error(), TreeError::INVALID_GEOMETRY));
  fixture.layout({30, 6}, false);
  CHECK(cause(label.geometry().error(), TreeError::INVALID_GEOMETRY));
  take(fixture.scene->completedLayout(*fixture.app));
  const auto saved = take(label.geometry());
  CHECK(take(label.size()) == saved.local.size());
  take(fixture.app->tree().invalidate(label.node(), Invalidation::LAYOUT));
  CHECK(cause(label.geometry().error(), TreeError::INVALID_GEOMETRY));
  CHECK(saved.valid);
  fixture.layout();
  CHECK(take(label.geometry()).local == saved.local);
  // Equal numeric coordinates still replace provenance, before a final publication exists.
  take(fixture.app->tree().frame([&] {
    take(fixture.app->tree().arrange(label.node(), saved.local, saved.clip));
  }));
  CHECK(cause(label.geometry().error(), TreeError::INVALID_GEOMETRY));
  take(fixture.scene->completedLayout(*fixture.app));
  CHECK(take(label.geometry()).local == saved.local);
}

TEST_CASE("final geometry is deferred, unchanged values stay idle and clip-only changes notify") {
  Fixture fixture;
  ui::Ref<Label> label;
  take(ui::mount(*fixture.scene, ui::label(label, "geometry")));
  std::vector<NodeGeometry> values;
  take(label.onLayout([&](const NodeGeometry &value) {
    values.push_back(value);
    CHECK(take(label.geometry()).clip == value.clip);
    // This mutation would fail if delivery borrowed an active frame.
    take(fixture.app->tree().setEnabled(label.node(), false));
  }));
  fixture.layout({30, 6}, false);
  fixture.dispatch();
  CHECK(values.empty());
  take(fixture.scene->completedLayout(*fixture.app));
  CHECK(values.empty());
  CHECK(fixture.scene->hasPendingLayout());
  fixture.dispatch();
  REQUIRE(values.size() == 1);
  CHECK_FALSE(fixture.scene->hasPendingLayout());
  fixture.layout();
  fixture.dispatch();
  CHECK(values.size() == 1);
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  const auto clipped = take(CellRect::from({}, {2, 1}));
  take(fixture.app->tree().frame([&] {
    take(fixture.app->tree().arrange(label.node(), values.front().local, values.front().surface, clipped));
  }));
  take(fixture.scene->completedLayout(*fixture.app));
  fixture.dispatch();
  REQUIRE(values.size() == 2);
  CHECK(values.back().local == values.front().local);
  CHECK(values.back().clip != values.front().clip);
}

TEST_CASE("invalidation cancels pending equal geometry and hidden parents restart initial delivery") {
  Fixture fixture;
  ui::Ref<Label> label;
  ui::Ref<Column> parent;
  take(ui::mount(*fixture.scene, ui::vbox(ui::label(label, "geometry")).reference(parent)));
  int calls{};
  take(label.onLayout([&](const NodeGeometry &) {
    ++calls;
  }));
  fixture.layout();
  take(fixture.app->tree().invalidate(label.node(), Invalidation::LAYOUT));
  fixture.dispatch();
  CHECK(calls == 0);
  fixture.layout();
  fixture.dispatch();
  CHECK(calls == 1);
  fixture.layout({20, 6});
  take(fixture.app->tree().setVisible(parent.node(), false));
  CHECK(cause(label.geometry().error(), TreeError::INVALID_GEOMETRY));
  fixture.dispatch();
  CHECK(calls == 1);
  fixture.layout();
  CHECK(cause(label.geometry().error(), TreeError::INVALID_GEOMETRY));
  take(fixture.app->tree().setVisible(parent.node(), true));
  fixture.layout();
  fixture.dispatch();
  CHECK(calls == 2);
}

TEST_CASE("self-removal retains executing capture and retires other observers") {
  Fixture fixture;
  ui::Ref<Label> label;
  take(ui::mount(*fixture.scene, ui::label(label, "remove")));
  auto capture = std::make_shared<int>(42);
  const auto weak = std::weak_ptr(capture);
  bool held{};
  int skipped{};
  take(label.onLayout([&, capture](const NodeGeometry &) {
    take(fixture.scene->remove(label.node()));
    held = !weak.expired() && *capture == 42;
  }));
  take(label.onLayout([&] {
    ++skipped;
  }));
  capture.reset();
  fixture.layout();
  fixture.dispatch();
  CHECK(held);
  CHECK(weak.expired());
  CHECK(skipped == 0);
  CHECK(cause(label.geometry().error(), TreeError::STALE_NODE));
  CHECK_FALSE(fixture.scene->hasPendingLayout());
}

TEST_CASE("hide and show between final frames restarts equal geometry observation") {
  Fixture fixture;
  ui::Ref<Label> label;
  ui::Ref<Column> parent;
  take(ui::mount(*fixture.scene, ui::vbox(ui::label(label, "same")).reference(parent)));
  int calls{};
  take(label.onLayout([&] {
    ++calls;
  }));
  fixture.layout();
  fixture.dispatch();
  CHECK(calls == 1);
  take(fixture.app->tree().setVisible(parent.node(), false));
  take(fixture.app->tree().setVisible(parent.node(), true));
  fixture.layout();
  fixture.dispatch();
  CHECK(calls == 2);
}

TEST_CASE("scene close cancels later observers while holding executing captures") {
  Fixture fixture;
  ui::Ref<Label> label;
  take(ui::mount(*fixture.scene, ui::label(label, "close")));
  auto capture = std::make_shared<int>(7);
  const auto weak = std::weak_ptr(capture);
  int calls{};
  take(label.onLayout([&, capture] {
    ++calls;
    take(fixture.scene->close());
    CHECK_FALSE(weak.expired());
    CHECK(*capture == 7);
  }));
  take(label.onLayout([&] {
    ++calls;
  }));
  capture.reset();
  fixture.layout();
  fixture.dispatch();
  CHECK(calls == 1);
  CHECK(weak.expired());
  CHECK_FALSE(fixture.scene->hasPendingLayout());
}

TEST_CASE("runtime registration during dispatch waits for another dispatch") {
  Fixture fixture;
  ui::Ref<Label> label;
  take(ui::mount(*fixture.scene, ui::label(label, "late")));
  fixture.layout();
  int first{}, second{};
  take(label.onLayout([&] {
    ++first;
    take(label.onLayout([&] {
      ++second;
    }));
  }));
  // Registration on an already final widget is admitted by the post-dispatch hook.
  fixture.dispatch();
  CHECK(first == 0);
  fixture.dispatch();
  CHECK(first == 1);
  CHECK(second == 0);
  fixture.dispatch();
  CHECK(second == 1);
}

TEST_CASE("UI affinity precedes mutable geometry and registration access") {
  Fixture fixture;
  ui::Ref<Label> label;
  take(ui::mount(*fixture.scene, ui::label(label, "thread")));
  fixture.layout();
  std::optional<ManagedError> readError, bindError;
  std::thread worker([&] {
    readError = label.geometry().error();
    bindError = label
                  .onLayout([] {
                  })
                  .error();
  });
  worker.join();
  REQUIRE(readError);
  REQUIRE(bindError);
  CHECK(cause(*readError, TreeError::WRONG_THREAD));
  CHECK(cause(*bindError, TreeError::WRONG_THREAD));
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}

TEST_CASE("queue-full recovery retains newest snapshot and uses existing delivery budgets") {
  ApplicationOptions options;
  options.scheduler.data_capacity = 1;
  options.scheduler.data_budget = 1;
  Fixture fixture(options);
  ui::Ref<Label> label;
  take(ui::mount(*fixture.scene, ui::label(label, "queue")));
  std::vector<NodeGeometry> values;
  take(label.onLayout([&](const NodeGeometry &value) {
    values.push_back(value);
  }));
  std::optional<SchedulerConnection<int>> other;
  take(fixture.app->tree().withScope(label.node(), [&](const UiScope &scope) {
    other = take(fixture.app->scheduler().connect<int>(scope, [](std::shared_ptr<const int>) {
    }));
  }));
  take(other->publisher.publish(std::make_shared<const int>(1)));
  fixture.layout({20, 3});
  fixture.layout({22, 4});
  const auto newest = take(label.geometry());
  CHECK(values.empty());
  CHECK(fixture.dispatch().data == 1);
  CHECK(values.empty());
  CHECK(fixture.dispatch().data == 1);
  REQUIRE(values.size() == 1);
  CHECK(values.front().surface == newest.surface);
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}

TEST_CASE("multiple observers obey the existing per-dispatch data budget") {
  ApplicationOptions options;
  options.scheduler.data_budget = 1;
  Fixture fixture(options);
  ui::Ref<Label> label;
  take(ui::mount(*fixture.scene, ui::label(label, "budget")));
  int calls{};
  take(label.onLayout([&] {
    ++calls;
  }));
  take(label.onLayout([&] {
    ++calls;
  }));
  fixture.layout();
  CHECK(fixture.dispatch().data == 1);
  CHECK(calls == 1);
  CHECK(fixture.scene->hasPendingLayout());
  CHECK(fixture.dispatch().data == 1);
  CHECK(calls == 2);
  CHECK_FALSE(fixture.scene->hasPendingLayout());
}

TEST_CASE("observer admission is bounded and removal releases capture and registration capacity") {
  ManagedOptions options;
  options.maxLayoutHandlers = 1;
  Fixture fixture({}, options);
  ui::Ref<Label> label;
  take(ui::mount(*fixture.scene, ui::label(label, "one")));
  auto capture = std::make_shared<int>(1);
  const auto weak = std::weak_ptr(capture);
  take(label.onLayout([capture] {
  }));
  capture.reset();
  const auto rejected = label.onLayout([] {
  });
  REQUIRE_FALSE(rejected);
  CHECK(cause(rejected.error(), TreeError::NODE_LIMIT));
  take(fixture.scene->remove(label.node()));
  CHECK(weak.expired());
  ui::Ref<Label> replacement;
  take(ui::mount(*fixture.scene, ui::label(replacement, "two")));
  CHECK(replacement.onLayout([] {
  }));
  CHECK(cause(label.geometry().error(), TreeError::STALE_NODE));
}

TEST_CASE("preliminary layouts never publish and preparation failure leaves no partial notification") {
  for (const bool fail : {false, true}) {
    example::ReplayTerminal terminal({}, 50, {30, 6});
    const auto app = take(Application::create({}, terminal.clock()));
    const auto owner = take(ManagedScene::create(*app));
    ui::Ref<Label> label;
    take(ui::mount(*owner, ui::label(label, "before")));
    int calls{};
    take(label.onLayout([&] {
      ++calls;
    }));

    struct Prepared final : ApplicationScene {
      ManagedScene &owner;
      ui::Ref<Label> label;
      bool fail;
      int passes{};

      Prepared(ManagedScene &scene, ui::Ref<Label> reference, bool failure)
          : owner(scene), label(std::move(reference)), fail(failure) {
      }

      bool usesPreparedLayout() const noexcept override {
        return true;
      }

      std::expected<void, TreeError> prepare(Application &app, CellSize size) override {
        return owner.prepare(app, size);
      }

      std::expected<void, TreeError> prepareViews(Application &, const LayoutSnapshot &) override {
        ++passes;
        CHECK(cause(label.geometry().error(), TreeError::INVALID_GEOMETRY));
        if (fail) {
          return std::unexpected(TreeError::DEPTH_LIMIT);
        }
        if (passes == 1) {
          label.text() = "prepared";
        }
        return {};
      }

      std::expected<void, TreeError> completedLayout(Application &app) override {
        return owner.completedLayout(app);
      }

      std::expected<void, TreeError> afterDispatch(Application &app) override {
        return owner.afterDispatch(app);
      }

      bool hasPendingLayout() const noexcept override {
        return owner.hasPendingLayout();
      }
    } scene(*owner, label, fail);

    const auto result = app->run(scene, terminal);
    if (fail) {
      REQUIRE_FALSE(result);
      CHECK(std::get<TreeError>(result.error().cause) == TreeError::DEPTH_LIMIT);
      CHECK(calls == 0);
    } else {
      REQUIRE(result);
      CHECK(scene.passes == 2);
      CHECK(calls == 1);
      CHECK(result->frames == 1);
    }
  }
}

TEST_CASE("permanent queue starvation cancels once and a new final frame can retry") {
  ApplicationOptions options;
  options.scheduler.data_capacity = 1;
  options.scheduler.data_budget = 1;
  Fixture fixture(options);
  ui::Ref<Label> label;
  take(ui::mount(*fixture.scene, ui::label(label, "starved")));
  int calls{};
  take(label.onLayout([&] {
    ++calls;
  }));
  std::optional<SchedulerConnection<int>> other;
  bool refill = true;
  take(fixture.app->tree().withScope(label.node(), [&](const UiScope &scope) {
    other = take(fixture.app->scheduler().connect<int>(scope, [&](std::shared_ptr<const int> value) {
      if (refill) {
        take(other->publisher.publish(std::move(value)));
      }
    }));
  }));
  take(other->publisher.publish(std::make_shared<const int>(1)));
  fixture.layout();
  fixture.dispatch();
  fixture.dispatch();
  CHECK_FALSE(fixture.scene->hasPendingLayout());
  CHECK(calls == 0);
  const auto errors = take(fixture.scene->diagnostics());
  REQUIRE(errors.errors.size() == 1);
  const auto *application = std::get_if<ApplicationError>(&errors.errors.front().error.cause);
  REQUIRE(application);
  CHECK(std::get<SchedulerError>(application->cause) == SchedulerError::WOULD_BLOCK);
  refill = false;
  fixture.dispatch();
  fixture.layout();
  fixture.dispatch();
  CHECK(calls == 1);
}

TEST_CASE("scene pipe delivery uses currentScene and actual final runner frames") {
  example::ReplayTerminal terminal({{50, "", CellSize{40, 8}}}, 100, {30, 6});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  ui::Ref<Label> label;
  int calls{};
  std::vector<CellSize> sizes;
  auto scene = ui::scene(ui::label(label, "runner"), options) | ui::onLayout(label, [&](const NodeGeometry &geometry) {
                 ++calls;
                 CHECK_FALSE(ui::detail::CurrentSceneScope::snapshot().expired());
                 CHECK(take(label.geometry()).surface == geometry.surface);
                 sizes.push_back(geometry.local.size());
               });
  const auto result = scene.run(terminal);
  REQUIRE(result);
  CHECK(terminal.closed);
  CHECK(calls == 2);
  REQUIRE(sizes.size() == 2);
  CHECK(sizes.front() != sizes.back());
  CHECK(result->frames == 2);
  CHECK(ui::detail::CurrentSceneScope::snapshot().expired());
}

TEST_CASE("callback failure preserves typed primary cause and reports exactly once") {
  example::ReplayTerminal terminal({}, 50);
  ui::SceneOptions options;
  options.clock = terminal.clock();
  ui::Ref<Label> label;
  int errors{}, calls{};
  auto scene =
    ui::scene(ui::label(label, "failure"), options) | ui::onLayout(label, [&]() -> std::expected<void, TreeError> {
      ++calls;
      return std::unexpected(TreeError::DEPTH_LIMIT);
    });
  scene.onError([&](const ManagedDiagnostic &) {
    ++errors;
  });
  const auto result = scene.run(terminal);
  REQUIRE_FALSE(result);
  CHECK(cause(result.error(), TreeError::DEPTH_LIMIT));
  CHECK(result.error().phase == ManagedPhase::LAYOUT);
  CHECK(calls == 1);
  CHECK(errors == 1);
}

TEST_CASE("EOF delivers final observations and preserves the existing frame cap for callback mutations") {
  example::ReplayTerminal terminal({}, 10, {30, 6});
  ui::SceneOptions options;
  options.clock = terminal.clock();
  ui::Ref<Label> label;
  int calls{};
  std::string changed;
  auto scene =
    ui::scene(ui::vbox(ui::label(label, "before")), options) | ui::onLayout(label, [&](const NodeGeometry &) {
      if (++calls == 1) {
        label.text() = "after";
        changed = take(label.text().get());
      }
    });
  const auto result = scene.run(terminal);
  REQUIRE(result);
  CHECK(result->frames == 1);
  CHECK(calls == 1);
  CHECK(changed == "after");
  CHECK(result->iterations <= 3);
}

TEST_CASE("onLayout exceptions map through the runner and release owned callbacks") {
  for (const bool memoryFailure : {false, true}) {
    CAPTURE(memoryFailure);
    example::ReplayTerminal terminal({}, 50);
    ui::SceneOptions options;
    options.clock = terminal.clock();
    ui::Ref<Label> label;
    auto capture = std::make_shared<int>(1);
    const std::weak_ptr<int> lifetime = capture;
    int errors{}, calls{}, stopped{};
    auto scene = ui::scene(ui::label(label, "throwing observer"), options) | ui::onLayout(label, [&, keep = capture] {
                   ++calls;
                   CHECK(*keep == 1);
                   if (memoryFailure) {
                     throw std::bad_alloc();
                   }
                   throw std::runtime_error("layout observer failure");
                 });
    capture.reset();
    scene.onError([&](const ManagedDiagnostic &) {
      ++errors;
    });
    scene.onStop([&] {
      ++stopped;
    });

    const auto result = scene.run(terminal);

    REQUIRE_FALSE(result);
    CHECK(cause(result.error(), memoryFailure ? TreeError::NO_MEMORY : TreeError::CALLBACK_FAILED));
    CHECK(result.error().phase == ManagedPhase::LAYOUT);
    CHECK(calls == 1);
    CHECK(errors == 1);
    CHECK(stopped == 1);
    CHECK(terminal.closed);
    CHECK(lifetime.expired());
    CHECK_FALSE(label.geometry());
    CHECK(ui::detail::CurrentSceneScope::snapshot().expired());
  }
}

TEST_CASE("cold final sizing is intersected with theme bounds at managed preparation") {
  for (const bool fitFirst : {false, true}) {
    CAPTURE(fitFirst);
    Fixture fixture;
    ui::Ref<Label> child;

    take(fixture.scene->theme(take(parsePss("Label { min-width: 30; }"))));

    auto recipe = ui::label(child, "small");

    if (fitFirst) {
      recipe = std::move(recipe).fitWidth().width(20).maxWidth(40);
    } else {
      recipe = std::move(recipe).width(20).minWidth(12).maxWidth(32).fitWidth();
    }

    take(ui::mount(*fixture.scene, ui::vbox(std::move(recipe))));
    fixture.layout({80, 8});
    CHECK(take(child.geometry()).local.size().width == 30);
  }

  Fixture fixture;
  ui::Ref<Label> invalid;

  take(fixture.scene->theme(take(parsePss("Label { min-width: 30; }"))));

  take(ui::mount(*fixture.scene, ui::label(invalid, "small").maxWidth(40).fitWidth().width(20)));

  const auto failed = fixture.scene->prepare(*fixture.app, {80, 8});

  REQUIRE_FALSE(failed);
  CHECK(failed.error() == TreeError::INVALID_ARGUMENT);
  const auto diagnostics = take(fixture.scene->diagnostics());

  REQUIRE_FALSE(diagnostics.errors.empty());
  const auto *style = std::get_if<StyleError>(&diagnostics.errors.back().error.cause);

  REQUIRE(style);
  CHECK(style->cause == TreeError::INVALID_ARGUMENT);
  bool installed = true;

  take(fixture.app->tree().read(invalid.node(), [&](const Label &widget) {
    installed = static_cast<bool>(widget.computedStyle());
  }));
  CHECK_FALSE(installed);
}

TEST_CASE("framed content menu follows caption changes up to its cap and stays idle afterwards") {
  Fixture fixture;
  ui::Ref<Menu> menu;
  ui::Ref<Border> outer;
  ui::ActionSet actions;
  auto save = actions.action("save", "Save", [] {
  });

  take(ui::mount(*fixture.scene,
                 ui::vbox(ui::menu(menu, {save}).fitWidth().minWidth(10).maxWidth(16).framed(outer, "rounded"))));
  fixture.layout({60, 10});
  const auto initial = take(outer.geometry()).local.size();

  CHECK(initial.width == 10);
  auto metadata = *take(save.snapshot());

  metadata.caption = "A much longer caption";
  REQUIRE(save.set(metadata));
  fixture.layout({60, 10});
  CHECK(take(outer.geometry()).local.size().width == 16);
  CHECK(take(menu.geometry()).clip.size().width == 14);

  metadata.caption = "Save";
  REQUIRE(save.set(metadata));
  fixture.layout({60, 10});
  CHECK(take(outer.geometry()).local.size() == initial);
  take(fixture.scene->prepare(*fixture.app, {60, 10}));
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
}
