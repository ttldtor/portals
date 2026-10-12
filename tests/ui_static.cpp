// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "../examples/replay/terminal.hpp"
#include <portals/ui_static.hpp>

#include <thread>

namespace {
using namespace portals;

constexpr LinearOptions FRAME{.gap = 1, .padding = {1, 1, 1, 1}};
constexpr ControlStyle CAPTION_STYLE{.normal = {.foreground = RgbColor{30, 180, 240}, .background = {}, .bold = true}};
constexpr SizePolicy FIELD_SIZE{.width = {.minimum = 4, .mode = SizeMode::FLEX, .weight = 2}, .height = {}};
constexpr auto FORM = ui::ct::scene(
  ui::ct::column<"form">(FRAME, ui::ct::label("Name", CAPTION_STYLE),
                         ui::ct::row(ui::ct::lineEdit<"field">("Portals", EditMode::IMMEDIATE, {}, FIELD_SIZE),
                                     ui::ct::button<"apply">("Apply")),
                         ui::ct::label<"status">("Ready")));

static_assert(FORM.root().NODE_COUNT == 6);
static_assert(FORM.root().linearOptions().padding.left == 1);
static_assert(std::get<0>(FORM.root().children()).text() == "Name");
static_assert(std::get<0>(FORM.root().children()).style().normal.bold);
static_assert(std::get<0>(std::get<1>(FORM.root().children()).children()).sizePolicy().width.weight == 2);
static_assert(std::get<0>(std::get<1>(FORM.root().children()).children()).editMode() == EditMode::IMMEDIATE);
static_assert(ui::ct::scene(ui::ct::column(ui::ct::label(""), ui::ct::label(""))).root().NODE_COUNT == 3);
static_assert(ui::ct::scene(ui::ct::row()).root().NODE_COUNT == 1);

consteval auto localBytes() {
  const char text[]{'o', 'w', 'n', 'e', 'd', '\0'};

  return ui::ct::scene(ui::ct::label(text));
}

constexpr auto OWNED = localBytes();
static_assert(OWNED.root().text() == "owned");
constexpr auto INVALID_UNICODE = ui::ct::scene(ui::ct::column(ui::ct::label<"good">("Good"), ui::ct::label("\xff")));
static_assert(std::get<1>(INVALID_UNICODE.root().children()).text().size() == 1);

template <class T, class E> T take(std::expected<T, E> result) {
  REQUIRE(result);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

bool treeCause(const ManagedError &failure, TreeError cause) {
  const auto *value = std::get_if<TreeError>(&failure.cause);

  return value && *value == cause;
}

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> owner = take(ManagedScene::create(*app));
};

struct Capture {
  bool &destroyed;

  ~Capture() {
    destroyed = true;
  }
};

auto detachedRecipe(ui::Ref<Label> reference) {
  // The static value and its bytes cease to exist before the caller mounts this recipe.
  constexpr auto LOCAL = ui::ct::scene(ui::ct::label<"owned">("owned"));

  return ui::ct::describe(LOCAL, ui::ct::reference<"owned">(reference));
}

struct Outcome {
  std::string output;
  std::size_t frames{};
  std::string applied;
  int clicks{};
};

Outcome replay(bool compileTime) {
  example::ReplayTerminal terminal({{50,
                                     "\x01"
                                     "Ada\t\r",
                                     {}},
                                    {100, "", CellSize{36, 8}}},
                                   150, {40, 10});
  ui::SceneOptions options;
  ui::Ref<LineEdit> field;
  ui::Ref<Label> status;
  Outcome outcome;

  options.clock = terminal.clock();

  auto action = [field, status, &outcome] {
    auto value = field.text().get();

    REQUIRE(value);

    outcome.applied = *value;
    ++outcome.clicks;

    take(status.text().set(*value).result());
  };
  auto recipe = compileTime ? ui::ct::describe(FORM, ui::ct::reference<"field">(field),
                                               ui::ct::reference<"status">(status), ui::ct::onClick<"apply">(action))
                            : ui::column(FRAME, ui::label("Name", CAPTION_STYLE),
                                         ui::row(ui::lineEdit(field, "Portals", {.policy = FIELD_SIZE}).id("field"),
                                                 ui::button("Apply", action).id("apply")),
                                         ui::label(status, "Ready").id("status"))
                                .id("form");
  auto scene = ui::scene(std::move(recipe), options);
  StyleSheet theme;
  StyleRule buttonStyle;

  buttonStyle.selector.id = "apply";
  buttonStyle.declarations.foreground = RgbColor{240, 200, 20};
  theme.rules.push_back(buttonStyle);

  scene.theme(theme);

  const auto result = take(scene.run(terminal));

  CHECK(terminal.closed);
  CHECK(!field.text().get());
  outcome.output = terminal.output;
  outcome.frames = result.frames;

  return outcome;
}
} // namespace

TEST_CASE("static scene matches dynamic text, layout, styles and replay") {
  const auto before = replay(false);
  const auto after = replay(true);

  CHECK(before.clicks == 1);
  CHECK(after.clicks == before.clicks);
  CHECK(after.applied == "Ada");
  CHECK(after.applied == before.applied);
  CHECK(after.frames == before.frames);
  CHECK(after.output == before.output);
}

TEST_CASE("runtime recipe owns static bytes after descriptor destruction") {
  Fixture fixture;
  ui::Ref<Label> label;
  auto recipe = detachedRecipe(label);

  take(ui::mount(*fixture.owner, std::move(recipe)));

  CHECK(take(label.text().get()) == "owned");
}

TEST_CASE("invalid static Unicode follows runtime rollback and never publishes refs") {
  Fixture fixture;
  ui::Ref<Label> good;
  const auto existing = take(fixture.owner->emplace<Widget>(fixture.owner->root()));
  const auto result = ui::mount(*fixture.owner, ui::ct::describe(INVALID_UNICODE, ui::ct::reference<"good">(good)));

  REQUIRE_FALSE(result);
  CHECK(treeCause(result.error(), TreeError::INVALID_ARGUMENT));
  CHECK(result.error().text == GraphemeText::from("\xff").error());
  CHECK(result.error().phase == ManagedPhase::MOUNT);
  CHECK(good.node() == Node<Label>{});
  CHECK(take(fixture.app->tree().inspect(fixture.owner->root())).children == std::vector<Node<Widget>>{existing});
}

TEST_CASE("node limits roll back static subtree and destroy unmounted callbacks") {
  auto app = take(Application::create());
  auto owner = take(ManagedScene::create(*app, {.maxNodes = 2}));
  ui::Ref<Button> button;
  bool destroyed{};
  auto capture = std::make_shared<Capture>(destroyed);
  constexpr auto LIMITED = ui::ct::scene(ui::ct::column(ui::ct::button<"button">("Apply"), ui::ct::label("Too many")));
  auto recipe = ui::ct::describe(LIMITED, ui::ct::reference<"button">(button), ui::ct::onClick<"button">([capture] {
                                 }));

  capture.reset();

  const auto result = ui::mount(*owner, std::move(recipe));

  REQUIRE_FALSE(result);
  CHECK(treeCause(result.error(), TreeError::NODE_LIMIT));
  CHECK(button.node() == Node<Button>{});
  CHECK(take(app->tree().inspect(owner->root())).children.empty());
  // Disconnect is logical; the existing router reclaims cancelled registrations at collection.
  take(app->router().focused());
  CHECK(destroyed);
}

TEST_CASE("runtime text binding uses scene state and preserves immediate publication") {
  Fixture fixture;
  ui::Ref<LineEdit> field;
  auto state = take(fixture.owner->state<std::string>("runtime"));
  constexpr auto EDITOR = ui::ct::scene(ui::ct::lineEdit<"field">("static"));

  take(ui::mount(*fixture.owner,
                 ui::ct::describe(EDITOR, ui::ct::bind<"field">(state), ui::ct::reference<"field">(field))));

  CHECK(take(field.text().get()) == "runtime");
  take(field.text().set("changed").result());
  CHECK(take(state.snapshot())->value == "changed");
}

TEST_CASE("static text conversion uses each receiving scene width policy") {
  constexpr auto CAPTION = ui::ct::scene(ui::ct::label<"caption">("\xc2\xb7"));

  for (auto ambiguous : {AmbiguousWidth::NARROW, AmbiguousWidth::WIDE}) {
    ApplicationOptions options;
    ui::Ref<Label> caption;

    options.widthPolicy = {.ambiguous = ambiguous, .revision = 7};

    auto app = take(Application::create(options));
    auto owner = take(ManagedScene::create(*app));

    take(ui::mount(*owner, ui::ct::describe(CAPTION, ui::ct::reference<"caption">(caption))));
    take(app->tree().read(caption.node(), [&](const Label &widget) {
      CHECK(widget.text().policy() == options.widthPolicy);
      CHECK(widget.text().measure() == (ambiguous == AmbiguousWidth::WIDE ? 2 : 1));
    }));
  }
}

TEST_CASE("static explicit editor mode retains draft until activation") {
  Fixture fixture;
  ui::Ref<LineEdit> field;
  auto state = take(fixture.owner->state<std::string>("runtime"));
  constexpr auto EDITOR = ui::ct::scene(ui::ct::lineEdit<"field">("static", EditMode::EXPLICIT));

  take(ui::mount(*fixture.owner,
                 ui::ct::describe(EDITOR, ui::ct::bind<"field">(state), ui::ct::reference<"field">(field))));
  take(field.text().set("draft").result());
  CHECK(take(field.text().get()) == "draft");
  CHECK(take(state.snapshot())->value == "runtime");
  take(fixture.app->router().invoke(field.node(), {Action::ACTIVATE}));
  CHECK(take(state.snapshot())->value == "draft");
}

TEST_CASE("foreign scene state failure leaves references unpublished") {
  Fixture first;
  Fixture second;
  ui::Ref<LineEdit> field;
  const auto foreign = take(first.owner->state<std::string>("foreign"));
  constexpr auto EDITOR = ui::ct::scene(ui::ct::lineEdit<"field">("static"));
  const auto result = ui::mount(
    *second.owner, ui::ct::describe(EDITOR, ui::ct::bind<"field">(foreign), ui::ct::reference<"field">(field)));

  REQUIRE_FALSE(result);
  CHECK(treeCause(result.error(), TreeError::INVALID_ARGUMENT));
  CHECK(field.node() == Node<LineEdit>{});
  CHECK(take(second.app->tree().inspect(second.owner->root())).children.empty());
}

TEST_CASE("scene owns callback through self removal and releases it at router collection") {
  Fixture fixture;
  ui::Ref<Button> button;
  bool destroyed{};
  bool survivedRemoval{};
  auto capture = std::make_shared<Capture>(destroyed);
  constexpr auto BUTTON = ui::ct::scene(ui::ct::button<"button">("Remove"));

  take(ui::mount(*fixture.owner,
                 ui::ct::describe(BUTTON, ui::ct::reference<"button">(button),
                                  ui::ct::onClick<"button">([capture, button, &fixture, &destroyed,
                                                             &survivedRemoval]() -> std::expected<void, TreeError> {
                                    auto result = fixture.owner->remove(button.node());

                                    survivedRemoval = result && !destroyed;

                                    return result ? std::expected<void, TreeError>{}
                                                  : std::unexpected(std::get<TreeError>(result.error().cause));
                                  }))));

  capture.reset();
  CHECK_FALSE(destroyed);
  take(fixture.app->router().invoke(button.node(), {Action::ACTIVATE}));
  CHECK(survivedRemoval);
  take(fixture.app->router().focused());
  CHECK(destroyed);
  CHECK_FALSE(button.text().get());
}

TEST_CASE("static facade callbacks and refs follow close and wrong-thread contracts") {
  ui::Ref<Button> button;
  bool destroyed{};
  auto capture = std::make_shared<Capture>(destroyed);
  constexpr auto BUTTON = ui::ct::scene(ui::ct::button<"button">("Close"));
  {
    auto scene =
      ui::scene(ui::ct::describe(BUTTON, ui::ct::reference<"button">(button), ui::ct::onClick<"button">([capture] {
                                 })));

    capture.reset();
    take(scene.prepareChecked());

    std::optional<std::expected<void, ManagedError>> foreign;
    std::thread worker([&] {
      foreign = scene.prepareChecked();
    });

    worker.join();
    REQUIRE(foreign);
    REQUIRE_FALSE(*foreign);
    CHECK(treeCause(foreign->error(), TreeError::WRONG_THREAD));
    CHECK_FALSE(destroyed);
    take(scene.close());
    CHECK_FALSE(button.text().get());
    take(scene.close());
  }

  // Explicit close retires the tree; final Application destruction also releases router captures.
  CHECK(destroyed);
}
