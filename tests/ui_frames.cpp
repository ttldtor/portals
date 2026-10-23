// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/ui.hpp>

#include <algorithm>
#include <array>
#include <source_location>
#include <type_traits>

namespace {
using namespace portals;

constexpr BorderOptions LEGACY_BORDER{PrimitiveGlyphs::ASCII, 123};
constexpr ScrollAreaOptions LEGACY_SCROLL{.frame = LEGACY_BORDER};
static_assert(LEGACY_BORDER.maxPaintCells == 123 && LEGACY_SCROLL.frame->maxPaintCells == 123);
static_assert(std::is_aggregate_v<BorderOptions> && std::is_aggregate_v<ScrollAreaOptions>);
static_assert(std::same_as<decltype(ui::button("Save").framed("rounded")), ui::Description<Button> &&>);

template <class T>
concept CanFrame = requires(T &&recipe) { std::forward<T>(recipe).framed("single"); };
static_assert(CanFrame<ui::Description<Button>> && CanFrame<ui::Description<LineEdit>>);
static_assert(!CanFrame<ui::Description<Widget>> && !CanFrame<ui::Description<Dialog>>);
static_assert(!CanFrame<ui::Description<Button> &>);

template <class T>
concept CanScrollOnFrame = requires(T &&recipe) { std::forward<T>(recipe).scrollOnFrame(); };
static_assert(CanScrollOnFrame<ui::Description<ScrollArea>>);
static_assert(!CanScrollOnFrame<ui::Description<Button>> && !CanScrollOnFrame<ui::Description<ScrollArea> &>);
static_assert(std::same_as<decltype(ui::button("Save").framed("rounded", "Actions")), ui::Description<Button> &&>);

template <class T, class E>
T take(std::expected<T, E> result, std::source_location origin = std::source_location::current()) {
  INFO("caller: " << std::string_view(origin.file_name()) << ':' << origin.line());

  if (!result) {
    if constexpr (std::same_as<E, ManagedError>) {
      ui::SceneReport report;
      report.failure = result.error();
      ui::reportToStderr(report);
    } else if constexpr (std::same_as<E, TreeError>) {
      INFO("TreeError: " << static_cast<int>(result.error()));
      REQUIRE(result);
    }
  }

  REQUIRE(result);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

void layout(ManagedScene &scene, Application &app, CellSize size = {30, 12}) {
  take(scene.prepare(app, size));
  const auto viewport = take(CellRect::from({}, size));

  take(app.tree().frame([&] {
    take(layoutTree(app.tree(), app.root(), viewport, viewport));
  }));
  take(scene.completedLayout(app));
  take(app.tree().consumeFrameRequest());
}

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));

  void arrange(CellSize size = {30, 12}) {
    layout(*scene, *app, size);
  }
};

struct FrameSink final : PaintContext {
  std::vector<std::string> text;
  std::optional<CellStyle> style;

  std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
    for (const auto &command : commands) {
      if (const auto *run = std::get_if<TextCommand>(&command)) {
        text.emplace_back(run->text.bytes());
        style = run->style;
      }
    }

    return {};
  }
};

FrameSink paint(WidgetTree &tree, Node<Widget> node) {
  FrameSink sink;

  take(tree.frame([&] {
    take(tree.paint(node, sink));
  }));

  return sink;
}

SizePolicy policy(WidgetTree &tree, Node<SizedWidget> node) {
  SizePolicy result;

  take(tree.read(node, [&](const SizedWidget &widget) {
    result = widget.sizePolicy();
  }));

  return result;
}
} // namespace

TEST_CASE("framed dimensions retain source types and route ordered size changes to the outer Border") {
  Fixture fixture;
  ui::Ref<Button> child;
  ui::Ref<Border> outer;
  auto recipe =
    ui::button(child, "Save").width(7).height(3).framed(outer, "rounded").width(12).flexX(3) | ui::height(6);
  ui::Ref<Column> host;
  take(ui::mount(*fixture.scene, ui::vbox(std::move(recipe)).reference(host)));

  CHECK(take(fixture.app->tree().inspect(outer.node())).parent == host.node());
  CHECK(take(fixture.app->tree().inspect(child.node())).parent == outer.node());
  const auto framePolicy = policy(fixture.app->tree(), outer.node());
  const auto childPolicy = policy(fixture.app->tree(), child.node());

  CHECK(framePolicy.width.minimum == 12);
  CHECK(framePolicy.width.maximum == 12);
  CHECK(framePolicy.width.mode == SizeMode::FLEX);
  CHECK(framePolicy.width.weight == 3);
  CHECK(framePolicy.height.preferred == 6);
  CHECK(framePolicy.height.mode == SizeMode::FIXED);
  CHECK(childPolicy.width.mode != SizeMode::FIXED);
  CHECK(childPolicy.height.mode != SizeMode::FIXED);
  fixture.arrange();
  CHECK(take(outer.geometry()).local.size() == CellSize{12, 6});
  CHECK(take(child.geometry()).local.origin() == CellPoint{1, 1});
  CHECK(take(child.geometry()).local.size() == CellSize{10, 1});
}

TEST_CASE("compact presets and pipe spelling paint the selected owning frame") {
  constexpr std::array<std::pair<std::string_view, std::string_view>, 6> PRESETS{{{"single", "\xe2\x94\x8c"},
                                                                                  {"plain", "\xe2\x94\x8c"},
                                                                                  {"rounded", "\xe2\x95\xad"},
                                                                                  {"double", "\xe2\x95\x94"},
                                                                                  {"heavy", "\xe2\x94\x8f"},
                                                                                  {"ascii", "+"}}};

  for (const auto &[name, corner] : PRESETS) {
    INFO(name);
    Fixture fixture;
    ui::Ref<Button> child;
    ui::Ref<Border> outer;

    take(ui::mount(*fixture.scene,
                   ui::vbox(ui::button(child, "x") | ui::width(8) | ui::framed(outer, name) | ui::height(4))));
    fixture.arrange();
    const auto painted = paint(fixture.app->tree(), outer.node());

    CHECK(std::ranges::find(painted.text, corner) != painted.text.end());
    CHECK(take(child.geometry()).local.size() == CellSize{6, 1});
  }
}

TEST_CASE("titled compact frames match options while owning strings and retaining hit identity") {
  for (const auto pattern : {"single", "plain", "double", "heavy", "rounded", "ascii"}) {
    std::vector<std::string> expectedPaint;

    for (int spelling = 0; spelling != 3; ++spelling) {
      INFO(std::string_view(pattern) << ": " << spelling);
      Fixture fixture;
      ui::Ref<Button> child;
      ui::Ref<Border> outer;
      int clicks{};
      auto recipe = ui::button(child, "Save")
                      .onClick([&] {
                        ++clicks;
                      })
                      .width(16);
      std::string title = "Actions";
      std::string name = pattern;

      if (spelling == 0) {
        FrameOptions options;
        options.pattern = *ui::detail::framePattern(name);
        options.title = title;
        recipe = std::move(recipe).framed(outer, std::move(options));
      } else if (spelling == 1) {
        recipe = std::move(recipe).framed(outer, name, title);
      } else {
        auto modifier = ui::framed(outer, name, title);

        name = "unknown";
        title = "changed";
        recipe = std::move(recipe) | std::move(modifier);
      }

      title = "changed again";
      take(ui::mount(*fixture.scene, ui::vbox(std::move(recipe).height(4))));
      fixture.arrange();
      const auto painted = paint(fixture.app->tree(), outer.node());

      if (spelling == 0) {
        expectedPaint = painted.text;
      } else {
        CHECK(painted.text == expectedPaint);
      }

      CHECK(std::ranges::find(painted.text, "Actions") != painted.text.end());
      CHECK(take(outer.geometry()).local.size() == CellSize{16, 4});
      CHECK(take(child.geometry()).local.size() == CellSize{14, 1});
      CHECK(take(fixture.app->tree().inspect(child.node())).parent == outer.node());
      take(fixture.app->router().invoke(child.node(), {Action::ACTIVATE}));
      CHECK(clicks == 1);
    }
  }
}

TEST_CASE("scrollOnFrame matches options without a second frame or inset") {
  for (int spelling = 0; spelling != 3; ++spelling) {
    INFO(spelling);
    Fixture fixture;
    ui::Ref<ScrollArea> area;
    ui::Ref<Button> child;
    auto recipe = ui::scrollArea(area, ui::button(child, "content").width(30).height(20));

    if (spelling == 0) {
      FrameOptions options;
      options.pattern = FramePattern::ROUNDED;
      options.title = "Sections";
      recipe = std::move(recipe).framed(std::move(options)).placement(ScrollPlacement::ON_FRAME);
    } else if (spelling == 1) {
      recipe = std::move(recipe).framed("rounded", "Sections").scrollOnFrame();
    } else {
      recipe = std::move(recipe) | ui::framed("rounded", "Sections") | ui::scrollOnFrame();
    }

    take(ui::mount(*fixture.scene, ui::vbox(std::move(recipe).width(16).height(6))));
    fixture.arrange();
    const auto layout = take(scrollAreaLayout(fixture.app->tree(), area.node()));
    const auto painted = paint(fixture.app->tree(), area.node());

    CHECK(take(area.geometry()).local.size() == CellSize{16, 6});
    CHECK(layout.placement.viewport == CellSize{14, 4});
    CHECK(layout.placement.viewportOrigin == CellPoint{1, 1});
    CHECK(layout.placement.horizontalBar);
    CHECK(layout.placement.verticalBar);
    CHECK_FALSE(layout.placement.hasCorner);
    CHECK(take(fixture.app->tree().inspect(child.node())).parent == area.node());
    CHECK(std::ranges::find(painted.text, "Sections") != painted.text.end());
  }
}

TEST_CASE("compact title and on-frame shortcuts retain typed pre-publication failures") {
  for (int failure = 0; failure != 6; ++failure) {
    INFO(failure);
    Fixture fixture;
    ui::Ref<ScrollArea> area;
    ui::Ref<Button> child;
    auto recipe = ui::scrollArea(area, ui::button(child, "content"));

    if (failure == 0) {
      recipe = std::move(recipe).framed("unknown", "Sections");
    } else if (failure == 1) {
      recipe = std::move(recipe) | ui::framed("unknown", "Sections");
    } else if (failure == 2) {
      recipe = std::move(recipe).framed("rounded", std::string_view("\xff", 1));
    } else if (failure == 3) {
      recipe = std::move(recipe) | ui::framed("rounded", std::string(FrameOptions{}.maxTitleBytes + 1, 'x'));
    } else if (failure == 4) {
      recipe = std::move(recipe).scrollOnFrame();
    } else {
      recipe = std::move(recipe) | ui::scrollOnFrame();
    }

    const auto result = ui::mount(*fixture.scene, std::move(recipe));

    REQUIRE_FALSE(result);
    CHECK(std::get<TreeError>(result.error().cause) == TreeError::INVALID_ARGUMENT);
    CHECK_FALSE(result.error().cleanup);
    CHECK(area.node() == Node<ScrollArea>{});
    CHECK(child.node() == Node<Button>{});
    CHECK(take(fixture.app->tree().inspect(fixture.scene->root())).children.empty());
  }
}

TEST_CASE("framed LineEdit retains its editable model and an optional outer observer") {
  Fixture fixture;
  const auto model = take(fixture.scene->state(std::string("draft")));
  ui::Ref<LineEdit> editor;
  ui::Ref<Border> outer;

  take(ui::mount(*fixture.scene,
                 ui::vbox(ui::lineEdit(model).reference(editor).height(4).framed(outer, "ascii").width(14))));
  fixture.arrange();
  auto changed = editor.text() = "changed";

  take(changed.result());
  CHECK(take(editor.text().get()) == "changed");
  CHECK(take(fixture.app->tree().inspect(editor.node())).parent == outer.node());
  const auto stale = outer.geometry();

  REQUIRE_FALSE(stale);
  CHECK(std::get<TreeError>(stale.error().cause) == TreeError::INVALID_GEOMETRY);
  fixture.arrange();
  const auto size = take(outer.geometry()).local.size();

  CHECK(size.width == 14);
  CHECK(size.height == 4);
}

TEST_CASE("erasure and nested Grid distinguish actual outer cells from source container children") {
  Fixture fixture;
  ui::Ref<Grid> grid, inner;
  ui::Ref<Column> column;
  ui::Ref<Button> button;
  ui::Ref<Label> value;
  ui::Ref<Border> columnFrame, gridFrame;
  auto erased = ui::detail::Access::erase(ui::vbox(ui::button(button, "left"))
                                            .reference(column)
                                            .width(8)
                                            .height(4)
                                            .framed(columnFrame, "ascii")
                                            .row(0)
                                            .column(0));
  auto nested = ui::grid(ui::label(value, "right"))
                  .columns(1)
                  .reference(inner)
                  .framed(gridFrame, "rounded")
                  .width(8)
                  .height(4)
                  .row(0)
                  .column(1);

  take(ui::mount(*fixture.scene, ui::grid(std::move(erased), std::move(nested)).columns(2).reference(grid)));
  fixture.arrange();
  const auto snapshot = take(fixture.app->tree().inspect(grid.node()));

  REQUIRE(snapshot.children.size() == 2);
  CHECK(snapshot.children[0] == columnFrame.node());
  CHECK(snapshot.children[1] == gridFrame.node());
  CHECK(take(fixture.app->tree().inspect(button.node())).parent == column.node());
  CHECK(take(fixture.app->tree().inspect(value.node())).parent == inner.node());
  take(fixture.app->tree().read(grid.node(), [&](const Grid &widget) {
    REQUIRE(widget.options().cells.size() == 2);
    CHECK(widget.options().cells[0].node == columnFrame.node().id());
    CHECK(widget.options().cells[1].node == gridFrame.node().id());
  }));
  take(fixture.app->tree().read(inner.node(), [&](const Grid &widget) {
    REQUIRE(widget.options().cells.size() == 1);
    CHECK(widget.options().cells.front().node == value.node().id());
  }));
  CHECK(take(columnFrame.geometry()).local.size() == CellSize{8, 4});
  CHECK(take(gridFrame.geometry()).local.size() == CellSize{8, 4});
  CHECK(take(inner.geometry()).local.size() == CellSize{6, 1});
}

TEST_CASE("frame validation and child setup failure roll back ancestors before publishing references") {
  Fixture fixture;
  take(ui::mount(*fixture.scene, ui::label("keep")));
  const auto before = take(fixture.app->tree().inspect(fixture.scene->root())).children;

  for (int failure = 0; failure != 3; ++failure) {
    INFO(failure);
    ui::Ref<Button> child;
    ui::Ref<Border> outer;
    int setups{};
    FrameOptions invalid;
    invalid.customGlyphs = FrameGlyphs{"xx", "+", "+", "+", "-", "|"};
    auto recipe =
      ui::button(child, "fail").setup([&](ManagedScene &, Node<Button> node) -> std::expected<void, ManagedError> {
        ++setups;

        if (failure == 2) {
          return std::unexpected(ManagedError{ManagedPhase::MOUNT, node.id(), TreeError::INVALID_ARGUMENT, {}, {}});
        }

        return {};
      });

    if (failure == 0) {
      recipe = std::move(recipe).framed(outer, "unknown");
    } else if (failure == 1) {
      recipe = std::move(recipe).framed(outer, invalid);
    } else {
      recipe = std::move(recipe).framed(outer, "ascii");
    }

    const auto result = ui::mount(*fixture.scene, std::move(recipe));

    REQUIRE_FALSE(result);
    CHECK(std::get<TreeError>(result.error().cause) == TreeError::INVALID_ARGUMENT);
    CHECK_FALSE(result.error().cleanup);
    CHECK(setups == (failure == 2 ? 1 : 0));
    CHECK(child.node() == Node<Button>{});
    CHECK(outer.node() == Node<Border>{});
    CHECK(take(fixture.app->tree().inspect(fixture.scene->root())).children == before);
  }
}

TEST_CASE("child self removal leaves its frame and explicit outer removal retires descendants") {
  Fixture fixture;
  ui::Ref<Button> child;
  ui::Ref<Border> outer;
  int clicks{};

  take(ui::mount(*fixture.scene, ui::button(child, "remove")
                                   .onClick([&] {
                                     ++clicks;
                                     take(fixture.scene->remove(child.node()));
                                   })
                                   .framed(outer, "ascii")));
  fixture.arrange();
  const auto childNode = child.node();

  take(fixture.app->router().invoke(childNode, {Action::ACTIVATE}));
  CHECK(clicks == 1);
  CHECK_FALSE(fixture.app->tree().inspect(childNode));
  CHECK(take(fixture.app->tree().inspect(outer.node())).children.empty());
  take(fixture.scene->remove(outer.node()));
  CHECK(take(fixture.app->tree().inspect(fixture.scene->root())).children.empty());

  ui::Ref<Button> second;
  ui::Ref<Border> secondOuter;
  take(ui::mount(*fixture.scene, ui::button(second, "both").framed(secondOuter)));
  const auto secondNode = second.node();
  const auto secondOuterNode = secondOuter.node();

  take(fixture.scene->remove(secondOuterNode));
  CHECK_FALSE(fixture.app->tree().inspect(secondNode));
  CHECK_FALSE(fixture.app->tree().inspect(secondOuterNode));
}

TEST_CASE("framed mount flags address outer while original child visibility remains local") {
  Fixture fixture;
  ui::Ref<Button> child;
  ui::Ref<Border> outer;

  take(ui::mount(*fixture.scene, ui::button(child, "flags").visible(false).framed(outer).enabled(false)));
  CHECK_FALSE(take(fixture.app->tree().inspect(outer.node())).visible);
  CHECK_FALSE(take(fixture.app->tree().inspect(outer.node())).enabled);
  CHECK(take(fixture.app->tree().inspect(child.node())).visible);
  take(fixture.app->tree().setVisible(outer.node(), true));
  take(fixture.app->tree().setVisible(child.node(), false));
  CHECK(take(fixture.app->tree().inspect(outer.node())).visible);
  CHECK_FALSE(take(fixture.app->tree().inspect(child.node())).visible);
}

TEST_CASE("managed framed appearance follows descendant focus theme changes and idle preparation") {
  Fixture fixture;
  ui::Ref<Button> child, other;
  ui::Ref<Border> outer;
  take(ui::mount(*fixture.scene, ui::vbox(ui::button(child, "inside")
                                            .onClick([] {
                                            })
                                            .framed(outer, "ascii")
                                            .width(12)
                                            .height(4),
                                          ui::button(other, "outside").onClick([] {
                                          }))));
  StyleSheet theme;

  for (const auto state : {StyleState::NORMAL, StyleState::FOCUSED, StyleState::DISABLED}) {
    StyleRule rule;
    rule.selector.type = "Border";
    rule.selector.state = state;
    rule.declarations.foreground = state == StyleState::NORMAL    ? RgbColor{1, 2, 3}
                                   : state == StyleState::FOCUSED ? RgbColor{4, 5, 6}
                                                                  : RgbColor{7, 8, 9};
    theme.rules.push_back(rule);
  }

  take(fixture.scene->theme(theme));
  fixture.arrange();
  take(other.focus());
  fixture.arrange();
  REQUIRE(paint(fixture.app->tree(), outer.node()).style);
  CHECK(paint(fixture.app->tree(), outer.node()).style->foreground == RgbColor{1, 2, 3});
  take(child.focus());
  fixture.arrange();
  CHECK(take(fixture.app->router().focused()) == child.node());
  CHECK(paint(fixture.app->tree(), outer.node()).style->foreground == RgbColor{4, 5, 6});
  take(fixture.scene->prepare(*fixture.app, {30, 12}));
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  take(fixture.app->tree().setEnabled(outer.node(), false));
  fixture.arrange();
  CHECK(paint(fixture.app->tree(), outer.node()).style->foreground == RgbColor{7, 8, 9});
  theme.rules.back().declarations.foreground = RgbColor{9, 8, 7};
  take(fixture.scene->theme(theme));
  fixture.arrange();
  CHECK(paint(fixture.app->tree(), outer.node()).style->foreground == RgbColor{9, 8, 7});
}

TEST_CASE("resource compact frame preserves source parents and forwards its actual Grid root") {
  ui::SceneResourceOptions options;
  options.format = ui::SceneResourceFormat::CONFIGURATION;
  auto owner = ui::scene(R"(scene { Grid#grid{columns=2
    VBox#group{frame="ascii" width=12 height=6 Button#save{"Save" frame="rounded" width=10 height=4}}
    Grid#inner{columns=1 frame="double" width=8 height=4 Label#value{"value"}}
  } })",
                         options);
  const auto grid = owner.get<Grid>("grid");
  const auto group = owner.get<Column>("group");
  const auto save = owner.get<Button>("save");
  const auto inner = owner.get<Grid>("inner");
  const auto value = owner.get<Label>("value");
  take(owner.prepareChecked());
  auto &scene = take(owner.managed()).get();
  auto &app = ui::detail::SceneAccess::application(scene);
  layout(scene, app);
  const auto groupOuter = take(app.tree().inspect(group.node())).parent;
  const auto innerOuter = take(app.tree().inspect(inner.node())).parent;
  const auto saveOuter = take(app.tree().inspect(save.node())).parent;

  CHECK(take(app.tree().inspect(saveOuter)).parent == group.node());
  CHECK(take(app.tree().inspect(value.node())).parent == inner.node());
  take(app.tree().read(grid.node(), [&](const Grid &widget) {
    REQUIRE(widget.options().cells.size() == 2);
    CHECK(widget.options().cells[0].node == groupOuter.id());
    CHECK(widget.options().cells[1].node == innerOuter.id());
  }));
  CHECK(take(app.tree().inspect(saveOuter)).geometry.local.size() == CellSize{10, 4});
  CHECK(take(save.geometry()).local.size() == CellSize{8, 1});
  take(owner.close());

  Fixture fixture;
  ui::Ref<Grid> host;
  take(ui::mount(*fixture.scene,
                 ui::grid(ui::resource(R"(scene { Button{"Forwarded" frame="ascii" width=12 height=4} })", options))
                   .columns(1)
                   .reference(host)));
  fixture.arrange();
  const auto children = take(fixture.app->tree().inspect(host.node())).children;

  REQUIRE(children.size() == 1);
  CHECK(take(fixture.app->tree().inspect(children.front())).children.size() == 1);
  take(fixture.app->tree().read(host.node(), [&](const Grid &widget) {
    REQUIRE(widget.options().cells.size() == 1);
    CHECK(widget.options().cells.front().node == children.front().id());
  }));
}
