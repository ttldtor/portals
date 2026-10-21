// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/ui.hpp>

#include <array>
#include <atomic>
#include <fstream>
#include <limits>

using namespace portals;

namespace {
using Code = SceneResourceErrorCode;

template <class T, class E> T take(std::expected<T, E> result) {
  REQUIRE(result);
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

bool cause(const ManagedError &error, TreeError expected) {
  const auto *value = std::get_if<TreeError>(&error.cause);
  return value && *value == expected;
}

struct TemporaryFile {
  static inline std::atomic<unsigned> sequence{};
  std::filesystem::path path =
    std::filesystem::temp_directory_path() / ("portals-grid-resource-" + std::to_string(sequence++) + ".ui");

  explicit TemporaryFile(std::string_view source) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(source.data(), static_cast<std::streamsize>(source.size()));
    REQUIRE(stream.good());
  }

  ~TemporaryFile() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
};

template <class Source> ManagedError rejected(Source &&source, ui::SceneResourceOptions options = {}) {
  auto owner = ui::scene(std::forward<Source>(source), std::move(options));
  const auto result = owner.prepareChecked();
  REQUIRE_FALSE(result);
  REQUIRE(result.error().sceneResource);
  return result.error();
}

struct LoadedGeometry {
  GridOptions options;
  CellRect first;
  CellRect second;
  CellRect reserved;
};

LoadedGeometry inspect(ui::Scene &owner) {
  auto grid = owner.get<Grid>("grid");
  auto first = owner.get<Label>("first");
  auto second = owner.get<Label>("second");
  auto reserved = owner.get<Label>("reserved");
  Application *app{};
  ManagedScene *scene{};
  owner.configure([&](ManagedScene &managed, Application &application) {
    scene = &managed;
    app = &application;
  });
  take(owner.prepareChecked());
  take(scene->prepare(*app, {40, 10}));
  const auto bounds = take(CellRect::from({}, {40, 10}));
  take(app->tree().frame([&] {
    take(layoutTree(app->tree(), app->root(), bounds, bounds));
  }));
  take(scene->completedLayout(*app));
  LoadedGeometry result;
  take(app->tree().read(grid.node(), [&](const Grid &value) {
    result.options = value.options();
  }));
  result.first = take(first.geometry()).local;
  result.second = take(second.geometry()).local;
  result.reserved = take(reserved.geometry()).local;
  REQUIRE(result.options.cells.size() == 3);
  CHECK(result.options.cells[0].node == first.node().id());
  CHECK(result.options.cells[1].node == second.node().id());
  CHECK(result.options.cells[2].node == reserved.node().id());
  take(owner.close());

  const auto closed = first.geometry();

  REQUIRE_FALSE(closed);
  CHECK(cause(closed.error(), TreeError::CLOSED));
  return result;
}
} // namespace

TEST_CASE("loaded Grid matches C++ placement in legacy configuration string and file") {
  auto cpp = ui::scene(ui::grid(ui::label("Name").id("first"), ui::label("Ada").columnSpan(2).id("second"),
                                ui::label("Reserved").row(0).column(2).id("reserved"))
                         .columns(3)
                         .columnGap(2)
                         .rowGap(1)
                         .id("grid"));
  const auto expected = inspect(cpp);
  const std::string_view content = R"(Grid#grid { columns=3 columnGap=2 rowGap=1
    Label#first { "Name" } Label#second { "Ada" columnSpan=2 }
    Label#reserved { "Reserved" row=0 column=2 }
  })";
  for (const bool configured : {false, true}) {
    const auto source = configured ? "scene { " + std::string(content) + " }" : std::string(content);
    TemporaryFile file(source);
    for (const bool fromFile : {false, true}) {
      ui::SceneResourceOptions options;
      if (configured) {
        options.format = ui::SceneResourceFormat::CONFIGURATION;
      }
      auto owner = fromFile ? ui::scene(ui::SceneResourceFile{file.path}, options) : ui::scene(source, options);
      const auto actual = inspect(owner);
      CHECK(actual.first == expected.first);
      CHECK(actual.second == expected.second);
      CHECK(actual.reserved == expected.reserved);
      CHECK(actual.options.columns.size() == expected.options.columns.size());
      CHECK(actual.options.rows.size() == expected.options.rows.size());
      CHECK(actual.options.cells[1].column_span == 2);
      CHECK(actual.options.cells[2].column == 2);
    }
  }
}

TEST_CASE("loaded grid preflights every nested placement before factories and text resolvers") {
  for (const bool configured : {false, true}) {
    for (const auto invalid :
         {"Grid { columns=2 Probe { text=i18n(\"x\") row=0 column=0 } Label { row=0 column=0 } }",
          "Grid { columns=2 Probe { text=i18n(\"x\") } Grid { columns=1 Label { columnSpan=2 } } }",
          "Grid { columns=2 Probe { text=i18n(\"x\") } Label { row=1 } }",
          "VBox { Probe { text=i18n(\"x\") } Label { row=0 column=0 } }"}) {
      int effects{};
      ui::SceneResourceOptions options;
      if (configured) {
        options.format = ui::SceneResourceFormat::CONFIGURATION;
      }
      take(options.registry.add<Label>("Probe", [&](ui::SceneResourceArguments arguments) {
        ++effects;
        return ui::label(arguments.text);
      }));
      options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
        ++effects;
        return "resolved";
      };
      const auto source = configured ? "scene { " + std::string(invalid) + " }" : std::string(invalid);
      TemporaryFile file(source);

      for (const bool fromFile : {false, true}) {
        const auto error = fromFile ? rejected(ui::SceneResourceFile{file.path}, options) : rejected(source, options);

        CHECK(cause(error, TreeError::INVALID_ARGUMENT));
        CHECK(effects == 0);
        CHECK(error.sceneResource->source);
        CHECK(error.sceneResource->line > 0);
      }
    }
  }
}

TEST_CASE("loaded grid bounds malformed literals overflow and occupancy work stay typed") {
  struct Scenario {
    std::string_view source;
    TreeError error;
  };

  const Scenario cases[]{
    {"Grid { columns=0 }", TreeError::INVALID_ARGUMENT},
    {"Grid { columns=-1 }", TreeError::INVALID_ARGUMENT},
    {"Grid { columns=184467440737095516160 }", TreeError::INVALID_ARGUMENT},
    {"Grid { columns=2 flow=BAD }", TreeError::INVALID_ARGUMENT},
    {"Grid { columns=2 rowGap=2147483648 }", TreeError::INVALID_ARGUMENT},
    {"Grid { columns=2 Label { rowSpan=0 } }", TreeError::INVALID_ARGUMENT},
    {"Grid { columns=3 maxTracksPerAxis=2 }", TreeError::NODE_LIMIT},
    {"Grid { columns=2 maxPlacements=1 Label{} Label{} }", TreeError::NODE_LIMIT},
    {"Grid { columns=2 maxPlacementWork=1 Label{} Label{} }", TreeError::NODE_LIMIT},
    {"Grid { columns=3 columnGap=2147483647 }", TreeError::INVALID_GEOMETRY},
    {"Grid { columns=2 Label { row=18446744073709551615 column=0 rowSpan=2 } }", TreeError::INVALID_GEOMETRY},
    {"Grid { columns=2 Label { horizontal=BAD } }", TreeError::INVALID_ARGUMENT},
  };
  for (const auto &test : cases) {
    INFO(test.source);
    const auto error = rejected(test.source);
    CHECK(cause(error, test.error));
  }
}

TEST_CASE("Grid remains available to custom factories definitions and derived aliases") {
  CHECK(ui::controlPropertySchema("Grid").empty());
  for (const bool configured : {false, true}) {
    for (const bool fromFile : {false, true}) {
      int called{};
      ui::SceneResourceOptions options;
      if (configured) {
        options.format = ui::SceneResourceFormat::CONFIGURATION;
        options.controls.emplace();
        constexpr std::string_view paths[]{"text", "width", "height", "flex"};
        options.controls->registerType("Grid", paths).define("Alias", "Grid");
      }
      take(options.registry.add<Label>(
        "Grid",
        [&](ui::SceneResourceArguments arguments) {
          ++called;
          return ui::label(arguments.text);
        },
        true));
      take(options.registry.add<Label>("header", [](ui::SceneResourceArguments arguments) {
        return ui::label(arguments.text);
      }));
      const auto source =
        configured ? "scene { Alias#value { text=\"custom\" header{} } }" : "Grid#value { text=\"custom\" header{} }";
      TemporaryFile file(source);
      auto owner = fromFile ? ui::scene(ui::SceneResourceFile{file.path}, options) : ui::scene(source, options);
      auto value = owner.get<Label>("value");
      take(owner.prepareChecked());
      CHECK(called == 1);
      CHECK(take(value.text().get()) == "custom");
      take(owner.close());
      const auto duplicate = options.registry.add<Label>("Grid", [](ui::SceneResourceArguments) {
        return ui::label("");
      });
      REQUIRE_FALSE(duplicate);
      CHECK(duplicate.error() == Code::DUPLICATE_TYPE);
    }
  }
  ui::SceneResourceOptions defined;
  defined.controls.emplace();
  defined.controls->define("Grid", "Label").define("Alias", "Grid");
  auto owner = ui::scene("Alias#label { text=\"definition\" }", defined);
  auto label = owner.get<Label>("label");
  take(owner.prepareChecked());
  CHECK(take(label.text().get()) == "definition");
  take(owner.close());

  ui::SceneResourceOptions schemaOnly;
  constexpr std::string_view paths[]{"text", "width", "height", "flex"};

  schemaOnly.controls.emplace();
  schemaOnly.controls->registerType("Grid", paths);

  const auto missingFactory = rejected("Grid{}", schemaOnly);

  CHECK(missingFactory.sceneResource->code == Code::UNKNOWN_TYPE);
  CHECK(schemaOnly.controls->hasRegisteredType("Grid"));
}

TEST_CASE("fallback Grid aliases and defaults retain caller snapshot and source origins") {
  ui::ControlContext context;
  ui::ControlSettings defaults;
  defaults.grid.columns = 2;
  context.define("Named", "Grid").defaults("Grid", defaults);
  ui::SceneResourceOptions options;
  options.controls = context;
  auto owner = ui::scene("Named#grid { Label#first{} Label#second{} Label#reserved{} }", options);
  auto grid = owner.get<Grid>("grid");
  take(owner.prepareChecked());
  CHECK(grid.node() != Node<Grid>{});
  take(owner.close());
  const auto unresolved = context.resolve("Grid");
  REQUIRE_FALSE(unresolved);
  CHECK(unresolved.error().code == ui::ControlContextErrorCode::UNKNOWN_TYPE);
  CHECK_FALSE(context.hasRegisteredType("Grid"));
  CHECK_FALSE(options.controls->hasRegisteredType("Grid"));

  options.format = ui::SceneResourceFormat::CONFIGURATION;
  options.controls.reset();
  const auto error = rejected("definitions { Named=Grid { columns=0 } } scene { Named{} }", options);
  CHECK(error.sceneResource->code == Code::INVALID_VALUE);
  REQUIRE(error.sceneResource->propertyPath);
  CHECK(*error.sceneResource->propertyPath == "Named.columns");
  CHECK(error.sceneResource->offset > 0);
  CHECK(error.sceneResource->line == 1);

  const std::string_view source = "definitions { Named=Grid { columns=0 } } scene { Named{} }";
  TemporaryFile file(source);
  const auto fileError = rejected(ui::SceneResourceFile{file.path}, options);

  REQUIRE(fileError.sceneResource->source);
  CHECK(std::filesystem::path(*fileError.sceneResource->source) == file.path);
  REQUIRE(fileError.sceneResource->propertyPath);
  CHECK(*fileError.sceneResource->propertyPath == "Named.columns");
  CHECK(fileError.sceneResource->offset == error.sceneResource->offset);
}

TEST_CASE("legacy allowlists above custom schema default budget remain supported") {
  int customFactories{};
  ui::SceneResourceOptions options;

  for (unsigned index = 0; index < 70; ++index) {
    take(options.registry.add<Label>("Custom" + std::to_string(index), [&, index](ui::SceneResourceArguments) {
      if (index == 69) {
        ++customFactories;
      }

      return ui::label("custom");
    }));
  }

  for (const bool fromFactory : {false, true}) {
    int sceneFactories{};

    customFactories = 0;

    auto owner = fromFactory ? ui::scene([&](ManagedScene &) {
      ++sceneFactories;
      return ui::resource("Grid { columns=1 Custom69#last{} }", options);
    })
                             : ui::scene("Grid { columns=1 Custom69#last{} }", options);
    auto last = owner.get<Label>("last");

    take(owner.prepareChecked());
    CHECK(sceneFactories == (fromFactory ? 1 : 0));
    CHECK(customFactories == 1);
    CHECK(take(last.text().get()) == "custom");
    take(owner.close());
  }

  std::array<ui::ControlContext, 4> configured{ui::ControlContext{}, ui::ControlContext{}, ui::ControlContext{},
                                               ui::ControlContext({.maxDefinitions = 1})};
  ui::ControlSettings defaults;

  defaults.text = TextValue("default");
  configured[0].define("Alias", "Label");
  configured[1].defaults("Label", defaults);

  constexpr std::string_view paths[]{"text"};

  configured[2].registerType("UserOnly", paths);

  for (const auto &controls : configured) {
    int sceneFactories{};
    ui::SceneOptions sceneOptions;

    sceneOptions.controls = controls;
    customFactories = 0;

    auto owner = ui::scene(
      [&](ManagedScene &) {
        ++sceneFactories;
        return ui::resource("Grid { columns=1 Custom69#last{} }", options);
      },
      sceneOptions);
    const auto prepared = owner.prepareChecked();

    REQUIRE_FALSE(prepared);
    REQUIRE(prepared.error().sceneResource);
    CHECK(prepared.error().sceneResource->code == Code::TYPE_LIMIT);
    CHECK(sceneFactories == 1);
    CHECK(customFactories == 0);
  }
}

TEST_CASE("placement properties cannot become inherited control defaults") {
  ui::SceneResourceOptions options;
  options.format = ui::SceneResourceFormat::CONFIGURATION;
  const auto error = rejected("defaults { Button { row=0 } } scene { Grid { columns=2 Button{} } }", options);
  CHECK(error.sceneResource->code == Code::UNKNOWN_PROPERTY);
  REQUIRE(error.sceneResource->propertyPath);
  CHECK(*error.sceneResource->propertyPath == "Button.row");
  const auto unsupported = rejected("scene { Button { columns=2 } }", options);
  CHECK(unsupported.sceneResource->code == Code::UNKNOWN_PROPERTY);
  REQUIRE(unsupported.sceneResource->propertyPath);
  CHECK(*unsupported.sceneResource->propertyPath == "Button.columns");
}

TEST_CASE("loaded header and body flatten real controls with independent positions") {
  for (const bool configured : {false, true}) {
    const std::string content = R"(Grid#grid { columns=2
      header { sticky=true Label#heading { text="Heading" columnSpan=2 } Label#sub { text="Sub" } }
      body { Label#first { text="a" } Label#second { text="b" row=0 column=1 } }
    })";
    auto selected = content;
    if (configured) {
      selected.replace(0, 4, "Named");
    }
    const auto source = configured ? "definitions { Named=Grid { columns=2 } } scene { " + selected + " }" : content;
    TemporaryFile file(source);
    for (const bool fromFile : {false, true}) {
      ui::SceneResourceOptions options;

      options.limits.maxDepth = 2;
      if (configured) {
        options.format = ui::SceneResourceFormat::CONFIGURATION;
      }
      auto owner = fromFile ? ui::scene(ui::SceneResourceFile{file.path}, options) : ui::scene(source, options);
      auto grid = owner.get<Grid>("grid");
      auto heading = owner.get<Label>("heading");
      auto sub = owner.get<Label>("sub");
      auto first = owner.get<Label>("first");
      auto second = owner.get<Label>("second");
      Application *app{};
      owner.configure([&](ManagedScene &, Application &application) {
        app = &application;
      });
      take(owner.prepareChecked());
      take(app->tree().read(grid.node(), [&](const Grid &value) {
        const auto state = value.options();
        REQUIRE(state.cells.size() == 4);
        CHECK(state.header_rows.size() == 2);
        CHECK(state.rows.size() == 1);
        CHECK(state.sticky_header);
        CHECK(state.cells[0].node == heading.node().id());
        CHECK(state.cells[0].row == 0);
        CHECK(state.cells[0].column_span == 2);
        CHECK(state.cells[0].region == GridRegion::HEADER);
        CHECK(state.cells[1].node == sub.node().id());
        CHECK(state.cells[1].row == 1);
        CHECK(state.cells[2].node == first.node().id());
        CHECK(state.cells[2].row == 0);
        CHECK(state.cells[2].column == 0);
        CHECK(state.cells[2].region == GridRegion::BODY);
        CHECK(state.cells[3].node == second.node().id());
        CHECK(state.cells[3].row == 0);
        CHECK(state.cells[3].column == 1);
      }));
      const auto children = take(app->tree().inspect(grid.node())).children;
      CHECK(children.size() == 4);
      CHECK(take(app->tree().inspect(heading.node())).parent == grid.node());
      CHECK(take(app->tree().inspect(first.node())).parent == grid.node());
      take(owner.close());
    }
  }
}

TEST_CASE("loaded regions reject mixed duplicate unbounded and cross-region spans before effects") {
  for (const auto content :
       {"Grid { columns=2 header{} header{} }", "Grid { columns=2 body{} body{} }",
        "Grid { columns=2 Label{} header{} }", "Grid { columns=2 header{} Label{} }",
        "Grid { rows=2 flow=COLUMN_MAJOR header{} body{} }",
        "Grid { columns=2 rows=1 header { Label{ columnSpan=3 } } body { Probe{} } }",
        "Grid { columns=2 header { sticky=bad } body { Probe{} } }", "Grid { columns=2 body { sticky=true } }",
        "Grid { columns=2 header { sticky=true sticky=false } }"}) {
    int effects{};
    ui::SceneResourceOptions options;
    take(options.registry.add<Label>("Probe", [&](ui::SceneResourceArguments) {
      ++effects;
      return ui::label("effect");
    }));
    const auto error = rejected(content, options);
    CHECK(cause(error, TreeError::INVALID_ARGUMENT));
    CHECK(effects == 0);
  }
}

TEST_CASE("hidden loaded children keep reserved cells and full identity mappings") {
  ui::SceneResourceOptions options;
  Application *app{};
  take(options.registry.add<Label>("Hidden", [&](ui::SceneResourceArguments) {
    return ui::label("hidden").setup([&](ManagedScene &, Node<Label> node) -> std::expected<void, ManagedError> {
      auto result = app->tree().setVisible(node, false);
      return result ? std::expected<void, ManagedError>{} : std::unexpected(ui::detail::error(node, result.error()));
    });
  }));
  // The custom recipe captures only this synchronous prepare; configure runs after mount,
  // so obtain the application through an explicit owning fixture instead.
  auto application = take(Application::create());
  app = application.get();
  auto managed = take(ManagedScene::create(*application));
  take(ui::mount(*managed, ui::resource("Grid { columns=2 columnGap=2 Hidden{} Label{text=\"shown\"} }", options)));
  const auto rootChildren = take(app->tree().inspect(managed->root())).children;
  REQUIRE(rootChildren.size() == 1);
  const auto children = take(app->tree().inspect(rootChildren[0])).children;
  REQUIRE(children.size() == 2);
  CHECK_FALSE(take(app->tree().inspect(children[0])).visible);
  take(app->tree().read(rootChildren[0], [&](const Widget &widget) {
    const auto *grid = dynamic_cast<const Grid *>(&widget);

    REQUIRE(grid);

    const auto state = grid->options();

    REQUIRE(state.cells.size() == 2);
    CHECK(state.cells[0].node == children[0].id());
    CHECK(state.cells[0].column == 0);
    CHECK(state.cells[1].node == children[1].id());
    CHECK(state.cells[1].column == 1);
  }));

  take(managed->prepare(*app, {40, 10}));

  const auto bounds = take(CellRect::from({}, {40, 10}));

  take(app->tree().frame([&] {
    take(layoutTree(app->tree(), app->root(), bounds, bounds));
  }));
  CHECK(take(app->tree().inspect(children[1])).geometry.local.origin().x == 2);
  take(managed->close());
}

TEST_CASE("fallback schema is bounded independently of a full user declaration budget") {
  ui::ControlContext context({.maxDefinitions = 1});
  context.define("Alias", "Grid");
  ui::SceneResourceOptions options;
  options.controls = context;
  auto owner = ui::scene("Alias { columns=2 Label{} }", options);
  take(owner.prepareChecked());
  take(owner.close());
  const auto original = context.resolve("Alias");
  REQUIRE_FALSE(original);
  CHECK(original.error().code == ui::ControlContextErrorCode::UNKNOWN_TYPE);
}

TEST_CASE("Grid definition owns its name after configuration declarations are collected") {
  for (const bool configured : {false, true}) {
    for (const bool fromFile : {false, true}) {
      ui::SceneResourceOptions options;
      std::string source;
      if (configured) {
        options.format = ui::SceneResourceFormat::CONFIGURATION;
        source = "scene { Alias#value { text=\"defined\" } } definitions { Alias=Grid{} Grid=Label{} }";
      } else {
        options.controls.emplace();
        options.controls->define("Grid", "Label").define("Alias", "Grid");
        source = "Alias#value { text=\"defined\" }";
      }
      TemporaryFile file(source);
      auto owner = fromFile ? ui::scene(ui::SceneResourceFile{file.path}, options) : ui::scene(source, options);
      auto value = owner.get<Label>("value");
      take(owner.prepareChecked());
      CHECK(take(value.text().get()) == "defined");
      take(owner.close());
    }
  }
}

TEST_CASE("inherited Grid settings preserve nested padding and column-major body limits") {
  ui::SceneResourceOptions options;
  options.format = ui::SceneResourceFormat::CONFIGURATION;
  auto owner = ui::scene(R"(
    defaults { Grid { columns=2 rows=3 flow=COLUMN_MAJOR padding { left=1 top=2 right=3 bottom=4 } } }
    definitions { Named=Grid { columnGap=2 rowGap=1 } }
    scene { Named#grid { header { Label { columnSpan=2 } } body { Label{} Label{} Label{} Label{} } } }
  )",
                         options);
  auto grid = owner.get<Grid>("grid");
  Application *app{};
  owner.configure([&](ManagedScene &, Application &application) {
    app = &application;
  });
  take(owner.prepareChecked());
  take(app->tree().read(grid.node(), [&](const Grid &value) {
    const auto state = value.options();
    CHECK(state.header_rows.size() == 1);
    CHECK(state.rows.size() == 3);
    CHECK(state.columns.size() == 2);
    CHECK(state.padding.left == 1);
    CHECK(state.padding.top == 2);
    CHECK(state.padding.right == 3);
    CHECK(state.padding.bottom == 4);
    CHECK(state.column_gap == 2);
    CHECK(state.row_gap == 1);
    REQUIRE(state.cells.size() == 5);
    CHECK(state.cells[1].row == 0);
    CHECK(state.cells[1].column == 0);
    CHECK(state.cells[2].row == 1);
    CHECK(state.cells[3].row == 2);
    CHECK(state.cells[4].row == 0);
    CHECK(state.cells[4].column == 1);
  }));
  take(owner.close());
}

TEST_CASE("loaded Grid scroll defaults preserve explicit false through aliases and instance overrides") {
  for (const bool configured : {false, true}) {
    ui::SceneResourceOptions options;
    std::string source;
    if (configured) {
      options.format = ui::SceneResourceFormat::CONFIGURATION;
      source = R"(
        defaults { Grid { columns=1 scroll=true } }
        definitions { Disabled=Grid { scroll=false } Enabled=Disabled { scroll=true } }
        scene { VBox { Grid#inherited{} Disabled#disabled{} Enabled#enabled{} Enabled#overridden{scroll=false} } }
      )";
    } else {
      ui::ControlSettings defaults;
      defaults.grid.columns = 1;
      defaults.grid.scroll = true;
      ui::ControlSettings disabled;
      disabled.grid.scroll = false;
      ui::ControlSettings enabled;
      enabled.grid.scroll = true;
      options.controls.emplace();
      options.controls->defaults("Grid", defaults)
        .define("Disabled", "Grid", disabled)
        .define("Enabled", "Disabled", enabled);
      source = "VBox { Grid#inherited{} Disabled#disabled{} Enabled#enabled{} Enabled#overridden{scroll=false} }";
    }
    TemporaryFile file(source);
    for (const bool fromFile : {false, true}) {
      auto owner = fromFile ? ui::scene(ui::SceneResourceFile{file.path}, options) : ui::scene(source, options);
      auto inherited = owner.get<Grid>("inherited");
      auto disabled = owner.get<Grid>("disabled");
      auto enabled = owner.get<Grid>("enabled");
      auto overridden = owner.get<Grid>("overridden");
      ManagedScene *managed{};
      owner.configure([&](ManagedScene &scene, Application &) {
        managed = &scene;
      });
      take(owner.prepareChecked());
      take(managed->setScrollOffset(inherited.node(), ScrollAxis::VERTICAL, {}));
      take(managed->setScrollOffset(enabled.node(), ScrollAxis::VERTICAL, {}));
      for (const auto node : {disabled.node(), overridden.node()}) {
        const auto rejected = managed->setScrollOffset(node, ScrollAxis::VERTICAL, {});
        REQUIRE_FALSE(rejected);
        CHECK(cause(rejected.error(), TreeError::INVALID_ARGUMENT));
      }
      take(owner.close());
    }
  }
}

TEST_CASE("resource preparation inherits scene context before selecting fallback or declared Grid") {
  SUBCASE("fallback accepts inherited aliases without changing the caller") {
    ui::ControlContext context;
    context.define("Alias", "Grid");
    ui::SceneOptions sceneOptions;
    sceneOptions.controls = context;
    auto owner = ui::scene(ui::resource("Alias#grid { columns=1 Label{} }"), sceneOptions);
    auto grid = owner.get<Grid>("grid");

    take(owner.prepareChecked());
    CHECK(grid.node() != Node<Grid>{});
    take(owner.close());

    const auto original = context.resolve("Alias");

    REQUIRE_FALSE(original);
    CHECK(original.error().code == ui::ControlContextErrorCode::UNKNOWN_TYPE);
  }

  SUBCASE("late configuration definition selects Label before validating inherited alias settings") {
    ui::ControlSettings text;
    text.text = TextValue("inherited");
    ui::ControlContext context;
    context.define("Alias", "Grid", text);
    ui::SceneOptions sceneOptions;
    sceneOptions.controls = context;
    ui::SceneResourceOptions resourceOptions;
    resourceOptions.format = ui::SceneResourceFormat::CONFIGURATION;
    auto owner =
      ui::scene(ui::resource("scene { Alias#value{} } definitions { Grid=Label{} }", resourceOptions), sceneOptions);
    auto value = owner.get<Label>("value");

    take(owner.prepareChecked());
    CHECK(take(value.text().get()) == "inherited");
    take(owner.close());
  }
}

TEST_CASE("factory scene validates context before invoking a resource-producing factory") {
  SUBCASE("a late resource definition cannot rescue invalid pre-factory context") {
    int factories{};
    ui::ControlSettings inherited;
    inherited.text = TextValue("late Label text");
    ui::ControlContext context;
    context.define("Alias", "Grid", inherited);
    ui::SceneOptions sceneOptions;
    sceneOptions.controls = context;
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    auto owner = ui::scene(
      [&](ManagedScene &) {
        ++factories;
        return ui::resource("definitions { Grid=Label{} } scene { Alias{} }", options);
      },
      sceneOptions);
    const auto result = owner.prepareChecked();

    REQUIRE_FALSE(result);
    REQUIRE(result.error().sceneResource);
    CHECK(result.error().sceneResource->code == Code::UNKNOWN_PROPERTY);
    CHECK(factories == 0);
  }

  SUBCASE("a valid pre-factory context retains the late configuration terminal user factory") {
    int factories{};
    int userFactories{};
    ui::ControlContext context;
    context.define("Alias", "Grid");
    ui::SceneOptions sceneOptions;
    sceneOptions.controls = context;
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    take(options.registry.add<Label>("UserLabel", [&](ui::SceneResourceArguments arguments) {
      ++userFactories;
      return ui::label(arguments.text);
    }));
    auto owner = ui::scene(
      [&](ManagedScene &) {
        ++factories;
        return ui::resource("scene { Alias#value{text=\"custom\"} } definitions { Grid=UserLabel{} }", options);
      },
      sceneOptions);
    auto value = owner.get<Label>("value");

    take(owner.prepareChecked());
    CHECK(factories == 1);
    CHECK(userFactories == 1);
    CHECK(take(value.text().get()) == "custom");
    take(owner.close());

    const auto original = context.resolve("Alias");

    REQUIRE_FALSE(original);
    CHECK(original.error().code == ui::ControlContextErrorCode::UNKNOWN_TYPE);
  }
}
