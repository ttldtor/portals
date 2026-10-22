// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

#include "../examples/scene_resource/form.hpp"
#include "../src/widgets/toast_state.hpp"
#include "support/examples/replay/terminal.hpp"

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <type_traits>

using namespace portals;

static_assert(std::is_aggregate_v<ManagedError>);
static_assert(std::is_aggregate_v<ui::SceneResourceOptions>);
static_assert(std::is_nothrow_copy_constructible_v<ManagedError>);
static_assert(std::is_nothrow_copy_assignable_v<ManagedError>);
static_assert(requires(ui::Description<Label> &recipe) {
  ui::detail::Access::preparedControl(recipe);
  ui::detail::Access::preparedControl(recipe, {});
  ui::detail::Access::preparedControl(recipe, std::uint32_t{0xc0000000U});
  ui::detail::Access::preparedControlPresence(recipe, ui::detail::ControlPropertyPresence{});
});

namespace {
std::atomic<std::ptrdiff_t> allocationsBeforeFailure{-1};
std::ptrdiff_t activeBudget{-1};

void oomTerminate() {
  allocationsBeforeFailure = -1;
  std::fprintf(stderr, "resource OOM terminate at budget=%td\n", activeBudget);
  std::abort();
}
} // namespace

// Fault injection is local to this test process and armed only around checked operations.
void *operator new(std::size_t size) {
  if (allocationsBeforeFailure == 0) {
    throw std::bad_alloc();
  }

  if (allocationsBeforeFailure > 0) {
    --allocationsBeforeFailure;
  }

  auto *memory = std::malloc(size ? size : 1);

  if (!memory) {
    throw std::bad_alloc();
  }

  return memory;
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete(void *memory) noexcept {
  std::free(memory);
}

void operator delete[](void *memory) noexcept {
  std::free(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
  std::free(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
  std::free(memory);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  return ::operator new(size, std::nothrow);
}

void operator delete(void *memory, const std::nothrow_t &) noexcept {
  std::free(memory);
}

void operator delete[](void *memory, const std::nothrow_t &) noexcept {
  std::free(memory);
}

namespace {
using Code = SceneResourceErrorCode;

bool cause(const ManagedError &error, TreeError expected) {
  const auto *value = std::get_if<TreeError>(&error.cause);

  return value && *value == expected;
}

ManagedError rejected(std::string_view source, ui::SceneResourceOptions options = {}) {
  auto owner = ui::scene(source, std::move(options));
  auto result = owner.prepareChecked();

  REQUIRE_FALSE(result);
  REQUIRE(result.error().sceneResource);

  return result.error();
}

struct TemporaryFile {
  std::filesystem::path path =
    std::filesystem::temp_directory_path() /
    ("portals-resource-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".scene");

  ~TemporaryFile() {
    std::error_code ignored;

    std::filesystem::remove(path, ignored);
  }

  void write(std::string_view bytes) {
    std::ofstream output(path, std::ios::binary);

    REQUIRE(output);
    output << bytes;
    REQUIRE(output);
  }
};

struct Custom final : SizedWidget {
  static inline int alive{};
  std::string text;

  explicit Custom(std::string_view value) : text(value) {
    ++alive;
  }

  ~Custom() override {
    --alive;
  }
};

struct Plain final : Widget {
  static inline int alive{};

  Plain() {
    ++alive;
  }

  ~Plain() override {
    --alive;
  }
};

struct PaintCounter final : PaintContext {
  std::size_t commands{};

  std::expected<void, TreeError> submit(std::span<const RenderCommand> values) override {
    commands += values.size();

    return {};
  }
};
} // namespace

TEST_CASE("resource instance flags override factories without requiring a control or size schema") {
  for (const bool configuration : {false, true}) {
    ui::SceneResourceOptions options;
    bool argumentsObserved{}, configured{};

    options.format = configuration ? ui::SceneResourceFormat::CONFIGURATION : ui::SceneResourceFormat::LEGACY;
    REQUIRE(options.registry.add<Plain>("Plain", [&](ui::SceneResourceArguments arguments) {
      argumentsObserved = arguments.visible == true && arguments.enabled == true;

      return ui::custom<Plain>().visible(false).enabled(false);
    }));

    const std::string body = "VBox#parent{enabled=false Plain#plain{visible=true enabled=true} "
                             "Button#hidden{\"Hidden\" visible=false} Button#disabled{\"Disabled\" enabled=false}}";
    auto owner = ui::scene(configuration ? "scene{" + body + "}" : body, options);
    auto parent = owner.get<Column>("parent");
    auto plain = owner.get<Plain>("plain");
    auto hidden = owner.get<Button>("hidden");
    auto disabled = owner.get<Button>("disabled");

    owner.configure([&](ManagedScene &scene, Application &application) {
      auto &tree = application.tree();
      const auto custom = tree.inspect(plain.node());
      const auto hiddenState = tree.inspect(hidden.node());
      const auto disabledState = tree.inspect(disabled.node());

      REQUIRE(custom);
      REQUIRE(hiddenState);
      REQUIRE(disabledState);
      CHECK(custom->visible);
      CHECK(custom->enabled);
      CHECK_FALSE(hiddenState->visible);
      CHECK(hiddenState->enabled);
      CHECK(disabledState->visible);
      CHECK_FALSE(disabledState->enabled);
      REQUIRE(tree.inputEligible(plain.node()));
      CHECK_FALSE(*tree.inputEligible(plain.node()));
      REQUIRE(tree.setEnabled(parent.node(), true));
      CHECK(*tree.inputEligible(plain.node()));
      const auto hiddenSize = tree.measure(hidden.node(), {40, 10});
      const auto disabledSize = tree.measure(disabled.node(), {40, 10});

      REQUIRE(hiddenSize);
      REQUIRE(disabledSize);
      CHECK(*hiddenSize == CellSize{});
      CHECK(disabledSize->width > 0);

      const auto viewport = CellRect::from({}, {40, 10});

      REQUIRE(viewport);
      REQUIRE(tree.arrange(application.root(), *viewport, *viewport));
      REQUIRE(tree.arrange(scene.root(), *viewport, *viewport));
      REQUIRE(tree.arrange(parent.node(), *viewport, *viewport));
      REQUIRE(tree.arrange(disabled.node(), *viewport, *viewport));

      PaintCounter paint;

      REQUIRE(tree.frame([&] {
        REQUIRE(tree.paint(hidden.node(), paint));
        CHECK(paint.commands == 0);
        REQUIRE(tree.paint(disabled.node(), paint));
        CHECK(paint.commands > 0);
      }));
      configured = true;
    });

    REQUIRE(owner.prepareChecked());
    CHECK(argumentsObserved);
    CHECK(configured);
    CHECK(Plain::alive == 1);
    REQUIRE(owner.prepareChecked());
    CHECK(Plain::alive == 1);
    REQUIRE(owner.close());
    CHECK(Plain::alive == 0);
  }
}

TEST_CASE("instance flag validation precedes all resource callbacks and retains exact property positions") {
  struct Scenario {
    std::string_view body;
    std::string_view key;
    Code code;
  };

  constexpr Scenario SCENARIOS[]{
    {"VBox{Label{text=i18n(\"early\")} Plain{visible=maybe}}", "visible", Code::INVALID_VALUE},
    {"VBox{Label{text=i18n(\"early\")} Plain{enabled=1}}", "enabled", Code::INVALID_VALUE},
    {"VBox{Label{text=i18n(\"early\")} Plain{visible=true visible=false}}", "visible", Code::DUPLICATE_PROPERTY},
    {"VBox{Label{text=i18n(\"early\")} Plain{enabled=false enabled=true}}", "enabled", Code::DUPLICATE_PROPERTY},
  };

  for (const bool configuration : {false, true}) {
    for (const auto &scenario : SCENARIOS) {
      int factories{}, resolutions{};
      ui::SceneResourceOptions options;

      options.format = configuration ? ui::SceneResourceFormat::CONFIGURATION : ui::SceneResourceFormat::LEGACY;
      options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
        ++resolutions;

        return "resolved";
      };
      REQUIRE(options.registry.add<Plain>("Plain", [&](ui::SceneResourceArguments) {
        ++factories;

        return ui::custom<Plain>();
      }));

      const auto source = configuration ? "scene{" + std::string(scenario.body) + "}" : std::string(scenario.body);
      const auto failure = rejected(source, options);

      CHECK(failure.sceneResource->code == scenario.code);
      CHECK(failure.sceneResource->offset == source.rfind(scenario.key));
      REQUIRE(failure.sceneResource->propertyPath);
      CHECK(*failure.sceneResource->propertyPath == "Plain." + std::string(scenario.key));
      CHECK(factories == 0);
      CHECK(resolutions == 0);
      CHECK(Plain::alive == 0);
    }
  }
}

TEST_CASE("instance flags stay outside named settings and consume ordinary node property budgets") {
  for (const auto source :
       {"defaults{Label{visible=false}} scene{Label{}}", "definitions{Hidden=Label{enabled=false}} scene{Hidden{}}"}) {
    const auto failure = rejected(source, {.format = ui::SceneResourceFormat::CONFIGURATION});

    CHECK(failure.sceneResource->code == Code::UNKNOWN_PROPERTY);
  }

  for (const bool configuration : {false, true}) {
    ui::SceneResourceOptions options;

    options.format = configuration ? ui::SceneResourceFormat::CONFIGURATION : ui::SceneResourceFormat::LEGACY;
    options.limits.maxProperties = 1;

    const std::string body = "Label{visible=true enabled=false}";
    const auto source = configuration ? "scene{" + body + "}" : body;
    const auto failure = rejected(source, options);

    CHECK(failure.sceneResource->code == Code::PROPERTY_LIMIT);
    CHECK(failure.sceneResource->offset == source.find("enabled"));
  }
}

TEST_CASE("declarative resource Grid uses generic instance flags in both formats") {
  for (const bool configuration : {false, true}) {
    ui::SceneResourceOptions options;

    options.format = configuration ? ui::SceneResourceFormat::CONFIGURATION : ui::SceneResourceFormat::LEGACY;

    const std::string body = "Grid#grid{columns=1 visible=false enabled=false Label{}}";
    auto owner = ui::scene(configuration ? "scene{" + body + "}" : body, options);
    auto grid = owner.get<Grid>("grid");
    bool configured{};

    owner.configure([&](ManagedScene &, Application &application) {
      const auto snapshot = application.tree().inspect(grid.node());

      REQUIRE(snapshot);
      CHECK_FALSE(snapshot->visible);
      CHECK_FALSE(snapshot->enabled);
      configured = true;
    });

    REQUIRE(owner.prepareChecked());
    CHECK(configured);
    REQUIRE(owner.close());
  }
}

TEST_CASE("description factories can borrow the parent node as an lvalue") {
  bool invoked{};
  auto recipe = ui::detail::Access::describe<Custom>([&](ManagedScene &scene, Node<Widget> &parent) {
    invoked = true;

    return scene.emplace<Custom>(parent, "borrowed parent");
  });
  auto owner = ui::scene(std::move(recipe));

  CHECK_FALSE(invoked);
  REQUIRE(owner.prepareChecked());
  CHECK(invoked);
  CHECK(Custom::alive == 1);
  REQUIRE(owner.close());
  CHECK(Custom::alive == 0);
}

TEST_CASE("runtime resource retains temporary source and publishes typed named refs") {
  auto owner = portals::ui::scene(std::string("VBox{ LineEdit#message{\"\"} Button#add{\"Add\"} }"));
  auto add = owner.get<portals::Button>("add");

  REQUIRE(owner.prepareChecked());
  REQUIRE(add.text().get());
  CHECK(*add.text().get() == "Add");
  REQUIRE(owner.close());
  CHECK_FALSE(add.text().get());
}

TEST_CASE("resource classes preserve absent empty and explicit factory overrides in both grammars") {
  struct ClassPaint final : PaintContext {
    std::size_t commands{};
    CellRect expectedClip;

    std::expected<void, TreeError> submit(std::span<const RenderCommand> values) override {
      for (const auto &value : values) {
        std::visit(
          [this](const auto &command) {
            CHECK(command.style.foreground == RgbColor{1, 2, 3});
            CHECK(command.style.italic);
            CHECK(command.clip == expectedClip);
          },
          value);
      }

      commands += values.size();

      return {};
    }
  };

  for (const bool configuration : {false, true}) {
    ui::SceneResourceOptions options;

    options.format = configuration ? ui::SceneResourceFormat::CONFIGURATION : ui::SceneResourceFormat::LEGACY;
    REQUIRE(options.registry.add<Plain>("Plain", [](ui::SceneResourceArguments arguments) {
      CHECK(arguments.classes == "accent");

      return ui::custom<Plain>().classes("factory");
    }));
    REQUIRE(options.registry.add<Button>("Styled", [](ui::SceneResourceArguments arguments) {
      return ui::button(arguments.text)
        .style(StyleTarget{.type = "Fancy", .classes = {"factory"}, .control = true, .baseTypes = {"Base"}});
    }));

    const std::string body =
      "VBox#group{classes=\"group\" Plain#plain{classes=\"accent\"} "
      "Styled#override{text=\"Paint\" classes=\"accent\"} Styled#absent{} Styled#clear{classes=\"\"} "
      "Grid#grid{columns=1 classes=\"accent\" Label{}}}";
    auto owner =
      ui::scene(configuration ? "scene{" + body + "}" : body, options) |
      ui::style(".accent { foreground:#010203; } Fancy.accent { background:#040506; } .factory { bold:true; } "
                "Control { italic:true; } #override { underline:true; } .group { bold:true; }");
    auto plain = owner.get<Plain>("plain");
    auto button = owner.get<Button>("override");
    auto absent = owner.get<Button>("absent");
    auto clear = owner.get<Button>("clear");
    auto group = owner.get<Column>("group");
    auto grid = owner.get<Grid>("grid");
    ManagedScene *managed{};
    Application *application{};

    owner.configure([&](ManagedScene &scene, Application &app) {
      managed = &scene;
      application = &app;
    });

    REQUIRE(owner.prepareChecked());
    REQUIRE(managed->prepare(*application, {40, 10}));
    REQUIRE(application->tree().read(plain.node(), [](const Plain &widget) {
      REQUIRE(widget.computedStyle());
      CHECK(widget.computedStyle()->normal.foreground == RgbColor{1, 2, 3});
      CHECK_FALSE(widget.computedStyle()->normal.bold);
    }));
    REQUIRE(application->tree().read(button.node(), [](const Button &widget) {
      REQUIRE(widget.computedStyle());
      CHECK(widget.computedStyle()->normal.foreground == RgbColor{1, 2, 3});
      CHECK(widget.computedStyle()->normal.italic);
      CHECK(widget.computedStyle()->normal.underline);
      CHECK(widget.computedStyle()->normal.background == RgbColor{4, 5, 6});
      CHECK_FALSE(widget.computedStyle()->normal.bold);
    }));
    REQUIRE(application->tree().read(absent.node(), [](const Button &widget) {
      REQUIRE(widget.computedStyle());
      CHECK(widget.computedStyle()->normal.bold);
    }));
    REQUIRE(application->tree().read(clear.node(), [](const Button &widget) {
      REQUIRE(widget.computedStyle());
      CHECK_FALSE(widget.computedStyle()->normal.bold);
    }));
    REQUIRE(application->tree().read(group.node(), [](const Column &widget) {
      REQUIRE(widget.computedStyle());
      CHECK(widget.computedStyle()->normal.bold);
    }));
    REQUIRE(application->tree().read(grid.node(), [](const Grid &widget) {
      REQUIRE(widget.computedStyle());
      CHECK(widget.computedStyle()->normal.foreground == RgbColor{1, 2, 3});
    }));

    ClassPaint paint;
    const auto viewport = CellRect::from({}, {40, 10});

    REQUIRE(viewport);
    REQUIRE(application->tree().arrange(application->root(), *viewport, *viewport));
    REQUIRE(application->tree().arrange(managed->root(), *viewport, *viewport));
    REQUIRE(application->tree().arrange(group.node(), *viewport, *viewport));
    REQUIRE(application->tree().arrange(button.node(), *viewport, *viewport));
    paint.expectedClip = *viewport;

    REQUIRE(application->tree().frame([&] {
      REQUIRE(application->tree().paint(button.node(), paint));
    }));
    CHECK(paint.commands > 0);
    REQUIRE(owner.close());
    CHECK(Plain::alive == 0);
  }
}

TEST_CASE("classes literals validate before early resource resolvers and factories") {
  struct Scenario {
    std::string_view body;
    Code code;
    std::string_view marker;
    std::size_t adjustment{};
  };

  constexpr Scenario SCENARIOS[]{
    {"VBox{Label{text=i18n(\"early\")} Plain{classes=true}}", Code::SYNTAX, "true"},
    {"VBox{Label{text=i18n(\"early\")} Plain{classes=i18n(\"late\")}}", Code::SYNTAX, "i18n(\"late\")"},
    {"VBox{Label{text=i18n(\"early\")} Plain{classes=\"a\" classes=\"b\"}}", Code::DUPLICATE_PROPERTY, "classes"},
    {"VBox{Label{text=i18n(\"early\")} Plain{classes=\"a\" classes=\"bad\\q\"}}", Code::DUPLICATE_PROPERTY, "classes"},
    {"VBox{Label{text=i18n(\"early\")} Plain{classes=\"bad\\q\"}}", Code::SYNTAX, "\\q", 1},
  };

  for (const bool configuration : {false, true}) {
    for (const auto &scenario : SCENARIOS) {
      INFO("configuration=" << configuration << "; source=" << scenario.body);

      int factories{}, resolutions{};
      ui::SceneResourceOptions options;

      options.format = configuration ? ui::SceneResourceFormat::CONFIGURATION : ui::SceneResourceFormat::LEGACY;
      options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
        ++resolutions;

        return "resolved";
      };
      REQUIRE(options.registry.add<Plain>("Plain", [&](ui::SceneResourceArguments) {
        ++factories;

        return ui::custom<Plain>();
      }));

      const auto source = configuration ? "scene{" + std::string(scenario.body) + "}" : std::string(scenario.body);
      const auto failure = rejected(source, options);

      CHECK(failure.sceneResource->code == scenario.code);
      CHECK(failure.sceneResource->offset == source.rfind(scenario.marker) + scenario.adjustment);
      REQUIRE(failure.sceneResource->propertyPath);
      CHECK(*failure.sceneResource->propertyPath == "Plain.classes");
      CHECK(factories == 0);
      CHECK(resolutions == 0);
      CHECK(Plain::alive == 0);
    }
  }
}

TEST_CASE("classes stay outside inherited settings and respect existing parser budgets") {
  for (const auto source : {"defaults{Label{classes=\"accent\"}} scene{Label{}}",
                            "definitions{Alias=Label{classes=\"accent\"}} scene{Alias{}}"}) {
    const auto failure = rejected(source, {.format = ui::SceneResourceFormat::CONFIGURATION});

    CHECK(failure.sceneResource->code == Code::UNKNOWN_PROPERTY);
  }

  for (const bool configuration : {false, true}) {
    ui::SceneResourceOptions options;

    options.format = configuration ? ui::SceneResourceFormat::CONFIGURATION : ui::SceneResourceFormat::LEGACY;
    options.limits.maxProperties = 1;

    const std::string body = "Label{classes=\"one\" visible=true}";
    const auto source = configuration ? "scene{" + body + "}" : body;
    const auto failure = rejected(source, options);

    CHECK(failure.sceneResource->code == Code::PROPERTY_LIMIT);
    CHECK(failure.sceneResource->offset == source.find("visible"));
    options.limits.maxStringBytes = 2;

    const std::string oversized = "Label{classes=\"abc\"}";
    const auto stringFailure = rejected(configuration ? "scene{" + oversized + "}" : oversized, options);

    CHECK(stringFailure.sceneResource->code == Code::STRING_LIMIT);
    REQUIRE(stringFailure.sceneResource->propertyPath);
    CHECK(*stringFailure.sceneResource->propertyPath == "Label.classes");
  }
}

TEST_CASE("configuration range scan preserves quoted braces and typed string diagnostics") {
  auto owner =
    ui::scene(R"(scene { Alias#text { text="{} \"quoted\" \\ end" classes="accent" } } definitions { Alias=Label{} })",
              {.format = ui::SceneResourceFormat::CONFIGURATION});
  auto text = owner.get<Label>("text");

  REQUIRE(owner.prepareChecked());
  REQUIRE(text.text().get());
  CHECK(*text.text().get() == "{} \"quoted\" \\ end");
  REQUIRE(owner.close());

  const std::string source = "scene{Label{text=\"bad\nvalue\"}}";
  const auto failure = rejected(source, {.format = ui::SceneResourceFormat::CONFIGURATION});

  CHECK(failure.sceneResource->code == Code::SYNTAX);
  CHECK(failure.sceneResource->offset == source.find('\n'));
  REQUIRE(failure.sceneResource->propertyPath);
  CHECK(*failure.sceneResource->propertyPath == "Label.text");
}

TEST_CASE("configuration unterminated quote guard rejects before resource callbacks") {
  const std::string unfinished = R"(scene{VBox{Label{text=i18n("early")} Plain{classes="unfinished}}})";

  for (const auto &source : {unfinished, unfinished + '\\'}) {
    int factories{}, resolutions{};
    ui::SceneResourceOptions options;

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;

      return "resolved";
    };
    REQUIRE(options.registry.add<Plain>("Plain", [&](ui::SceneResourceArguments) {
      ++factories;

      return ui::custom<Plain>();
    }));

    const auto failure = rejected(source, options);

    CHECK(failure.sceneResource->code == Code::SYNTAX);
    CHECK(failure.sceneResource->offset == source.find('"', source.find("classes=")));
    CHECK(factories == 0);
    CHECK(resolutions == 0);
    CHECK(Plain::alive == 0);
  }
}

TEST_CASE("configured resources resolve forward definitions and preserve independent sparse history") {
  constexpr std::string_view SOURCE = R"(
    scene { VBox {
      MyEdit#first { text = "" }
      MyEdit#second { text = "" }
      MyEdit#disabled { history = { maxOperations = 0 } }
      MyEdit#explicit { history { maxOperations = 128, enabled = true } }
    } }
    defaults { LineEdit { history { enabled = false, maxBytes = 4096 } } }
    definitions {
      MyEdit = Later { history { maxOperations = 2 } }
      Later = LineEdit { history { enabled = true } }
    }
  )";

  for (const bool fromFile : {false, true}) {
    TemporaryFile file;

    file.write(SOURCE);

    ui::SceneResourceOptions options;

    options.format = ui::SceneResourceFormat::CONFIGURATION;

    auto owner = fromFile ? ui::scene(ui::SceneResourceFile{file.path}, options) : ui::scene(SOURCE, options);
    auto first = owner.get<LineEdit>("first");
    auto second = owner.get<LineEdit>("second");
    auto disabled = owner.get<LineEdit>("disabled");
    auto explicitValue = owner.get<LineEdit>("explicit");

    REQUIRE(owner.prepareChecked());

    for (const auto value : {"a", "b", "c"}) {
      REQUIRE((first.text() = value).result());
      REQUIRE((disabled.text() = value).result());
      REQUIRE((explicitValue.text() = value).result());
    }

    REQUIRE((second.text() = "independent").result());
    REQUIRE(first.undo().result());
    CHECK(*first.text().get() == "b");
    REQUIRE(first.undo().result());
    CHECK(*first.text().get() == "a");
    REQUIRE(first.undo().result());
    CHECK(*first.text().get() == "a");
    CHECK(*second.text().get() == "independent");
    REQUIRE(disabled.undo().result());
    CHECK(*disabled.text().get() == "c");

    for (int index = 0; index < 3; ++index) {
      REQUIRE(explicitValue.undo().result());
    }

    CHECK(*explicitValue.text().get() == "");
    REQUIRE(owner.close());
    CHECK_FALSE(first.text().get());
  }
}

TEST_CASE("configured resources validate every layer before resolvers or factories") {
  struct Scenario {
    std::string_view source;
    Code code;
    std::string_view path;
  };

  const Scenario scenarios[]{
    {"definitions { A = B {} B = A {} } scene { Probe{} }", Code::CYCLIC_TYPE, {}},
    {"definitions { A = Missing {} } scene { Probe{} }", Code::UNKNOWN_TYPE, {}},
    {"definitions { LineEdit = Button {} } scene { Probe{} }", Code::DUPLICATE_TYPE, {}},
    {"defaults { LineEdit{} LineEdit{} } scene { Probe{} }", Code::DUPLICATE_PROPERTY, {}},
    {"definitions { A = LineEdit { missing=1 } } scene { Probe{} }", Code::UNKNOWN_PROPERTY, "A.missing"},
    {"definitions { A = Label { readOnly=true } } scene { Probe{} }", Code::UNKNOWN_PROPERTY, "A.readOnly"},
    {"defaults { LineEdit { editMode=UNKNOWN } } scene { Probe{} }", Code::INVALID_VALUE, "LineEdit.editMode"},
    {"scene { VBox { Probe{} LineEdit { history { maxBytes=1 maxBytes=2 } } } }", Code::DUPLICATE_PROPERTY,
     "LineEdit.history.maxBytes"},
    {"scene { VBox { Probe{} LineEdit { history{} history={} } } }", Code::DUPLICATE_PROPERTY, "LineEdit.history"},
    {"scene { VBox { Probe{} LineEdit { history { maxBytes=$budget } } } }", Code::INVALID_VALUE, {}},
    {"scene { VBox { Probe{} ThreeState { cycle=TWO_STATE } } }", Code::INVALID_VALUE, "ThreeState.cycle"},
    {"defaults { Label { readOnly=true } } scene { VBox { Probe{} Label { text=\"override\" } } }",
     Code::UNKNOWN_PROPERTY, "Label.readOnly"},
  };

  for (const auto &scenario : scenarios) {
    CAPTURE(scenario.source);

    int factories{}, resolutions{};
    ui::SceneResourceOptions options;

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;

      return "resolved";
    };
    REQUIRE(options.registry.add<Custom>("Probe", [&](ui::SceneResourceArguments) {
      ++factories;

      return ui::custom<Custom>("probe");
    }));

    const auto failure = rejected(scenario.source, options);

    CHECK(failure.sceneResource->code == scenario.code);

    if (!scenario.path.empty()) {
      REQUIRE(failure.sceneResource->propertyPath);
      CHECK(*failure.sceneResource->propertyPath == scenario.path);
    }

    CHECK(factories == 0);
    CHECK(resolutions == 0);
    CHECK(Custom::alive == 0);
  }
}

TEST_CASE("configured and C++ linear settings reach real Row and Column arrangement") {
  for (const bool configuration : {false, true}) {
    ui::ControlContext controls;
    ui::ControlSettings defaults;
    defaults.grid = {.paddingLeft = 1, .paddingTop = 2, .paddingRight = 3, .paddingBottom = 4};
    defaults.linear = {
      .mainAlign = LayoutAlign::END, .crossAlign = LayoutAlign::CENTER, .overflow = LayoutOverflow::CLIP};
    controls.defaults("VBox", defaults).defaults("HBox", defaults);
    ui::Ref<Column> vertical;
    ui::Ref<Row> horizontal;
    auto owner = [&] {
      if (configuration) {
        ui::SceneResourceOptions options;
        options.format = ui::SceneResourceFormat::CONFIGURATION;

        return ui::scene(
          "defaults { VBox { padding{left=1 top=2 right=3 bottom=4} mainAlign=END crossAlign=CENTER overflow=CLIP } "
          "HBox { padding{left=1 top=2 right=3 bottom=4} mainAlign=END crossAlign=CENTER overflow=CLIP } } "
          "definitions { Padded=VBox{padding{left=0}} } "
          "scene{Padded#vertical{width=20 height=10 HBox#horizontal{width=20 height=10}}}",
          options);
      }

      std::vector<ui::Description<Row>> children;
      children.push_back(ui::row().reference(horizontal).width(20).height(10));

      return ui::scene(ui::column(LinearOptions{.gap = 3}, std::move(children))
                         .settings({.grid = {.paddingLeft = 0}})
                         .reference(vertical)
                         .width(20)
                         .height(10),
                       {.controls = controls});
    }();

    if (configuration) {
      vertical = owner.get<Column>("vertical");
      horizontal = owner.get<Row>("horizontal");
    }

    owner.configure([&](ManagedScene &, Application &application) {
      const std::array children{MeasuredChild{.desired = {2, 1}}};

      REQUIRE(application.tree().read(vertical.node(), [&](const Column &widget) {
        const auto options = widget.options();
        CHECK(options.gap == (configuration ? 0 : 3));
        CHECK(options.padding.left == 0);
        CHECK(options.padding.top == 2);
        CHECK(options.padding.right == 3);
        CHECK(options.padding.bottom == 4);
        CHECK(widget.sizePolicy().width.preferred == 20);
        const auto arranged = widget.arrangeChildren(children, {20, 10});
        REQUIRE(arranged);
        REQUIRE(arranged->size() == 1);
        CHECK(arranged->front().origin() == CellPoint{7, 5});
        CHECK(arranged->front().size() == CellSize{2, 1});
      }));
      REQUIRE(application.tree().read(horizontal.node(), [&](const Row &widget) {
        const auto arranged = widget.arrangeChildren(children, {20, 10});
        REQUIRE(arranged);
        REQUIRE(arranged->size() == 1);
        CHECK(arranged->front().origin() == CellPoint{15, 3});
        CHECK(arranged->front().size() == CellSize{2, 1});
      }));
    });

    REQUIRE(owner.prepareChecked());
    REQUIRE(owner.close());
  }
}

TEST_CASE("configured linear and text settings reject before resolver or custom factory effects") {
  struct Scenario {
    std::string_view source;
    std::string_view property;
    std::string_view path;
    Code code;
  };

  const Scenario scenarios[]{
    {"scene{VBox{Probe{} Label{text=i18n(\"early\")} HBox{mainAlign=WRONG}}}", "mainAlign", "HBox.mainAlign",
     Code::INVALID_VALUE},
    {"scene{VBox{Probe{} Label{text=i18n(\"early\")} VBox{crossAlign=wrong}}}", "crossAlign", "VBox.crossAlign",
     Code::INVALID_VALUE},
    {"scene{VBox{Probe{} Label{text=i18n(\"early\")} HBox{overflow=SCROLL}}}", "overflow", "HBox.overflow",
     Code::INVALID_VALUE},
    {"scene{VBox{Probe{} Label{text=i18n(\"early\")} VBox{padding{left=-1}}}}", "left", "VBox.padding.left",
     Code::INVALID_VALUE},
    {"scene{VBox{Probe{} Label{text=i18n(\"early\")} Button{mainAlign=END}}}", "mainAlign", "Button.mainAlign",
     Code::UNKNOWN_PROPERTY},
    {"scene{VBox{Probe{} Label{text=i18n(\"early\")} LineEdit{padding{top=1}}}}", "top", "LineEdit.padding.top",
     Code::UNKNOWN_PROPERTY},
    {"scene{VBox{Probe{} Label{text=i18n(\"early\")} Button{limits{maxCells=1}}}}", "maxCells",
     "Button.limits.maxCells", Code::UNKNOWN_PROPERTY},
    {"scene{VBox{Probe{} Label{text=i18n(\"early\")} LineEdit{limits{maxInputBytes=-1}}}}", "maxInputBytes",
     "LineEdit.limits.maxInputBytes", Code::INVALID_VALUE},
    {"scene{VBox{Probe{} Label{text=i18n(\"early\")} LineEdit{limits{maxCells=18446744073709551616}}}}", "maxCells",
     "LineEdit.limits.maxCells", Code::INVALID_VALUE},
    {"scene{VBox{Probe{} Label{text=i18n(\"early\")} LineEdit{limits{maxCells=1oops}}}}", "maxCells",
     "LineEdit.limits.maxCells", Code::SYNTAX},
    {"scene{VBox{Probe{} Label{text=i18n(\"early\")} LineEdit{limits{maxBytes=1}}}}", "maxBytes",
     "LineEdit.limits.maxBytes", Code::UNKNOWN_PROPERTY},
  };

  for (const auto &scenario : scenarios) {
    CAPTURE(scenario.source);
    int factories{}, resolutions{};
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;

      return "private resolver output";
    };
    REQUIRE(options.registry.add<Custom>("Probe", [&](ui::SceneResourceArguments) {
      ++factories;

      return ui::custom<Custom>("probe");
    }));

    const auto failure = rejected(scenario.source, options);
    REQUIRE(failure.sceneResource->propertyPath);
    CHECK(*failure.sceneResource->propertyPath == scenario.path);
    CHECK(failure.sceneResource->code == scenario.code);
    const auto propertyOffset = scenario.source.find(scenario.property);

    if (scenario.code == Code::UNKNOWN_PROPERTY || scenario.property == "mainAlign" ||
        scenario.property == "crossAlign" || scenario.property == "overflow") {
      CHECK(failure.sceneResource->offset == propertyOffset);
    }

    CHECK(factories == 0);
    CHECK(resolutions == 0);
  }
}

TEST_CASE("effective LineEdit limits validate initial text and placeholder before any resource factory") {
  struct Bound {
    std::string_view path;
    std::string_view value;
    TextErrorKind kind;
  };

  const Bound bounds[]{
    {"maxInputBytes", "ab", TextErrorKind::INPUT_LIMIT},
    {"maxClusterBytes", "\xc3\xa9", TextErrorKind::CLUSTER_LIMIT},
    {"maxGraphemes", "ab", TextErrorKind::GRAPHEME_LIMIT},
    {"maxCells", "\xe7\x95\x8c", TextErrorKind::CELL_LIMIT},
  };

  for (const auto &bound : bounds) {
    for (const auto property : {"text", "placeholder"}) {
      for (const bool localized : {false, true}) {
        CAPTURE(bound.path);
        CAPTURE(property);
        CAPTURE(localized);
        int factories{}, resolutions{};
        ui::SceneResourceOptions options;
        options.format = ui::SceneResourceFormat::CONFIGURATION;
        options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
          ++resolutions;

          return std::string(bound.value);
        };
        REQUIRE(options.registry.add<Custom>("Probe", [&](ui::SceneResourceArguments) {
          ++factories;

          return ui::custom<Custom>("probe");
        }));
        const auto expression =
          localized ? std::string("i18n(\"secret-key\")") : std::string("\"") + std::string(bound.value) + "\"";
        const auto source = std::string("definitions{Bounded=LineEdit{limits{") + std::string(bound.path) + "=1} " +
                            property + "=" + expression + "}} scene{VBox{Probe{} Bounded{}}}";
        const auto failure = rejected(source, options);

        REQUIRE(failure.text);
        CHECK(failure.text->kind == bound.kind);
        CHECK(failure.sceneResource->code == Code::TEXT);
        REQUIRE(failure.sceneResource->propertyPath);
        CHECK(*failure.sceneResource->propertyPath == std::string("Bounded.") + property);
        CHECK(failure.sceneResource->offset == source.find(std::string(property) + "="));
        CHECK(factories == 0);
        CHECK(resolutions == (localized ? 1 : 0));
      }
    }
  }
}

TEST_CASE("new structured limits respect depth and materialization budgets before callbacks") {
  for (const bool depth : {false, true}) {
    int factories{}, resolutions{};
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.controls.emplace(depth ? ui::ControlContextLimits{.maxStructDepth = 1}
                                   : ui::ControlContextLimits{.maxMaterializedLeaves = 1});
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;

      return "value";
    };
    REQUIRE(options.registry.add<Custom>("Probe", [&](ui::SceneResourceArguments) {
      ++factories;

      return ui::custom<Custom>("probe");
    }));

    const auto failure =
      rejected("scene{VBox{Probe{} Label{text=i18n(\"early\")} LineEdit{limits{maxCells=2 maxGraphemes=2}}}}", options);

    CHECK(failure.sceneResource->code == (depth ? Code::DEPTH_LIMIT : Code::MATERIALIZATION_LIMIT));
    REQUIRE(failure.sceneResource->propertyPath);
    CHECK(*failure.sceneResource->propertyPath == (depth ? "LineEdit.limits" : "LineEdit.limits.maxCells"));
    CHECK(factories == 0);
    CHECK(resolutions == 0);
  }
}

TEST_CASE("LineEdit sparse limits preserve empty zero values and independent sibling bounds") {
  for (const bool configuration : {false, true}) {
    ui::ControlContext controls;
    controls
      .defaults("LineEdit", {.limits = {.maxInputBytes = 4, .maxClusterBytes = 4, .maxGraphemes = 2, .maxCells = 4}})
      .define("One", "LineEdit", {.limits = {.maxGraphemes = 1}});
    ui::Ref<LineEdit> narrow, wide, empty;
    auto owner = [&] {
      if (configuration) {
        ui::SceneResourceOptions options;
        options.format = ui::SceneResourceFormat::CONFIGURATION;

        return ui::scene("defaults{LineEdit{limits{maxInputBytes=4 maxClusterBytes=4 maxGraphemes=2 maxCells=4}}} "
                         "definitions{One=LineEdit{limits{maxGraphemes=1}}} "
                         "scene{VBox{HBox{One#narrow{text=\"a\"}} LineEdit#wide{text=\"ab\"} "
                         "LineEdit#empty{text=\"\" placeholder=\"\" limits{maxInputBytes=0 maxClusterBytes=0 "
                         "maxGraphemes=0 maxCells=0}}}}",
                         options);
      }

      return ui::scene(
        ui::column(ui::row(ui::lineEdit(narrow, "a").variant("One")), ui::lineEdit(wide, "ab"),
                   ui::lineEdit(empty, "").settings(
                     {.placeholder = text(""),
                      .limits = {.maxInputBytes = 0, .maxClusterBytes = 0, .maxGraphemes = 0, .maxCells = 0}})),
        {.controls = controls});
    }();

    if (configuration) {
      narrow = owner.get<LineEdit>("narrow");
      wide = owner.get<LineEdit>("wide");
      empty = owner.get<LineEdit>("empty");
    }

    REQUIRE(owner.prepareChecked());
    CHECK(*narrow.text().get() == "a");
    CHECK(*wide.text().get() == "ab");
    CHECK(*empty.text().get() == "");
    CHECK_FALSE((narrow.text() = "ab").result());
    CHECK_FALSE((empty.text() = "a").result());
    REQUIRE((wide.text() = "cd").result());
    CHECK(*narrow.text().get() == "a");
    REQUIRE(owner.close());
  }
}

TEST_CASE("high text limit leaves are counted once for prepared resources and added for factory overrides") {
  ui::SceneResourceOptions exact;
  exact.format = ui::SceneResourceFormat::CONFIGURATION;
  exact.controls.emplace(ui::ControlContextLimits{.maxMaterializedLeaves = 1});
  auto prepared = ui::scene("scene{LineEdit#empty{limits{maxCells=0}}}", exact);
  auto empty = prepared.get<LineEdit>("empty");

  REQUIRE(prepared.prepareChecked());
  CHECK(*empty.text().get() == "");
  REQUIRE(prepared.close());

  for (const std::size_t budget : {1U, 2U}) {
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.controls.emplace(ui::ControlContextLimits{.maxMaterializedLeaves = budget});
    REQUIRE(options.registry.add<LineEdit>("FactoryEdit", [](ui::SceneResourceArguments arguments) {
      return ui::lineEdit(arguments.text).settings({.limits = {.maxCells = 1}});
    }));
    auto owner = ui::scene("scene{FactoryEdit#editor{text=\"a\"}}", options);
    auto editor = owner.get<LineEdit>("editor");
    const auto result = owner.prepareChecked();

    if (budget == 1) {
      REQUIRE_FALSE(result);
      REQUIRE(result.error().sceneResource);
      CHECK(result.error().sceneResource->code == Code::MATERIALIZATION_LIMIT);
      CHECK(editor.node() == Node<LineEdit>{});
    } else {
      REQUIRE(result);
      CHECK(*editor.text().get() == "a");
      CHECK_FALSE((editor.text() = "ab").result());
    }

    REQUIRE(owner.close());
  }
}

TEST_CASE("C++ LineEdit initial and placeholder text use the same effective sparse limits") {
  for (const bool placeholder : {false, true}) {
    ui::ControlSettings settings;
    settings.limits.maxCells = 0;

    if (placeholder) {
      settings.placeholder = text("a");
    }

    auto owner = ui::scene(ui::lineEdit(placeholder ? "" : "a").settings(settings));
    const auto result = owner.prepareChecked();

    REQUIRE_FALSE(result);
    REQUIRE(result.error().text);
    CHECK(result.error().text->kind == TextErrorKind::CELL_LIMIT);
    REQUIRE(owner.close());
  }
}

TEST_CASE("prepared high-bit settings survive the precharged mount handoff without a second leaf charge") {
  for (const std::size_t budget : {2U, 3U}) {
    for (const bool addLeaf : {false, true}) {
      INFO("budget=", budget, ", addLeaf=", addLeaf);

      ui::ControlSettings materialized;
      // Positional empty text is still a present source leaf in the normal LineEdit recipe.
      materialized.text = text("");
      materialized.limits.maxCells = 0;

      auto patch = materialized;

      if (addLeaf) {
        patch.limits.maxGraphemes = 0;
      }

      ui::Ref<LineEdit> editor;
      auto child = ui::lineEdit(editor, "").settings(patch);
      ui::detail::Access::preparedControlPresence(child, ui::detail::controlSettingsPresenceSet(materialized));
      auto handoff = ui::detail::Access::transaction(
        [child = std::move(child),
         materialized](ManagedScene &scene, Node<Widget> parent,
                       ui::detail::MountContext &context) mutable -> std::expected<Node<Widget>, ManagedError> {
          const auto prepared = context.controls.resolve("LineEdit", materialized);

          if (!prepared) {
            return std::unexpected(ui::detail::controlContextError(prepared.error(), parent));
          }

          // The loader reserves resolved source leaves before handing its prepared recipe to mount.
          context.controlLeaves += prepared->materializedLeaves;
          auto mounted = ui::detail::Access::mount(std::move(child), scene, parent, context);

          if (!mounted) {
            return std::unexpected(mounted.error());
          }

          return Node<Widget>(*mounted);
        });
      ui::SceneOptions options;
      options.controls = ui::ControlContext({.maxMaterializedLeaves = budget});
      auto owner = ui::scene(std::move(handoff), options);
      const auto result = owner.prepareChecked();
      const auto errorCode =
        !result && result.error().sceneResource ? static_cast<int>(result.error().sceneResource->code) : -1;

      INFO("prepareSucceeded=", result.has_value(), ", sceneResourceErrorCode=", errorCode);

      if (addLeaf && budget == 2) {
        REQUIRE_FALSE(result);
        REQUIRE(result.error().sceneResource);
        CHECK(result.error().sceneResource->code == Code::MATERIALIZATION_LIMIT);
        CHECK(editor.node() == Node<LineEdit>{});
      } else {
        REQUIRE(result);
        CHECK(*editor.text().get() == "");
      }

      REQUIRE(owner.close());
    }
  }
}

TEST_CASE("configuration additions retain legacy adapters and external Grid padding") {
  for (const bool configuration : {false, true}) {
    ui::SceneResourceOptions options;
    options.format = configuration ? ui::SceneResourceFormat::CONFIGURATION : ui::SceneResourceFormat::LEGACY;
    auto owner = ui::scene(configuration ? "scene{Grid#grid{columns=1 padding{left=2 top=1 right=3 bottom=4} Label{}}}"
                                         : "Grid#grid{columns=1 padding{left=2 top=1 right=3 bottom=4} Label{}}",
                           options);
    auto grid = owner.get<Grid>("grid");
    owner.configure([&](ManagedScene &, Application &app) {
      REQUIRE(app.tree().read(grid.node(), [](const Grid &widget) {
        const auto options = widget.options();

        CHECK(options.padding.left == 2);
        CHECK(options.padding.top == 1);
        CHECK(options.padding.right == 3);
        CHECK(options.padding.bottom == 4);
        CHECK(options.columns.size() == 1);
      }));
    });

    REQUIRE(owner.prepareChecked());
    REQUIRE(owner.close());
  }

  auto legacy = ui::scene("VBox#box{padding={left=3}}");
  auto box = legacy.get<Column>("box");
  legacy.configure([&](ManagedScene &, Application &app) {
    REQUIRE(app.tree().read(box.node(), [](const Column &widget) {
      CHECK(widget.options().padding.left == 0);
    }));
  });
  REQUIRE(legacy.prepareChecked());
  REQUIRE(legacy.close());

  ui::SceneResourceOptions typedLegacy;
  typedLegacy.controls.emplace();
  typedLegacy.controls->define("Named", "VBox");
  const auto failure = rejected("Named{padding={left=3}}", typedLegacy);
  CHECK(failure.sceneResource->code == Code::UNKNOWN_PROPERTY);
  REQUIRE(failure.sceneResource->propertyPath);
  CHECK(*failure.sceneResource->propertyPath == "Named.padding.left");
  CHECK(rejected("LineEdit{limits{maxCells=1}}").sceneResource->code == Code::CHILDREN_FORBIDDEN);
  CHECK(rejected("VBox{mainAlign=END}").sceneResource->code == Code::UNKNOWN_PROPERTY);
}

TEST_CASE("primitive resource validation precedes reachable text resolvers and caller factories") {
  struct Scenario {
    std::string_view child;
    Code code;
    std::string_view path;
  };

  const Scenario scenarios[]{
    {"Button{chrome=INVALID}", Code::INVALID_VALUE, "Button.chrome"},
    {"Label{chrome=NONE}", Code::UNKNOWN_PROPERTY, "Label.chrome"},
    {"Border{glyphs=INVALID Label{}}", Code::INVALID_VALUE, "Border.glyphs"},
    {"Border{maxPaintCells=0 Label{}}", Code::INVALID_VALUE, "Border.maxPaintCells"},
    {"Gauge{preferredWidth=-1}", Code::INVALID_VALUE, "Gauge.preferredWidth"},
    {"Gauge{value=nan}", Code::INVALID_VALUE, "Gauge.value"},
    {"Gauge{value=inf}", Code::INVALID_VALUE, "Gauge.value"},
    {"Gauge{value=1e9999}", Code::INVALID_VALUE, "Gauge.value"},
    {"Gauge{value=1e}", Code::SYNTAX, "Gauge.value"},
    {"Gauge{value=.5cells}", Code::SYNTAX, "Gauge.value"},
    {"Gauge{value=$model}", Code::INVALID_VALUE, "Gauge.value"},
    {"Gauge{minimum=2 maximum=1 value=1}", Code::INVALID_VALUE, "Gauge.maximum"},
    {"Gauge{minimum=-1e308 maximum=1e308}", Code::INVALID_VALUE, "Gauge.maximum"},
    {"Gauge{minimum=2 maximum=3}", Code::INVALID_VALUE, "Gauge.minimum"},
    {"Gauge{value=2}", Code::INVALID_VALUE, "Gauge.value"},
    {"Border{}", Code::INVALID_VALUE, "Border.children"},
    {"Border{Label{} Label{}}", Code::INVALID_VALUE, "Border.children"},
    {"Gauge{Label{}}", Code::CHILDREN_FORBIDDEN, {}},
  };

  for (const auto &scenario : scenarios) {
    CAPTURE(scenario.child);
    int factories{}, resolutions{};
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;

      return "early value";
    };
    REQUIRE(options.registry.add<Custom>("Probe", [&](ui::SceneResourceArguments) {
      ++factories;

      return ui::custom<Custom>("probe");
    }));
    const auto source =
      std::string("scene{VBox{Probe{} Label{text=i18n(\"early\")} ") + std::string(scenario.child) + "}}";
    const auto failure = rejected(source, options);

    CHECK(failure.sceneResource->code == scenario.code);

    if (!scenario.path.empty()) {
      REQUIRE(failure.sceneResource->propertyPath);
      CHECK(*failure.sceneResource->propertyPath == scenario.path);
    }

    CHECK(factories == 0);
    CHECK(resolutions == 0);
    CHECK(Custom::alive == 0);
  }
}

TEST_CASE("compact primitive staging preserves untouched resource origins and clears replaced leaf origins") {
  auto source = std::make_shared<ui::ControlSourceOrigin>();
  source->sourceIdentity = "earlier.scene";
  source->byteOffset = 17;
  source->line = 4;
  source->column = 3;

  for (const bool border : {false, true}) {
    for (const bool replaceInvalidLeaf : {false, true}) {
      auto recipe = [&]() -> ui::Description<Widget> {
        if (border) {
          auto description = replaceInvalidLeaf ? ui::border(ui::label("content"))
                                                    .settings({.origin = source, .glyphs = PrimitiveGlyphs::ASCII})
                                                    .glyphs(static_cast<PrimitiveGlyphs>(9))
                                                : ui::border(ui::label("content"))
                                                    .settings({.origin = source, .maxPaintCells = 0})
                                                    .glyphs(PrimitiveGlyphs::ASCII);

          return ui::detail::Access::erase(std::move(description));
        }

        auto description =
          replaceInvalidLeaf
            ? ui::button("caption")
                .settings({.origin = source, .chrome = ButtonChrome::BRACKETS})
                .chrome(static_cast<ButtonChrome>(9))
            : ui::button("caption").settings({.width = -1, .origin = source}).chrome(ButtonChrome::NONE);

        return ui::detail::Access::erase(std::move(description));
      }();
      auto owner = ui::scene(std::move(recipe));
      const auto prepared = owner.prepareChecked();

      REQUIRE_FALSE(prepared);
      REQUIRE(prepared.error().sceneResource);
      const auto &failure = *prepared.error().sceneResource;
      REQUIRE(failure.propertyPath);
      CHECK(*failure.propertyPath == (border ? replaceInvalidLeaf ? "Border.glyphs" : "Border.maxPaintCells"
                                      : replaceInvalidLeaf ? "Button.chrome"
                                                           : "Button.width"));

      if (replaceInvalidLeaf) {
        CHECK_FALSE(failure.source);
        CHECK(failure.line == 0);
      } else {
        REQUIRE(failure.source);
        CHECK(*failure.source == "earlier.scene");
        CHECK(failure.offset == 17);
        CHECK(failure.line == 4);
        CHECK(failure.column == 3);
      }

      REQUIRE(owner.close());
    }
  }
}

TEST_CASE("primitive effective range failures retain inherited declaration origins") {
  TemporaryFile file;
  const std::string source = "definitions{Progress=Gauge{minimum=2 maximum=1}}\n"
                             "scene{VBox{Label{text=i18n(\"early\")} Progress{value=0}}}";
  file.write(source);
  int resolutions{};
  ui::SceneResourceOptions options;
  options.format = ui::SceneResourceFormat::CONFIGURATION;
  options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
    ++resolutions;

    return "early";
  };
  ManagedError failure;

  {
    auto owner = ui::scene(ui::SceneResourceFile{file.path}, options);
    const auto prepared = owner.prepareChecked();

    REQUIRE_FALSE(prepared);
    failure = prepared.error();
  }

  REQUIRE(failure.sceneResource);
  REQUIRE(failure.sceneResource->propertyPath);
  REQUIRE(failure.sceneResource->source);
  CHECK(failure.sceneResource->code == Code::INVALID_VALUE);
  CHECK(*failure.sceneResource->propertyPath == "Progress.maximum");
  CHECK(*failure.sceneResource->source == file.path.generic_string());
  CHECK(failure.sceneResource->offset == source.find("maximum"));
  CHECK(failure.sceneResource->line == 1);
  CHECK(resolutions == 0);
}

TEST_CASE("primitive fields consume whole-document high-bit budgets before callbacks") {
  ui::SceneResourceOptions minimal;
  minimal.format = ui::SceneResourceFormat::CONFIGURATION;
  minimal.controls.emplace(ui::ControlContextLimits{.maxMaterializedLeaves = 1});
  auto empty = ui::scene("scene{Button#empty{chrome=NONE}}", minimal);
  auto button = empty.get<Button>("empty");
  empty.configure([&](ManagedScene &, Application &app) {
    const auto measured = app.tree().measure(button.node(), {});

    REQUIRE(measured);
    CHECK(*measured == CellSize{0, 1});
  });
  REQUIRE(empty.prepareChecked());
  REQUIRE(empty.close());

  for (const std::size_t budget : {2U, 3U}) {
    int factories{}, resolutions{};
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.controls.emplace(ui::ControlContextLimits{.maxMaterializedLeaves = budget});
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;

      return "early";
    };
    REQUIRE(options.registry.add<Custom>("Probe", [&](ui::SceneResourceArguments) {
      ++factories;

      return ui::custom<Custom>("probe");
    }));
    auto owner = ui::scene("scene{VBox{Probe{} Label{text=i18n(\"early\")} Gauge{value=0 glyphs=ASCII}}}", options);
    const auto prepared = owner.prepareChecked();

    if (budget == 2) {
      REQUIRE_FALSE(prepared);
      REQUIRE(prepared.error().sceneResource);
      CHECK(prepared.error().sceneResource->code == Code::MATERIALIZATION_LIMIT);
      CHECK(factories == 0);
      CHECK(resolutions == 0);
    } else {
      REQUIRE(prepared);
      CHECK(factories == 1);
      CHECK(resolutions == 1);
    }

    REQUIRE(owner.close());
    CHECK(Custom::alive == 0);
  }
}

TEST_CASE("primitive fallbacks preserve caller names legacy syntax and exact custom allowlists") {
  for (const auto name : {"Border", "Gauge"}) {
    for (const bool configuration : {false, true}) {
      int factories{};
      ui::SceneResourceOptions options;
      options.format = configuration ? ui::SceneResourceFormat::CONFIGURATION : ui::SceneResourceFormat::LEGACY;
      REQUIRE(options.registry.add<Label>(name, [&](ui::SceneResourceArguments arguments) {
        ++factories;

        return ui::label(arguments.text);
      }));
      const auto body = std::string(name) + "#caller{text=\"caller\"}";
      auto owner = ui::scene(configuration ? "scene{" + body + "}" : body, options);
      auto caller = owner.get<Label>("caller");

      REQUIRE(owner.prepareChecked());
      CHECK(*caller.text().get() == "caller");
      CHECK(factories == 1);
      REQUIRE(owner.close());
    }

    ui::SceneResourceOptions declared;
    declared.format = ui::SceneResourceFormat::CONFIGURATION;
    auto owner = ui::scene(
      std::string("definitions{") + name + "=Label{}} scene{" + name + "#declared{text=\"named\"}}", declared);
    auto caller = owner.get<Label>("declared");

    REQUIRE(owner.prepareChecked());
    CHECK(*caller.text().get() == "named");
    REQUIRE(owner.close());
    CHECK(rejected(std::string(name) + "{}").sceneResource->code == Code::UNKNOWN_TYPE);
  }

  int factories{}, resolutions{};
  ui::SceneResourceOptions custom;
  custom.format = ui::SceneResourceFormat::CONFIGURATION;
  custom.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
    ++resolutions;

    return "early";
  };
  REQUIRE(custom.registry.add<Button>("CustomButton", [&](ui::SceneResourceArguments arguments) {
    ++factories;

    return ui::button(arguments.text);
  }));
  const auto failure = rejected("scene{VBox{Label{text=i18n(\"early\")} CustomButton{chrome=NONE}}}", custom);

  CHECK(failure.sceneResource->code == Code::UNKNOWN_PROPERTY);
  REQUIRE(failure.sceneResource->propertyPath);
  CHECK(*failure.sceneResource->propertyPath == "CustomButton.chrome");
  CHECK(factories == 0);
  CHECK(resolutions == 0);
  CHECK(rejected("Button{chrome=NONE}").sceneResource->code == Code::UNKNOWN_PROPERTY);
}

TEST_CASE("scroll resource scalar cross-field and content failures precede reachable callbacks") {
  struct Scenario {
    std::string_view child;
    Code code;
    std::string_view path;
  };

  const Scenario scenarios[]{
    {"ScrollBar{wheelStep=0}", Code::INVALID_VALUE, "ScrollBar.wheelStep"},
    {"ScrollBar{wheelStep=1e999}", Code::INVALID_VALUE, "ScrollBar.wheelStep"},
    {"ScrollBar{keyStep=0}", Code::INVALID_VALUE, "ScrollBar.keyStep"},
    {"ScrollBar{keyStep=18446744073709551616}", Code::INVALID_VALUE, "ScrollBar.keyStep"},
    {"ScrollBar{minimumThumb=0}", Code::INVALID_VALUE, "ScrollBar.minimumThumb"},
    {"ScrollBar{minimumThumb=2147483648}", Code::INVALID_VALUE, "ScrollBar.minimumThumb"},
    {"ScrollBar{pageOverlap=-1}", Code::INVALID_VALUE, "ScrollBar.pageOverlap"},
    {"ScrollBar{axis=DIAGONAL}", Code::INVALID_VALUE, "ScrollBar.axis"},
    {"ScrollArea{horizontal=SOMETIMES Label{}}", Code::INVALID_VALUE, "ScrollArea.horizontal"},
    {"ScrollArea{placement=ON_FRAME Label{}}", Code::INVALID_VALUE, "ScrollArea.placement"},
    {"ScrollArea{frame{glyphs=ASCII} Label{}}", Code::INVALID_VALUE, "ScrollArea.frame.glyphs"},
    {"ScrollArea{frame{enabled=false maxPaintCells=8} Label{}}", Code::INVALID_VALUE, "ScrollArea.frame.maxPaintCells"},
    {"ScrollArea{}", Code::INVALID_VALUE, "ScrollArea.children"},
    {"ScrollArea{Label{} Label{}}", Code::INVALID_VALUE, "ScrollArea.children"},
    {"ScrollBar{Label{}}", Code::CHILDREN_FORBIDDEN, ""}};
  for (const auto &scenario : scenarios) {
    INFO(scenario.child);
    int factories{}, resolutions{};
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;
      return "early";
    };
    REQUIRE(options.registry.add<Custom>("Probe", [&](ui::SceneResourceArguments) {
      ++factories;
      return ui::custom<Custom>("probe");
    }));
    const auto failure = rejected(
      std::string("scene{VBox{Probe{} Label{text=i18n(\"early\")} ") + std::string(scenario.child) + "}}", options);
    CHECK(failure.sceneResource->code == scenario.code);
    if (!scenario.path.empty()) {
      REQUIRE(failure.sceneResource->propertyPath);
      CHECK(*failure.sceneResource->propertyPath == scenario.path);
    }
    CHECK(factories == 0);
    CHECK(resolutions == 0);
    CHECK(Custom::alive == 0);
  }
}

TEST_CASE("scroll frame tail leaves consume document budgets and retain inherited failure origins") {
  for (const std::size_t budget : {3U, 4U}) {
    int resolutions{};
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.controls.emplace(ui::ControlContextLimits{.maxMaterializedLeaves = budget});
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;
      return "early";
    };
    auto owner = ui::scene("scene{VBox{Label{text=i18n(\"early\")} ScrollArea{"
                           "frame{enabled=true glyphs=ASCII maxPaintCells=32} Label{}}}}",
                           options);
    const auto prepared = owner.prepareChecked();
    if (budget == 3) {
      REQUIRE_FALSE(prepared);
      REQUIRE(prepared.error().sceneResource);
      CHECK(prepared.error().sceneResource->code == Code::MATERIALIZATION_LIMIT);
      CHECK(resolutions == 0);
    } else {
      REQUIRE(prepared);
      CHECK(resolutions == 1);
    }
    REQUIRE(owner.close());
  }
  TemporaryFile file;
  const std::string source = "definitions{Viewport=ScrollArea{placement=ON_FRAME}}\n"
                             "scene{VBox{Label{text=i18n(\"early\")} Viewport{Label{}}}}";
  file.write(source);
  int resolutions{};
  ui::SceneResourceOptions options;
  options.format = ui::SceneResourceFormat::CONFIGURATION;
  options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
    ++resolutions;
    return "early";
  };
  ManagedError failure;
  {
    auto owner = ui::scene(ui::SceneResourceFile{file.path}, options);
    const auto prepared = owner.prepareChecked();
    REQUIRE_FALSE(prepared);
    failure = prepared.error();
  }
  REQUIRE(failure.sceneResource);
  REQUIRE(failure.sceneResource->propertyPath);
  REQUIRE(failure.sceneResource->source);
  CHECK(failure.sceneResource->code == Code::INVALID_VALUE);
  CHECK(*failure.sceneResource->propertyPath == "Viewport.placement");
  CHECK(*failure.sceneResource->source == file.path.generic_string());
  CHECK(failure.sceneResource->offset == source.find("placement"));
  CHECK(resolutions == 0);
}

TEST_CASE("scroll fallback names preserve caller definitions custom allowlists and legacy grammar") {
  for (const auto name : {"ScrollArea", "ScrollBar"}) {
    for (const bool configuration : {false, true}) {
      int calls{};
      ui::SceneResourceOptions options;
      options.format = configuration ? ui::SceneResourceFormat::CONFIGURATION : ui::SceneResourceFormat::LEGACY;
      REQUIRE(options.registry.add<Label>(name, [&](ui::SceneResourceArguments arguments) {
        ++calls;
        return ui::label(arguments.text);
      }));
      const auto body = std::string(name) + "#caller{text=\"caller\"}";
      auto owner = ui::scene(configuration ? "scene{" + body + "}" : body, options);
      auto caller = owner.get<Label>("caller");
      REQUIRE(owner.prepareChecked());
      CHECK(*caller.text().get() == "caller");
      CHECK(calls == 1);
      REQUIRE(owner.close());
    }
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    auto owner = ui::scene(
      std::string("definitions{") + name + "=Label{}} scene{" + name + "#caller{text=\"declared\"}}", options);
    auto caller = owner.get<Label>("caller");
    REQUIRE(owner.prepareChecked());
    CHECK(*caller.text().get() == "declared");
    REQUIRE(owner.close());
    CHECK(rejected(std::string(name) + "{}").sceneResource->code == Code::UNKNOWN_TYPE);
  }
  int factories{}, resolutions{};
  ui::SceneResourceOptions options;
  options.format = ui::SceneResourceFormat::CONFIGURATION;
  options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
    ++resolutions;
    return "early";
  };
  REQUIRE(options.registry.add<ScrollBar>("CustomBar", [&](ui::SceneResourceArguments) {
    ++factories;
    return ui::scrollBar();
  }));
  const auto failure = rejected("scene{VBox{Label{text=i18n(\"early\")} CustomBar{keyStep=2}}}", options);
  CHECK(failure.sceneResource->code == Code::UNKNOWN_PROPERTY);
  CHECK(factories == 0);
  CHECK(resolutions == 0);
}

TEST_CASE("scroll compact staging preserves untouched provenance and frame depth remains bounded") {
  auto source = std::make_shared<ui::ControlSourceOrigin>();
  source->sourceIdentity = "scroll-origin.scene";
  source->byteOffset = 27;
  source->line = 3;
  source->column = 5;
  for (const bool replace : {false, true}) {
    auto recipe = replace ? ui::scrollBar().settings({.origin = source, .wheelStep = 2}).wheelStep(0)
                          : ui::scrollBar().settings({.origin = source, .wheelStep = 0}).axis(ScrollAxis::HORIZONTAL);
    auto owner = ui::scene(std::move(recipe));
    const auto prepared = owner.prepareChecked();
    REQUIRE_FALSE(prepared);
    REQUIRE(prepared.error().sceneResource);
    const auto &failure = *prepared.error().sceneResource;
    REQUIRE(failure.propertyPath);
    CHECK(*failure.propertyPath == "ScrollBar.wheelStep");
    if (replace) {
      CHECK_FALSE(failure.source);
      CHECK(failure.line == 0);
    } else {
      REQUIRE(failure.source);
      CHECK(*failure.source == "scroll-origin.scene");
      CHECK(failure.offset == 27);
      CHECK(failure.line == 3);
      CHECK(failure.column == 5);
    }
    REQUIRE(owner.close());
  }
  int resolutions{};
  ui::SceneResourceOptions options;
  options.format = ui::SceneResourceFormat::CONFIGURATION;
  options.controls.emplace(ui::ControlContextLimits{.maxStructDepth = 1});
  options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
    ++resolutions;
    return "early";
  };
  const auto failure =
    rejected("scene{VBox{Label{text=i18n(\"early\")} ScrollArea{frame{enabled=true} Label{}}}}", options);
  CHECK(failure.sceneResource->code == Code::DEPTH_LIMIT);
  CHECK(resolutions == 0);
}

TEST_CASE("registered frame children retain their identity under unrelated resource parents") {
  for (const bool configured : {false, true}) {
    for (const bool empty : {false, true}) {
      INFO("configuration: ", configured, "; empty child: ", empty);
      int factories{};
      ui::SceneResourceOptions options;
      options.format = configured ? ui::SceneResourceFormat::CONFIGURATION : ui::SceneResourceFormat::LEGACY;
      REQUIRE(options.registry.add<Label>("frame", [&](ui::SceneResourceArguments arguments) {
        ++factories;
        return ui::label(arguments.text).id("child");
      }));
      const std::string body = empty ? "VBox#parent{frame{}}" : "VBox#parent{frame{text=\"content\"}}";
      auto owner = ui::scene(configured ? "scene{" + body + "}" : body, options);
      auto parent = owner.get<Column>("parent");
      auto child = owner.get<Label>("child");
      owner.configure([&](ManagedScene &, Application &app) {
        const auto info = app.tree().inspect(parent.node());
        REQUIRE(info);
        REQUIRE(info->children.size() == 1);
        CHECK(info->children.front().id() == child.node().id());
      });
      REQUIRE(owner.prepareChecked());
      REQUIRE(child.text().get());
      CHECK(*child.text().get() == (empty ? "" : "content"));
      CHECK(factories == 1);
      REQUIRE(owner.close());
    }
  }
}

TEST_CASE("native frame groups remain explicit beside caller registered frame children") {
  for (const bool derived : {false, true}) {
    int factories{};
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    REQUIRE(options.registry.add<Label>("frame", [&](ui::SceneResourceArguments arguments) {
      ++factories;
      return ui::label(arguments.text).id("content");
    }));
    const std::string source =
      derived ? "definitions{Viewport=ScrollArea{frame{enabled=true}}} "
                "scene{Viewport#area{placement=ON_FRAME horizontal=ALWAYS vertical=NEVER frame{text=\"child\"}}}"
              : "scene{ScrollArea#area{placement=ON_FRAME horizontal=ALWAYS vertical=NEVER "
                "frame={enabled=true} frame{text=\"child\"}}}";
    auto owner = ui::scene(source, options);
    auto area = owner.get<ScrollArea>("area");
    auto content = owner.get<Label>("content");
    owner.configure([&](ManagedScene &, Application &app) {
      const auto info = app.tree().inspect(area.node());
      REQUIRE(info);
      REQUIRE(info->children.size() == 1);
      CHECK(info->children.front().id() == content.node().id());
    });
    REQUIRE(owner.prepareChecked());
    CHECK(*content.text().get() == "child");
    CHECK(factories == 1);
    REQUIRE(owner.close());
  }
  int factories{}, resolutions{};
  ui::SceneResourceOptions options;
  options.format = ui::SceneResourceFormat::CONFIGURATION;
  REQUIRE(options.registry.add<Label>("frame", [&](ui::SceneResourceArguments) {
    ++factories;
    return ui::label("child");
  }));
  options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
    ++resolutions;
    return "early";
  };
  const auto failure = rejected("scene{VBox{Label{text=i18n(\"early\")} "
                                "ScrollArea{frame={enabled=false glyphs=ASCII} frame{}}}}",
                                options);
  CHECK(failure.sceneResource->code == Code::INVALID_VALUE);
  REQUIRE(failure.sceneResource->propertyPath);
  CHECK(*failure.sceneResource->propertyPath == "ScrollArea.frame.glyphs");
  CHECK(factories == 0);
  CHECK(resolutions == 0);
}

TEST_CASE("ordinary Overlay invalid leaves and tail budgets fail before reachable resolvers and factories") {
  for (const auto child : {"Overlay{horizontalAlign=SIDEWAYS}", "Overlay{verticalAlign=SIDEWAYS}",
                           "Overlay{padding{left=-1}}", "Overlay{padding{bottom=2147483648}}"}) {
    int factories{}, resolutions{};
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;
      return "early";
    };
    REQUIRE(options.registry.add<Custom>("Probe", [&](ui::SceneResourceArguments) {
      ++factories;
      return ui::custom<Custom>("probe");
    }));
    const auto failure =
      rejected(std::string("scene{VBox{Probe{} Label{text=i18n(\"early\")} ") + child + "}}", options);
    CHECK(failure.sceneResource->code == Code::INVALID_VALUE);
    CHECK(factories == 0);
    CHECK(resolutions == 0);
  }
  for (const std::size_t budget : {2U, 3U}) {
    int factories{}, resolutions{};
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.controls.emplace(ui::ControlContextLimits{.maxMaterializedLeaves = budget});
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;
      return "early";
    };
    REQUIRE(options.registry.add<Custom>("Probe", [&](ui::SceneResourceArguments) {
      ++factories;
      return ui::custom<Custom>("probe");
    }));
    auto owner = ui::scene("scene{VBox{Probe{} Label{text=i18n(\"early\")} "
                           "Overlay{horizontalAlign=START verticalAlign=STRETCH}}}",
                           options);
    const auto prepared = owner.prepareChecked();
    if (budget == 2) {
      REQUIRE_FALSE(prepared);
      REQUIRE(prepared.error().sceneResource);
      CHECK(prepared.error().sceneResource->code == Code::MATERIALIZATION_LIMIT);
      CHECK(factories == 0);
      CHECK(resolutions == 0);
    } else {
      REQUIRE(prepared);
      CHECK(factories == 1);
      CHECK(resolutions == 1);
    }
    REQUIRE(owner.close());
    CHECK(Custom::alive == 0);
  }
}

TEST_CASE("ordinary Overlay names and custom allowlists retain caller priority and legacy grammar") {
  for (const bool configured : {false, true}) {
    int factories{};
    ui::SceneResourceOptions options;
    options.format = configured ? ui::SceneResourceFormat::CONFIGURATION : ui::SceneResourceFormat::LEGACY;
    REQUIRE(options.registry.add<Label>("Overlay", [&](ui::SceneResourceArguments arguments) {
      ++factories;
      return ui::label(arguments.text);
    }));
    const std::string body = "Overlay#caller{text=\"caller\"}";
    auto owner = ui::scene(configured ? "scene{" + body + "}" : body, options);
    auto caller = owner.get<Label>("caller");
    REQUIRE(owner.prepareChecked());
    CHECK(*caller.text().get() == "caller");
    CHECK(factories == 1);
    REQUIRE(owner.close());
  }
  ui::SceneResourceOptions named;
  named.format = ui::SceneResourceFormat::CONFIGURATION;
  auto owner = ui::scene("definitions{Overlay=Label{}} scene{Overlay#caller{text=\"declared\"}}", named);
  auto caller = owner.get<Label>("caller");
  REQUIRE(owner.prepareChecked());
  CHECK(*caller.text().get() == "declared");
  REQUIRE(owner.close());
  CHECK(rejected("Overlay{}").sceneResource->code == Code::UNKNOWN_TYPE);
  int factories{}, resolutions{};
  ui::SceneResourceOptions custom;
  custom.format = ui::SceneResourceFormat::CONFIGURATION;
  REQUIRE(custom.registry.add<Overlay>(
    "CustomOverlay",
    [&](ui::SceneResourceArguments) {
      ++factories;
      return ui::overlay();
    },
    true));
  auto accepted = ui::scene("scene{CustomOverlay#custom{width=7}}", custom);
  auto native = accepted.get<Overlay>("custom");
  REQUIRE(accepted.prepareChecked());
  CHECK(native.node() != Node<Overlay>{});
  CHECK(factories == 1);
  REQUIRE(accepted.close());
  factories = 0;
  custom.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
    ++resolutions;
    return "early";
  };
  const auto failure = rejected("scene{VBox{Label{text=i18n(\"early\")} CustomOverlay{horizontalAlign=END}}}", custom);
  CHECK(failure.sceneResource->code == Code::UNKNOWN_PROPERTY);
  CHECK(factories == 0);
  CHECK(resolutions == 0);
}

TEST_CASE("ordinary Overlay padding groups do not swallow caller-owned padding children") {
  for (const bool declared : {false, true}) {
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    if (!declared) {
      REQUIRE(options.registry.add<Label>("padding", [](ui::SceneResourceArguments arguments) {
        return ui::label(arguments.text);
      }));
    }
    const auto prefix = declared ? "definitions{padding=Label{}} " : "";
    auto owner =
      ui::scene(std::string(prefix) + "scene{Overlay#parent{padding={left=1} padding{text=\"child\"}}}", options);
    auto parent = owner.get<Overlay>("parent");
    owner.configure([&](ManagedScene &, Application &app) {
      const auto children = app.tree().inspect(parent.node());
      REQUIRE(children);
      REQUIRE(children->children.size() == 1);
      const auto measured = app.tree().measure(children->children.front(), {});
      REQUIRE(measured);
      CHECK(*measured == CellSize{5, 1});
      REQUIRE(app.tree().read(parent.node(), [](const Overlay &widget) {
        CHECK(widget.options().padding.left == 1);
      }));
    });
    REQUIRE(owner.prepareChecked());
    REQUIRE(owner.close());
  }
}

TEST_CASE("ordinary Overlay compact provenance preserves untouched inset origins and clears changed alignment") {
  auto origin = std::make_shared<ui::ControlSourceOrigin>();
  origin->sourceIdentity = "overlay-origin.scene";
  origin->byteOffset = 23;
  origin->line = 2;
  for (const bool replace : {false, true}) {
    auto recipe =
      replace
        ? ui::overlay()
            .settings({.origin = origin, .horizontalAlign = LayoutAlign::START})
            .horizontalAlign(static_cast<LayoutAlign>(9))
        : ui::overlay().settings({.origin = origin, .grid = {.paddingLeft = -1}}).horizontalAlign(LayoutAlign::END);
    auto owner = ui::scene(std::move(recipe));
    const auto prepared = owner.prepareChecked();
    REQUIRE_FALSE(prepared);
    REQUIRE(prepared.error().sceneResource);
    const auto &failure = *prepared.error().sceneResource;
    REQUIRE(failure.propertyPath);
    CHECK(*failure.propertyPath == (replace ? "Overlay.horizontalAlign" : "Overlay.padding.left"));
    if (replace) {
      CHECK_FALSE(failure.source);
    } else {
      REQUIRE(failure.source);
      CHECK(*failure.source == "overlay-origin.scene");
      CHECK(failure.offset == 23);
      CHECK(failure.line == 2);
    }
    REQUIRE(owner.close());
  }
  TemporaryFile file;
  const std::string source = "definitions{Layer=Overlay{verticalAlign=INVALID}} scene{Layer{}}";
  file.write(source);
  ui::SceneResourceOptions options;
  options.format = ui::SceneResourceFormat::CONFIGURATION;
  ManagedError failure;
  {
    auto owner = ui::scene(ui::SceneResourceFile{file.path}, options);
    const auto prepared = owner.prepareChecked();
    REQUIRE_FALSE(prepared);
    failure = prepared.error();
  }
  REQUIRE(failure.sceneResource);
  REQUIRE(failure.sceneResource->source);
  REQUIRE(failure.sceneResource->propertyPath);
  CHECK(*failure.sceneResource->source == file.path.generic_string());
  CHECK(*failure.sceneResource->propertyPath == "Layer.verticalAlign");
  CHECK(failure.sceneResource->offset == source.find("verticalAlign"));
}

TEST_CASE("configured placeholder aliases match canonical definitions defaults and instances") {
  const auto prepare = [](std::string_view source) {
    std::vector<std::string> resolvedKeys;
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.resolver = [&](std::string_view key) -> std::expected<std::string, TextResourceError> {
      resolvedKeys.emplace_back(key);

      return std::string(key);
    };

    auto owner = ui::scene(source, options);
    const auto result = owner.prepareChecked();

    CHECK(result.has_value());
    if (result) {
      CHECK(owner.close().has_value());
    }

    std::ranges::sort(resolvedKeys);
    return resolvedKeys;
  };

  constexpr std::string_view ALIASES =
    "definitions { Named=LineEdit{placeHolder=i18n(\"definition\")} } "
    "defaults { LineEdit{placeHolder=i18n(\"default\")} } "
    "scene { VBox{ Named#defined{} LineEdit#defaulted{} LineEdit#instance{placeHolder=i18n(\"instance\")} } }";
  constexpr std::string_view CANONICAL =
    "definitions { Named=LineEdit{placeholder=i18n(\"definition\")} } "
    "defaults { LineEdit{placeholder=i18n(\"default\")} } "
    "scene { VBox{ Named#defined{} LineEdit#defaulted{} LineEdit#instance{placeholder=i18n(\"instance\")} } }";

  const auto aliasKeys = prepare(ALIASES);
  const auto canonicalKeys = prepare(CANONICAL);
  CHECK(aliasKeys == canonicalKeys);
  CHECK(aliasKeys == std::vector<std::string>{"default", "definition", "instance"});
}

TEST_CASE("configured placeholder alias duplicates fail at the second spelling before callbacks") {
  struct Scenario {
    std::string_view source;
    std::string_view second;
    std::string_view path;
  };

  constexpr Scenario SCENARIOS[]{
    {"definitions { A=LineEdit{placeholder=i18n(\"first\") placeHolder=?} } scene { Probe{} }", "placeHolder",
     "A.placeholder"},
    {"definitions { A=LineEdit{placeHolder=i18n(\"first\") placeholder=?} } scene { Probe{} }", "placeholder",
     "A.placeholder"},
    {"defaults { LineEdit{placeholder=i18n(\"first\") placeHolder=?} } scene { Probe{} }", "placeHolder",
     "LineEdit.placeholder"},
    {"defaults { LineEdit{placeHolder=i18n(\"first\") placeholder=?} } scene { Probe{} }", "placeholder",
     "LineEdit.placeholder"},
    {"scene { VBox{ Probe{} LineEdit{placeholder=i18n(\"first\") placeHolder=?} } }", "placeHolder",
     "LineEdit.placeholder"},
    {"scene { VBox{ Probe{} LineEdit{placeHolder=i18n(\"first\") placeholder=?} } }", "placeholder",
     "LineEdit.placeholder"},
  };

  for (const auto &scenario : SCENARIOS) {
    int factories{}, resolutions{};
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;

      return "resolved";
    };
    REQUIRE(options.registry.add<Custom>("Probe", [&](ui::SceneResourceArguments) {
      ++factories;

      return ui::custom<Custom>("probe");
    }));

    const auto failure = rejected(scenario.source, options);
    CHECK(failure.sceneResource->code == Code::DUPLICATE_PROPERTY);
    REQUIRE(failure.sceneResource->propertyPath);
    CHECK(*failure.sceneResource->propertyPath == scenario.path);
    CHECK(failure.sceneResource->offset == scenario.source.find(scenario.second));
    CHECK(factories == 0);
    CHECK(resolutions == 0);
    CHECK(Custom::alive == 0);
  }
}

TEST_CASE("configured property aliases remain case sensitive") {
  const auto failure =
    rejected("scene { LineEdit{PlaceHolder=\"value\"} }", {.format = ui::SceneResourceFormat::CONFIGURATION});
  CHECK(failure.sceneResource->code == Code::UNKNOWN_PROPERTY);
  REQUIRE(failure.sceneResource->propertyPath);
  CHECK(*failure.sceneResource->propertyPath == "LineEdit.PlaceHolder");
}

TEST_CASE("configured resource budgets cover declarations structure and all effective node leaves") {
  ui::SceneResourceOptions options;

  options.format = ui::SceneResourceFormat::CONFIGURATION;
  options.controls.emplace(ui::ControlContextLimits{.maxDefinitions = 1});
  CHECK(rejected("definitions { A=LineEdit{} B=LineEdit{} } scene { Label{} }", options).sceneResource->code ==
        Code::TYPE_LIMIT);
  options.controls.emplace(ui::ControlContextLimits{.maxInheritance = 1});
  CHECK(rejected("definitions { A=B{} B=LineEdit{} } scene { Label{} }", options).sceneResource->code ==
        Code::DEPTH_LIMIT);
  options.controls.emplace(ui::ControlContextLimits{.maxStructDepth = 1});
  CHECK(rejected("scene { LineEdit { history { maxOperations=1 } } }", options).sceneResource->code ==
        Code::DEPTH_LIMIT);
  options.controls.emplace(ui::ControlContextLimits{.maxMaterializedLeaves = 3});

  int factories{}, resolutions{};

  REQUIRE(options.registry.add<Custom>("Probe", [&](ui::SceneResourceArguments) {
    ++factories;

    return ui::custom<Custom>("probe");
  }));
  options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
    ++resolutions;

    return "value";
  };

  const auto failure = rejected(
    "defaults { Label { text=i18n(\"caption\"), width=1 } } scene { VBox { Probe{} Label{} Label{} } }", options);

  CHECK(failure.sceneResource->code == Code::MATERIALIZATION_LIMIT);
  CHECK(factories == 0);
  CHECK(resolutions == 0);
}

TEST_CASE("legacy resource names and large custom registries remain independent of configuration syntax") {
  ui::SceneResourceOptions options;
  int calls{};

  for (const auto name : {"definitions", "defaults", "scene"}) {
    REQUIRE(options.registry.add<Custom>(name, [&](ui::SceneResourceArguments arguments) {
      ++calls;

      return ui::custom<Custom>(arguments.text);
    }));
  }

  for (int index = 0; index < 70; ++index) {
    REQUIRE(options.registry.add<Custom>("Custom" + std::to_string(index), [](ui::SceneResourceArguments) {
      return ui::custom<Custom>("custom");
    }));
  }

  for (const auto name : {"definitions", "defaults", "scene"}) {
    auto owner = ui::scene(std::string(name) + "{\"ordinary\"}", options);

    REQUIRE(owner.prepareChecked());
    REQUIRE(owner.close());
  }

  CHECK(calls == 3);
  CHECK(Custom::alive == 0);
}

TEST_CASE("configured text and placeholder failures retain inherited file origin after destruction") {
  for (const auto property : {"text", "placeholder"}) {
    TemporaryFile file;
    const auto source =
      std::string("definitions {\n  MyEdit = LineEdit { ") + property + "=i18n(\"absent\") }\n}\nscene { MyEdit{} }";

    file.write(source);

    ManagedError failure;

    {
      ui::SceneResourceOptions options;

      options.format = ui::SceneResourceFormat::CONFIGURATION;

      auto owner = ui::scene(ui::SceneResourceFile{file.path}, options);
      const auto prepared = owner.prepareChecked();

      REQUIRE_FALSE(prepared);
      failure = prepared.error();
    }

    REQUIRE(failure.resource);
    CHECK(*failure.resource == TextResourceError::RESOLVER_REQUIRED);
    REQUIRE(failure.sceneResource);
    CHECK(failure.sceneResource->code == Code::TEXT);
    REQUIRE(failure.sceneResource->propertyPath);
    CHECK(*failure.sceneResource->propertyPath == std::string("MyEdit.") + property);
    REQUIRE(failure.sceneResource->source);
    CHECK(*failure.sceneResource->source == file.path.generic_string());
    CHECK(failure.sceneResource->line == 2);
    CHECK(failure.sceneResource->offset == source.find(std::string(property) + "="));
  }
}

TEST_CASE("configured custom aliases reuse owning factories and checked rollback") {
  ui::SceneResourceOptions options;
  ui::Ref<Custom> reference;
  int calls{};

  options.format = ui::SceneResourceFormat::CONFIGURATION;
  REQUIRE(options.registry.add<Custom>("Probe", [&, reference](ui::SceneResourceArguments arguments) {
    ++calls;
    CHECK(arguments.text == "inherited");
    CHECK(arguments.width == 7);

    return ui::custom<Custom>(reference, arguments.text);
  }));

  auto owner = ui::scene("definitions { Alias=Probe{text=\"inherited\", width=7} } scene { Alias#alias{} }", options);

  REQUIRE(owner.prepareChecked());
  CHECK(calls == 1);
  CHECK(reference.node() != Node<Custom>{});
  REQUIRE(owner.close());
  CHECK(Custom::alive == 0);

  ui::Ref<Custom> unpublished;

  REQUIRE(options.registry.add<Custom>("Fail", [unpublished](ui::SceneResourceArguments) {
    return ui::custom<Custom>(unpublished, "failure")
      .setup([](ManagedScene &, Node<Custom>) -> std::expected<void, ManagedError> {
        return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::WOULD_BLOCK});
      });
  }));

  auto failed = ui::scene("scene { VBox { Fail{} Label{} } }", options);
  const auto result = failed.prepareChecked();

  REQUIRE_FALSE(result);
  CHECK(cause(result.error(), TreeError::WOULD_BLOCK));
  CHECK(unpublished.node() == Node<Custom>{});
  CHECK(Custom::alive == 0);
}

TEST_CASE("configured ThreeState retains fixed cycle and base derived id selectors") {
  ui::SceneResourceOptions options;

  options.format = ui::SceneResourceFormat::CONFIGURATION;

  auto owner =
    ui::scene("definitions { Flag=ThreeState{} } scene { Flag#choice { text=\"flag\", state=MIXED } }", options) |
    ui::style("CheckBox { bold:true; foreground:#000001; } Flag { italic:true; foreground:#000002; } "
              "ThreeState { underline:true; } #choice { foreground:#000003; }");
  auto choice = owner.get<ThreeState>("choice");
  auto base = owner.get<CheckBox>("choice");
  Application *application{};
  ManagedScene *managed{};

  owner.configure([&](ManagedScene &scene, Application &app) {
    managed = &scene;
    application = &app;
  });

  REQUIRE(owner.prepareChecked());
  REQUIRE(application);
  REQUIRE(managed);
  CHECK(Node<CheckBox>{choice.node()} == base.node());
  REQUIRE(managed->prepare(*application, {40, 8}));
  REQUIRE(application->tree().read(choice.node(), [](const ThreeState &widget) {
    CHECK(widget.checkState() == CheckState::MIXED);
    REQUIRE(widget.computedStyle());

    const auto &style = widget.computedStyle()->normal;

    CHECK(style.bold);
    CHECK(style.italic);
    CHECK(style.underline);
    CHECK(style.foreground == RgbColor{0, 0, 3});
  }));

  for (const auto expected : {CheckState::UNCHECKED, CheckState::CHECKED, CheckState::MIXED}) {
    REQUIRE(application->router().invoke(base.node(), {Action::ACTIVATE}));
    REQUIRE(application->tree().read(choice.node(), [&](const ThreeState &widget) {
      CHECK(widget.checkState() == expected);
    }));
  }

  REQUIRE(owner.close());
}

TEST_CASE("configured aliases preserve custom factory classes and id style metadata") {
  ui::SceneResourceOptions options;

  options.format = ui::SceneResourceFormat::CONFIGURATION;
  REQUIRE(options.registry.add<Button>("CustomButton", [](ui::SceneResourceArguments arguments) {
    return ui::button(arguments.text).style(StyleTarget{.type = "Button", .classes = {"accent"}, .control = true});
  }));

  auto owner = ui::scene("definitions { Alias=CustomButton{} } scene { Alias#named{} }", options) |
               ui::style("Button.accent { bold:true; } Alias { italic:true; } #named { underline:true; }");
  auto button = owner.get<Button>("named");
  Application *application{};
  ManagedScene *managed{};

  owner.configure([&](ManagedScene &scene, Application &app) {
    managed = &scene;
    application = &app;
  });

  REQUIRE(owner.prepareChecked());
  REQUIRE(managed->prepare(*application, {40, 8}));
  REQUIRE(application->tree().read(button.node(), [](const Button &widget) {
    REQUIRE(widget.computedStyle());
    CHECK(widget.computedStyle()->normal.bold);
    CHECK(widget.computedStyle()->normal.italic);
    CHECK(widget.computedStyle()->normal.underline);
  }));
  REQUIRE(owner.close());
}

TEST_CASE("configured resource text resolves before factories and rejects multiline placeholders") {
  ui::SceneResourceOptions options;
  int resolved{}, factories{};

  options.format = ui::SceneResourceFormat::CONFIGURATION;
  options.resolver = [&](std::string_view key) -> std::expected<std::string, TextResourceError> {
    ++resolved;

    return std::string(key);
  };
  REQUIRE(options.registry.add<Custom>("Probe", [&](ui::SceneResourceArguments) {
    ++factories;
    CHECK(resolved == 2);

    return ui::custom<Custom>("probe");
  }));

  auto owner = ui::scene(
    "defaults { LineEdit { text=i18n(\"value\"), placeholder=i18n(\"hint\") } } scene { VBox{ Probe{} LineEdit{} } }",
    options);

  REQUIRE(owner.prepareChecked());
  CHECK(resolved == 2);
  CHECK(factories == 1);
  REQUIRE(owner.close());

  const auto failure = rejected("scene { VBox{ Probe{} LineEdit{ placeholder=\"line\\nseparator\" } } }", options);

  CHECK(failure.sceneResource->code == Code::TEXT);
  REQUIRE(failure.sceneResource->propertyPath);
  CHECK(*failure.sceneResource->propertyPath == "LineEdit.placeholder");
  CHECK(factories == 1);
}

TEST_CASE("resource contexts inherit immutable app definitions and keep local defaults in their subtree") {
  ui::ControlSettings definition;

  definition.history.maxOperations = 1;

  ui::SceneOptions sceneOptions;

  sceneOptions.controls.define("AppEdit", "LineEdit").defaults("AppEdit", definition);

  ui::SceneResourceOptions options;

  options.format = ui::SceneResourceFormat::CONFIGURATION;

  auto owner = ui::scene(
    ui::column(ui::resource("defaults { AppEdit { history { maxOperations=2 } } } scene { AppEdit#local{} }", options),
               ui::resource("scene { AppEdit#inherited{} }", options)),
    sceneOptions);
  auto local = owner.get<LineEdit>("local");
  auto inherited = owner.get<LineEdit>("inherited");

  REQUIRE(owner.prepareChecked());

  for (const auto value : {"a", "b"}) {
    REQUIRE((local.text() = value).result());
    REQUIRE((inherited.text() = value).result());
  }

  REQUIRE(local.undo().result());
  REQUIRE(local.undo().result());
  CHECK(*local.text().get() == "");
  REQUIRE(inherited.undo().result());
  REQUIRE(inherited.undo().result());
  CHECK(*inherited.text().get() == "a");
  REQUIRE(owner.close());

  options.controls.emplace();
  const auto failure = [&] {
    auto isolated = ui::scene("scene { AppEdit{} }", options, sceneOptions);
    const auto prepared = isolated.prepareChecked();

    REQUIRE_FALSE(prepared);

    return prepared.error();
  }();

  REQUIRE(failure.sceneResource);
  CHECK(failure.sceneResource->code == Code::UNKNOWN_TYPE);
}

TEST_CASE("custom resource schemas must exactly match the explicit factory allowlist") {
  ui::SceneResourceOptions options;

  options.format = ui::SceneResourceFormat::CONFIGURATION;
  options.controls.emplace();

  const std::string_view partial[]{"text"};

  options.controls->registerType("Probe", partial);
  REQUIRE(options.registry.add<Custom>("Probe", [](ui::SceneResourceArguments arguments) {
    return ui::custom<Custom>(arguments.text);
  }));

  CHECK(rejected("scene { Probe{} }", options).sceneResource->code == Code::DUPLICATE_TYPE);

  const std::string_view exact[]{"text", "width", "height", "flex"};

  options.controls.emplace();
  options.controls->registerType("Probe", exact);

  auto owner = ui::scene("scene { Probe{} }", options);

  REQUIRE(owner.prepareChecked());
  REQUIRE(owner.close());
  CHECK(Custom::alive == 0);
}

TEST_CASE("resource properties use existing controls layout and PSS through named refs") {
  auto owner = ui::scene(R"(VBox#root{ gap = 1; width: 32
    Label#caption{ text = "Hello" }
    HBox{ Button#add{"Add" flex=2} CheckBox#choice{"Yes" checked=true} }
    LineEdit#message{text="initial" height=1}
  })") | ui::style("Label { bold: true; }");
  auto label = owner.get<Label>("caption");
  auto message = owner.get<LineEdit>("message");
  auto choice = owner.get<CheckBox>("choice");
  bool configured{};

  owner.configure([&](ManagedScene &managed, Application &application) {
    configured = true;
    REQUIRE(label.text().get());
    CHECK(*label.text().get() == "Hello");
    REQUIRE(message.text().get());
    CHECK(*message.text().get() == "initial");
    REQUIRE(application.tree().read(choice.node(), [](const CheckBox &widget) {
      CHECK(widget.checked());
    }));
    CHECK(managed.root() != Node<Widget>{});
  });

  REQUIRE(owner.prepareChecked());
  CHECK(configured);
  REQUIRE(owner.close());
}

TEST_CASE("complete schema and duplicate validation precede custom factories") {
  struct Scenario {
    std::string_view source;
    Code expected;
  };

  const Scenario scenarios[]{
    {"VBox{ Probe{} Missing{} }", Code::UNKNOWN_TYPE},
    {"VBox{ Probe{} Button{unknown=1} }", Code::UNKNOWN_PROPERTY},
    {"VBox{ Probe#same{} Label#same{} }", Code::DUPLICATE_ID},
    {"VBox{ Probe{} Button{text=\"a\" text:\"b\"} }", Code::DUPLICATE_PROPERTY},
    {"VBox{ Probe{} Button{\"a\" text=\"b\"} }", Code::DUPLICATE_PROPERTY},
    {"VBox{ Probe{} Label{Button{}} }", Code::CHILDREN_FORBIDDEN},
    {"VBox{ Probe{} Label{width=2+3} }", Code::SYNTAX},
    {"VBox{ Probe{} Label{style=someExpression()} }", Code::UNKNOWN_PROPERTY},
    {"VBox{ Probe{} Label{width=-1} }", Code::INVALID_VALUE},
    {"VBox{ Probe{} Label{flex=0} }", Code::INVALID_VALUE},
    {"VBox{ Probe{} Label{height=2147483648} }", Code::INVALID_VALUE},
    {"VBox{ Probe{} CheckBox{checked=1} }", Code::INVALID_VALUE},
    {"VBox{ Probe{}", Code::SYNTAX},
  };

  for (const auto &scenario : scenarios) {
    CAPTURE(scenario.source);

    int calls{};
    ui::SceneResourceOptions options;

    REQUIRE(options.registry.add<Custom>("Probe", [&](ui::SceneResourceArguments arguments) {
      ++calls;

      return ui::custom<Custom>(std::move(arguments.text));
    }));

    const auto failure = rejected(scenario.source, std::move(options));

    CHECK(failure.sceneResource->code == scenario.expected);
    CHECK(calls == 0);
    CHECK(Custom::alive == 0);
  }
}

TEST_CASE("resource failures own exact source and byte based position after owner destruction") {
  std::string source = "VBox{\n  Label{\"Привет\"}\n  Unknown{}\n}";
  const auto expected = source;
  const auto failure = rejected(source);

  source.assign("replaced");
  REQUIRE(failure.sceneResource->source);
  CHECK(*failure.sceneResource->source == expected);
  CHECK(failure.sceneResource->offset == expected.find("Unknown"));
  CHECK(failure.sceneResource->line == 3);
  CHECK(failure.sceneResource->column == 3);
  CHECK(cause(failure, TreeError::INVALID_ARGUMENT));
}

TEST_CASE("source UTF8 is checked including comments and escaped text is owning") {
  for (const std::string invalid : {std::string("Label{\"\xC0\xAF\"}"), std::string("Label{} // \xF4\x90\x80\x80")}) {
    CHECK(rejected(invalid).sceneResource->code == Code::INVALID_UTF8);
  }

  CHECK(rejected("Label{\"\\q\"}").sceneResource->code == Code::SYNTAX);
  CHECK(rejected("Label{\"unfinished}").sceneResource->code == Code::SYNTAX);

  auto owner = ui::scene(R"(Label#text{"say \"hello\"\\"})");
  auto text = owner.get<Label>("text");

  REQUIRE(owner.prepareChecked());
  REQUIRE(text.text().get());
  CHECK(*text.text().get() == "say \"hello\"\\");
}

TEST_CASE("all parser limits reject before factories and exact boundaries remain usable") {
  ui::SceneResourceOptions options;

  options.limits.maxSourceBytes = 7;
  CHECK(rejected("Label{\"\"}", options).sceneResource->code == Code::SOURCE_LIMIT);
  options.limits = {};
  options.limits.maxNodes = 1;
  CHECK(rejected("VBox{Label{}}", options).sceneResource->code == Code::NODE_LIMIT);
  options.limits = {};
  options.limits.maxDepth = 1;
  CHECK(rejected("VBox{Label{}}", options).sceneResource->code == Code::DEPTH_LIMIT);
  options.limits = {};
  options.limits.maxStringBytes = 2;
  CHECK(rejected("Label{\"abc\"}", options).sceneResource->code == Code::STRING_LIMIT);
  options.limits = {};
  options.limits.maxProperties = 1;
  CHECK(rejected("Label{text=\"a\" width=1}", options).sceneResource->code == Code::PROPERTY_LIMIT);
  options.limits = {};
  options.limits.maxDepth = 257;
  CHECK(rejected("Label{}", options).sceneResource->code == Code::INVALID_LIMIT);
  options.limits = {};
  options.limits.maxNodes = 0;
  CHECK(rejected("Label{}", options).sceneResource->code == Code::INVALID_LIMIT);
  options.limits = {.maxSourceBytes = 10, .maxNodes = 1, .maxDepth = 1, .maxStringBytes = 1, .maxProperties = 1};

  auto owner = ui::scene("Label{\"a\"}", options);

  REQUIRE(owner.prepareChecked());
}

TEST_CASE("resource depth limits include exact default and configured boundaries") {
  for (const std::size_t depth : {std::size_t{16}, std::size_t{32}, std::size_t{64}}) {
    std::string source;

    for (std::size_t index = 0; index < depth; ++index) {
      source += "VBox{";
    }

    source.append(depth, '}');

    ui::SceneResourceOptions options;

    options.limits.maxDepth = depth;

    example::ReplayTerminal terminal({}, 100, {24, 8});
    ui::SceneOptions sceneOptions;

    sceneOptions.clock = terminal.clock();

    auto owner = ui::scene(source, options, sceneOptions);
    auto prepared = owner.prepareChecked();

    REQUIRE(prepared);

    auto rendered = owner.run(terminal);

    REQUIRE(rendered);
    CHECK(rendered->frames == 1);
    CHECK(terminal.closed);

    options.limits.maxDepth = depth - 1;
    CHECK(rejected(source, options).sceneResource->code == Code::DEPTH_LIMIT);
  }

  ui::SceneResourceOptions unsupported;

  unsupported.limits.maxDepth = 256;
  CHECK(rejected("VBox{}", unsupported).sceneResource->code == Code::INVALID_LIMIT);

  std::string overlong;

  for (std::size_t index = 0; index < 256; ++index) {
    overlong += "VBox{";
  }

  overlong.append(256, '}');
  CHECK(rejected(overlong).sceneResource->code == Code::DEPTH_LIMIT);
}

TEST_CASE("resource files are explicit lazy bounded and retain file identity on failure") {
  TemporaryFile file;
  auto owner = ui::scene(ui::SceneResourceFile{file.path});

  file.write("Button#loaded{\"from file\"}");

  auto loaded = owner.get<Button>("loaded");

  REQUIRE(owner.prepareChecked());
  REQUIRE(loaded.text().get());
  CHECK(*loaded.text().get() == "from file");

  ui::SceneResourceOptions small;

  small.limits.maxSourceBytes = 5;

  auto limited = ui::scene(ui::SceneResourceFile{file.path}, small);
  auto limit = limited.prepareChecked();

  REQUIRE_FALSE(limit);
  REQUIRE(limit.error().sceneResource);
  CHECK(limit.error().sceneResource->code == Code::SOURCE_LIMIT);
  CHECK(*limit.error().sceneResource->source == file.path.generic_string());

  std::error_code ignored;

  std::filesystem::remove(file.path, ignored);

  example::ReplayTerminal terminal;
  auto absent = ui::scene(ui::SceneResourceFile{file.path});
  auto failure = absent.prepareChecked();

  REQUIRE_FALSE(failure);
  REQUIRE(failure.error().sceneResource);
  CHECK(failure.error().sceneResource->code == Code::FILE_IO);
  CHECK(failure.error().sceneResource->system);
  CHECK(absent.runApp(terminal) == 1);
  CHECK(terminal.output.empty());
  CHECK_FALSE(terminal.closed);
}

TEST_CASE("explicit resolver localizes every text before invoking custom factories") {
  int resolved{}, factories{};
  ui::SceneResourceOptions options;

  options.resolver = [&](std::string_view key) -> std::expected<std::string, TextResourceError> {
    ++resolved;

    return key == "greeting" ? "Здравствуйте" : "Добавить";
  };
  REQUIRE(options.registry.add<Custom>("Localized", [&](ui::SceneResourceArguments arguments) {
    ++factories;
    CHECK(resolved == 2);
    CHECK(arguments.text == "Здравствуйте");

    return ui::custom<Custom>(std::move(arguments.text));
  }));

  auto owner = ui::scene("VBox{ Localized{i18n(\"greeting\")} Button#add{text=i18n(\"add\")} }", options);
  auto add = owner.get<Button>("add");

  REQUIRE(owner.prepareChecked());
  CHECK(factories == 1);
  REQUIRE(add.text().get());
  CHECK(*add.text().get() == "Добавить");
  REQUIRE(owner.close());
  CHECK(Custom::alive == 0);

  const auto missing = rejected("Label{i18n(\"absent\")}");

  REQUIRE(missing.resource);
  CHECK(*missing.resource == TextResourceError::RESOLVER_REQUIRED);
  CHECK(missing.sceneResource->code == Code::TEXT);

  options.resolver = [](std::string_view) -> std::expected<std::string, TextResourceError> {
    return std::unexpected(TextResourceError::MISSING_RESOURCE);
  };

  const auto notFound = rejected("VBox{ Localized{} Label{i18n(\"absent\")} }", options);

  CHECK(factories == 1);
  REQUIRE(notFound.resource);
  CHECK(*notFound.resource == TextResourceError::MISSING_RESOURCE);
}

TEST_CASE("custom registry rejects replacement and retains factory captures until prepare") {
  ui::SceneResourceOptions options;
  auto text = std::make_shared<std::string>("custom text");
  auto weak = std::weak_ptr(text);
  ui::Ref<Custom> reference;

  REQUIRE(options.registry.add<Custom>("Custom", [text, reference](ui::SceneResourceArguments) {
    return ui::custom<Custom>(reference, *text);
  }));
  CHECK_FALSE(options.registry.add<Custom>("Button", [](ui::SceneResourceArguments) {
    return ui::custom<Custom>("");
  }));
  CHECK_FALSE(options.registry.add<Custom>("bad-name", [](ui::SceneResourceArguments) {
    return ui::custom<Custom>("");
  }));

  auto owner = ui::scene("Custom#custom{width=8}", std::move(options));

  text.reset();
  CHECK_FALSE(weak.expired());
  REQUIRE(owner.prepareChecked());
  CHECK(reference.node() != Node<Custom>{});
  CHECK(Custom::alive == 1);
  REQUIRE(owner.close());
  CHECK(Custom::alive == 0);
}

TEST_CASE("custom mount failure rolls back complete resource without publishing earlier references") {
  ui::Ref<Custom> reference;
  ui::SceneResourceOptions options;

  REQUIRE(options.registry.add<Custom>("Good", [reference](ui::SceneResourceArguments) {
    return ui::custom<Custom>(reference, "alive");
  }));
  REQUIRE(options.registry.add<Custom>("Fail", [](ui::SceneResourceArguments) {
    return ui::custom<Custom>("failure").setup([](ManagedScene &, Node<Custom>) -> std::expected<void, ManagedError> {
      return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::WOULD_BLOCK});
    });
  }));

  auto owner = ui::scene("VBox{ Good{}\n Fail{} }", std::move(options));
  auto result = owner.prepareChecked();

  REQUIRE_FALSE(result);
  CHECK(cause(result.error(), TreeError::WOULD_BLOCK));
  CHECK(result.error().phase == ManagedPhase::BIND);
  REQUIRE(result.error().sceneResource);
  CHECK(result.error().sceneResource->line == 2);
  CHECK(result.error().sceneResource->column == 2);
  CHECK(Custom::alive == 0);
  CHECK(reference.node() == Node<Custom>{});
}

TEST_CASE("factory throw and typed rejection preserve exact cause and resource location") {
  for (const bool throwing : {false, true}) {
    ui::SceneResourceOptions options;

    REQUIRE(options.registry.add<Custom>(
      "Fail", [throwing](ui::SceneResourceArguments) -> std::expected<ui::Description<Custom>, ManagedError> {
        if (throwing) {
          throw std::runtime_error("factory");
        }

        return std::unexpected(ManagedError{ManagedPhase::STATE, {}, TreeError::NODE_LIMIT});
      }));

    const auto failure = rejected("VBox{\n Fail{} }", std::move(options));

    CHECK(cause(failure, throwing ? TreeError::CALLBACK_FAILED : TreeError::NODE_LIMIT));
    CHECK(failure.sceneResource->code == Code::FACTORY);
    CHECK(failure.sceneResource->line == 2);
  }
}

TEST_CASE("resource error copies share immutable context without allocating") {
  const auto original = rejected("VBox{\n Unknown{} }");
  ManagedError assigned{};

  allocationsBeforeFailure = 0;
  const ManagedError copied(original);
  assigned = original;
  allocationsBeforeFailure = -1;

  CHECK(copied.sceneResource == original.sceneResource);
  CHECK(assigned.sceneResource == original.sceneResource);
  REQUIRE(copied.sceneResource);
  CHECK(copied.sceneResource->code == Code::UNKNOWN_TYPE);
  CHECK(copied.sceneResource->line == 2);
  CHECK(cause(copied, TreeError::INVALID_ARGUMENT));
  CHECK(cause(assigned, TreeError::INVALID_ARGUMENT));
}

TEST_CASE("resource attribution allocation failure preserves original typed factory error") {
  ui::SceneResourceOptions options;
  REQUIRE(options.registry.add<Custom>(
    "Fail", [](ui::SceneResourceArguments) -> std::expected<ui::Description<Custom>, ManagedError> {
      allocationsBeforeFailure = 0;
      return std::unexpected(ManagedError{ManagedPhase::STATE, {}, TreeError::NODE_LIMIT});
    }));
  auto owner = ui::scene("VBox{\n Fail{} }", std::move(options));

  const auto result = owner.prepareChecked();
  allocationsBeforeFailure = -1;

  REQUIRE_FALSE(result);
  CHECK(cause(result.error(), TreeError::NODE_LIMIT));
  CHECK(result.error().phase == ManagedPhase::STATE);
  CHECK_FALSE(result.error().sceneResource);
  CHECK(Custom::alive == 0);
  REQUIRE(owner.close());
}

TEST_CASE("prepare reentry and throwing notification cannot replace the first resource failure") {
  ui::SceneResourceOptions options;
  ui::Scene *active{};

  REQUIRE(options.registry.add<Custom>("Reenter", [&](ui::SceneResourceArguments) {
    auto nested = active->prepareChecked();

    REQUIRE_FALSE(nested);
    CHECK(cause(nested.error(), TreeError::WOULD_BLOCK));

    return ui::custom<Custom>("reentry");
  }));

  auto owner = ui::scene("Reenter{}", std::move(options));

  active = &owner;

  auto failure = owner.prepareChecked();

  REQUIRE_FALSE(failure);
  CHECK(cause(failure.error(), TreeError::WOULD_BLOCK));
  CHECK(Custom::alive == 0);

  auto invalid = ui::scene("Unknown{}");
  int notified{}, reported{};

  invalid.onError([&](const ManagedDiagnostic &diagnostic) {
    ++notified;
    REQUIRE(diagnostic.error.sceneResource);
    CHECK(diagnostic.error.sceneResource->code == Code::UNKNOWN_TYPE);
    throw std::runtime_error("rejected notification");
  });
  invalid.reportOnError([&](const ui::SceneReport &report) {
    ++reported;
    REQUIRE(report.failure);
    REQUIRE(report.failure->sceneResource);
    CHECK(report.failure->sceneResource->code == Code::UNKNOWN_TYPE);
  });

  example::ReplayTerminal terminal;

  CHECK(invalid.runApp(terminal) == 1);
  CHECK(notified == 1);
  CHECK(reported == 1);
  CHECK(terminal.output.empty());
}

TEST_CASE("self removal during mount leaves no published resource identity") {
  ui::Ref<Custom> reference;
  ui::SceneResourceOptions options;

  REQUIRE(options.registry.add<Custom>("Gone", [reference](ui::SceneResourceArguments) {
    return ui::custom<Custom>(reference, "gone").setup([](ManagedScene &managed, Node<Custom> node) {
      return managed.remove(node);
    });
  }));

  auto owner = ui::scene("VBox{Gone{}}", std::move(options));
  auto failure = owner.prepareChecked();

  REQUIRE_FALSE(failure);
  CHECK(cause(failure.error(), TreeError::STALE_NODE));
  CHECK(reference.node() == Node<Custom>{});
  CHECK(Custom::alive == 0);
}

TEST_CASE("resource composes with a late managed model and the enclosing C++ transaction") {
  ui::Ref<Label> sibling;
  auto owner = ui::scene([sibling](ManagedScene &managed) -> std::expected<ui::Description<Column>, ManagedError> {
    auto state = managed.state(std::string("late model"));

    if (!state) {
      return std::unexpected(state.error());
    }

    ui::SceneResourceOptions options;
    const auto registered = options.registry.add<LineEdit>("BoundField", [state = *state](ui::SceneResourceArguments) {
      return ui::lineEdit(state);
    });

    REQUIRE(registered);

    return ui::vbox(ui::label(sibling, "sibling"),
                    ui::resource("VBox{BoundField#field{}}", std::move(options)).id("resource"));
  });
  auto field = owner.get<LineEdit>("field");
  auto root = owner.get<Column>("resource");

  REQUIRE(owner.prepareChecked());
  CHECK(root.node() != Node<Column>{});
  REQUIRE(field.text().get());
  CHECK(*field.text().get() == "late model");
  CHECK(sibling.node() != Node<Label>{});
  REQUIRE(owner.close());

  ui::Ref<Label> unpublished;
  auto invalid = ui::scene(ui::vbox(ui::label(unpublished, "sibling"), ui::resource("Missing{}")));

  REQUIRE_FALSE(invalid.prepareChecked());
  CHECK(unpublished.node() == Node<Label>{});
}

TEST_CASE("resource preparation allocation failures remain checked and retire partial ownership") {
  constexpr std::ptrdiff_t MAX_BUDGET = 2048;
  for (const int mode : {0, 1, 2}) {
    const bool localized = mode == 1;
    const bool configured = mode == 2;
    std::size_t failed{}, succeeded{};

    for (std::ptrdiff_t budget = 0; budget <= MAX_BUDGET; ++budget) {
      ui::Ref<Custom> reference;
      ui::SceneResourceOptions options;

      options.format = configured ? ui::SceneResourceFormat::CONFIGURATION : ui::SceneResourceFormat::LEGACY;

      options.resolver = [](std::string_view) -> std::expected<std::string, TextResourceError> {
        return "translated";
      };

      REQUIRE(options.registry.add<Custom>("Custom", [reference](ui::SceneResourceArguments arguments) {
        return ui::custom<Custom>(reference, std::move(arguments.text));
      }));

      const auto source =
        configured
          ? "definitions{Short=LineEdit{history{maxOperations=2}}} "
            "defaults{LineEdit{history{groupTyping=false}}} "
            "scene{VBox{Custom{text=\"owned\"} Short#field{text=i18n(\"initial\")} ThreeState{text=\"check\"}}}"
        : localized
          ? "VBox{Custom{i18n(\"own\")} LineEdit#field{i18n(\"initial\")} HBox{Label#caption{i18n(\"caption\")} "
            "Button#button{i18n(\"button\")} CheckBox#check{i18n(\"check\") checked=true}}}"
          : "VBox{Custom{\"owning temporary\"} LineEdit#field{\"initial\"} HBox{Label#caption{\"label\"} "
            "Button#button{\"button\"} CheckBox#check{\"check\" checked=true}}}";
      auto owner = ui::scene(source, std::move(options));
      std::expected<void, ManagedError> result;
      bool escaped{};

      const auto previousTerminate = std::set_terminate(oomTerminate);

      activeBudget = budget;
      allocationsBeforeFailure = budget;

      try {
        result = owner.prepareChecked();
      } catch (...) {
        escaped = true;
      }

      allocationsBeforeFailure = -1;
      std::set_terminate(previousTerminate);

      CAPTURE(budget);
      REQUIRE_FALSE(escaped);

      if (!result) {
        ++failed;

        const auto *application = std::get_if<ApplicationError>(&result.error().cause);

        CHECK((cause(result.error(), TreeError::NO_MEMORY) || application));
        CHECK(Custom::alive == 0);
        CHECK(reference.node() == Node<Custom>{});

        // Persistent allocation rejection also prevents best-effort diagnostic context.
        CHECK_FALSE(result.error().sceneResource);
      } else {
        ++succeeded;
      }

      REQUIRE(owner.close());
      CHECK(Custom::alive == 0);

      if (succeeded) {
        break;
      }
    }

    CHECK(failed > 0);
    CHECK(succeeded == 1);
    std::fprintf(stderr, "resource OOM sweep mode=%d failures=%zu first-success-budget=%td\n", mode, failed,
                 activeBudget);
  }
}

TEST_CASE("resource text errors survive allocation rejection during optional provenance enrichment") {
  bool resolved{};
  ui::SceneResourceOptions options;
  options.format = ui::SceneResourceFormat::CONFIGURATION;
  options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
    resolved = true;
    allocationsBeforeFailure = 0;

    return std::unexpected(TextResourceError::MISSING_RESOURCE);
  };

  auto owner = ui::scene("scene { LineEdit { text=i18n(\"missing\") } }", options);
  std::expected<void, ManagedError> result;
  bool escaped{};

  try {
    result = owner.prepareChecked();
  } catch (...) {
    escaped = true;
  }

  allocationsBeforeFailure = -1;
  REQUIRE_FALSE(escaped);
  REQUIRE(resolved);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().resource);
  CHECK(*result.error().resource == TextResourceError::MISSING_RESOURCE);
  REQUIRE(owner.close());
}

TEST_CASE("custom recipe preserves nonstring move-only arguments with and without owning text") {
  struct Owned final : Widget {
    std::string text;
    std::unique_ptr<int> value;

    explicit Owned(std::unique_ptr<int> input) : value(std::move(input)) {
    }

    Owned(std::string_view label, std::unique_ptr<int> input) : text(label), value(std::move(input)) {
    }
  };

  ui::Ref<Owned> first, second;
  auto one = std::make_unique<int>(41);
  auto two = std::make_unique<int>(42);
  auto owner = ui::scene(ui::vbox(ui::custom<Owned>(first, std::move(one)),
                                  ui::custom<Owned>(second, std::string("owned label"), std::move(two))));
  bool verified{};

  owner.configure([&](ManagedScene &, Application &application) {
    REQUIRE(application.tree().read(first.node(), [](const Owned &widget) {
      CHECK(*widget.value == 41);
    }));
    REQUIRE(application.tree().read(second.node(), [](const Owned &widget) {
      CHECK(*widget.value == 42);
      CHECK(widget.text == "owned label");
    }));
    verified = true;
  });

  CHECK_FALSE(one);
  CHECK_FALSE(two);
  REQUIRE(owner.prepareChecked());
  CHECK(verified);
}

TEST_CASE("complete public resource form handles Add Hello and F10 with existing input routing") {
  example::ReplayTerminal terminal({{20, "\t\r", {}}, {60, "\x1b[21~", {}}}, 100, {40, 8});
  ui::SceneOptions options;

  options.clock = terminal.clock();

  auto owner = scene_resource_example::form(options);
  auto message = owner.get<LineEdit>("message");
  std::string final;

  owner.onStop([&] {
    auto value = message.text().get();

    REQUIRE(value);
    final = *value;
  });

  const auto result = owner.run(terminal);

  REQUIRE(result);
  CHECK(final == "Hello!");
  CHECK(terminal.closed);
  CHECK(result->frames >= 2);
  CHECK_FALSE(message.text().get());
}

TEST_CASE("fluent resource controls retain lvalue identity and consume temporary chains") {
  int created{};
  ui::SceneResourceOptions options;
  auto factory = [&](ui::SceneResourceArguments arguments) {
    ++created;

    return ui::custom<Custom>(arguments.text);
  };

  CHECK(&options.control<Custom>("First", factory).control<Custom>("Second", factory) == &options);
  CHECK(created == 0);

  auto owner = ui::scene("VBox{First{} Second{}}", options);

  REQUIRE(owner.prepareChecked());
  CHECK(created == 2);
  CHECK(Custom::alive == 2);
  REQUIRE(owner.close());
  CHECK(Custom::alive == 0);

  auto temporary = ui::scene("Group{Leaf{}}", ui::SceneResourceOptions{}
                                                .control<Column>(
                                                  "Group",
                                                  [](ui::SceneResourceArguments) {
                                                    return ui::column();
                                                  },
                                                  true)
                                                .control<Custom>("Leaf", factory));

  REQUIRE(temporary.prepareChecked());
  CHECK(created == 3);
  REQUIRE(temporary.close());
  CHECK(Custom::alive == 0);
}

TEST_CASE("fluent resource registration rejects before parser resolver factories and terminal") {
  for (const auto name : {"Label", "bad-name"}) {
    int created{}, resolved{}, notified{};
    ui::SceneResourceOptions options;

    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolved;

      return "unused";
    };
    auto factory = [&](ui::SceneResourceArguments) {
      ++created;

      return ui::custom<Custom>("unused");
    };

    options.control<Custom>("Good", factory).control<Custom>(name, factory);

    auto owner = ui::scene("invalid syntax i18n(\"unused\")", options);
    const auto expected = name == std::string_view("Label") ? Code::DUPLICATE_TYPE : Code::UNKNOWN_TYPE;

    owner.onError([&](const ManagedDiagnostic &diagnostic) {
      ++notified;
      REQUIRE(diagnostic.error.sceneResource);
      CHECK(diagnostic.error.sceneResource->code == expected);
      CHECK(diagnostic.error.sceneResource->line == 0);
      CHECK(diagnostic.error.sceneResource->column == 0);
      CHECK(cause(diagnostic.error, TreeError::INVALID_ARGUMENT));
    });

    example::ReplayTerminal terminal;

    CHECK(owner.runApp(terminal) == 1);
    CHECK(notified == 1);
    CHECK(created == 0);
    CHECK(resolved == 0);
    CHECK(terminal.output.empty());
    CHECK_FALSE(terminal.closed);

    CHECK(rejected("Good{i18n(\"unused\")}", options).sceneResource->code == expected);
    CHECK(created == 0);
    CHECK(resolved == 0);

    TemporaryFile absent;
    auto fileOwner = ui::scene(ui::SceneResourceFile{absent.path}, std::move(options));
    auto fileResult = fileOwner.prepareChecked();

    REQUIRE_FALSE(fileResult);
    REQUIRE(fileResult.error().sceneResource);
    CHECK(fileResult.error().sceneResource->code == expected);
    CHECK(fileResult.error().sceneResource->line == 0);
    CHECK(created == 0);
    CHECK(resolved == 0);
  }
}

namespace {
struct RegistrationFactory {
  std::shared_ptr<int> copies;
  bool *reject{};
  bool allocationFailure{};

  RegistrationFactory(std::shared_ptr<int> counter, bool &rejection, bool allocation = false)
      : copies(std::move(counter)), reject(&rejection), allocationFailure(allocation) {
  }

  RegistrationFactory(const RegistrationFactory &other)
      : copies(other.copies), reject(other.reject), allocationFailure(other.allocationFailure) {
    ++*copies;

    if (*reject) {
      if (allocationFailure) {
        throw std::bad_alloc();
      }

      throw std::runtime_error("registration copy");
    }
  }

  RegistrationFactory(RegistrationFactory &&) = default;

  ui::Description<Custom> operator()(ui::SceneResourceArguments arguments) {
    return ui::custom<Custom>(arguments.text);
  }
};
} // namespace

TEST_CASE("fluent registration first failure stops later capture work and survives copies") {
  bool reject{};
  auto copies = std::make_shared<int>();
  RegistrationFactory factory(copies, reject);
  ui::SceneResourceOptions options;

  options.control<Custom>("bad-name", factory);

  const auto before = *copies;

  reject = true;
  options.control<Custom>("Label", factory);
  CHECK(*copies == before);

  auto copy = options;
  auto moved = std::move(copy);

  options = {};
  CHECK(rejected("Label{}", std::move(moved)).sceneResource->code == Code::UNKNOWN_TYPE);
}

TEST_CASE("fluent resource callbacks and options copies retain owning captures") {
  auto lifetime = std::make_shared<std::string>("retained");
  std::weak_ptr<std::string> observed = lifetime;
  ui::SceneResourceOptions options;

  options.control<Custom>("Owned", [lifetime](ui::SceneResourceArguments) {
    return ui::custom<Custom>(*lifetime);
  });

  auto copy = options;
  auto moved = std::move(copy);

  lifetime.reset();
  options = {};

  {
    auto owner = ui::scene("Owned{}", std::move(moved));

    REQUIRE(owner.prepareChecked());
    CHECK_FALSE(observed.expired());
    REQUIRE(owner.close());
  }

  // Registry has a fallible copy transfer on checked MSVC; release any source captures too.
  copy = {};
  moved = {};
  CHECK(observed.expired());
  CHECK(Custom::alive == 0);
}

TEST_CASE("checked registry failure remains recoverable before fluent registration") {
  ui::SceneResourceOptions options;
  auto factory = [](ui::SceneResourceArguments arguments) {
    return ui::custom<Custom>(arguments.text);
  };

  CHECK_FALSE(options.registry.add<Custom>("Label", factory));
  CHECK_FALSE(options.registry.add<Custom>("bad-name", factory));
  options.control<Custom>("Recovered", factory);

  auto owner = ui::scene("Recovered{}", std::move(options));

  REQUIRE(owner.prepareChecked());
  REQUIRE(owner.close());
  CHECK(Custom::alive == 0);
}

TEST_CASE("fluent registration captures internal callback copy exceptions as checked failures") {
  for (const bool allocation : {false, true}) {
    bool reject = true;
    RegistrationFactory factory(std::make_shared<int>(), reject, allocation);
    ui::SceneResourceOptions options;

    CHECK_NOTHROW(options.control<Custom>("Throwing", factory));

    reject = false;
    options.control<Custom>("Label", factory);

    const auto failure = rejected("Throwing{}", std::move(options));

    CHECK(cause(failure, allocation ? TreeError::NO_MEMORY : TreeError::CALLBACK_FAILED));
    CHECK(failure.sceneResource->code == (allocation ? Code::NO_MEMORY : Code::FACTORY));
    CHECK(failure.sceneResource->line == 0);
  }
}

TEST_CASE("fluent registration allocation rejection retains an allocation-free first failure") {
  std::size_t failures{};
  bool succeeded{};

  for (std::ptrdiff_t budget = 0; budget < 128 && !succeeded; ++budget) {
    ui::SceneResourceOptions options;
    auto factory = [](ui::SceneResourceArguments arguments) {
      return ui::custom<Custom>(arguments.text);
    };
    bool escaped{};

    allocationsBeforeFailure = budget;

    try {
      options.control<Custom>("AllocationProbeWithAnOwningRegistryName", factory);
    } catch (...) {
      escaped = true;
    }

    allocationsBeforeFailure = -1;
    CHECK_FALSE(escaped);

    auto owner = ui::scene("AllocationProbeWithAnOwningRegistryName{}", std::move(options));
    auto result = owner.prepareChecked();

    if (!result) {
      ++failures;
      CHECK(cause(result.error(), TreeError::NO_MEMORY));
      REQUIRE(result.error().sceneResource);
      CHECK(result.error().sceneResource->code == Code::NO_MEMORY);
      CHECK(result.error().sceneResource->line == 0);
    } else {
      succeeded = true;
    }

    REQUIRE(owner.close());
    CHECK(Custom::alive == 0);
  }

  CHECK(failures > 0);
  CHECK(succeeded);
}

TEST_CASE("registered resource Dialog layers use the same selected scene host and exact owning refs") {
  for (const bool explicitHost : {false, true}) {
    ui::Ref<OverlayHost> host;
    ui::Ref<Dialog> dialog;
    ui::Ref<Button> opener, inside;
    ui::SceneResourceOptions options;
    Application *application{};
    int factories{};

    REQUIRE(options.registry.add<Dialog>("Confirm", [&, dialog, inside](ui::SceneResourceArguments arguments) {
      ++factories;

      return ui::dialog(dialog, arguments.text, ui::button(inside, "Accept").onClick([] {
      }));
    }));

    auto body = ui::button(opener, "Open").onClick([] {
    });
    auto layer = ui::resource("Confirm{\"Resource confirmation\"}", std::move(options));
    auto owner = explicitHost ? ui::scene(ui::overlayHost(host, std::move(body)), std::move(layer))
                              : ui::scene(std::move(body), std::move(layer));

    owner.configure([&](ManagedScene &, Application &app) {
      application = &app;
    });
    CHECK(factories == 0);
    REQUIRE(owner.prepareChecked());
    REQUIRE(owner.prepareChecked());
    CHECK(factories == 1);

    const auto parent = application->tree().inspect(dialog.node());

    REQUIRE(parent);

    const auto selected = parent->parent;
    const auto hostSnapshot = application->tree().inspect(selected);

    REQUIRE(hostSnapshot);

    CHECK(hostSnapshot->children == std::vector<Node<Widget>>{opener.node(), dialog.node()});
    CHECK((explicitHost ? host.node() == selected : host.node() == Node<OverlayHost>{}));
    REQUIRE(opener.focus());
    REQUIRE(dialog.open().result());
    const auto entered = application->router().focused();

    REQUIRE(entered);
    CHECK(*entered == inside.node());
    REQUIRE(dialog.dismiss().result());

    const auto returned = application->router().focused();

    REQUIRE(returned);
    CHECK(*returned == opener.node());
    REQUIRE(dialog.open().result());

    const auto reopened = application->tree().inspect(selected);

    REQUIRE(reopened);
    CHECK(reopened->children == hostSnapshot->children);
    REQUIRE(owner.close());
  }
}

TEST_CASE("resource layer rejection retires the unpublished body and preserves source diagnostics") {
  SUBCASE("ordinary erased root is not a Dialog layer") {
    ui::Ref<Custom> body;
    auto owner = ui::scene(ui::custom<Custom>("alive").reference(body), ui::resource("Label{\"Not a Dialog\"}"));
    const auto result = owner.prepareChecked();

    REQUIRE_FALSE(result);
    CHECK(cause(result.error(), TreeError::INVALID_ARGUMENT));
    CHECK(Custom::alive == 0);
    CHECK(body.node() == Node<Custom>{});
  }

  SUBCASE("checked custom layer setup error retains original resource attribution") {
    ui::Ref<Label> body;
    ui::Ref<Dialog> layer;
    ui::SceneResourceOptions options;
    int factories{};

    REQUIRE(options.registry.add<Dialog>("Fail", [&, layer](ui::SceneResourceArguments) {
      ++factories;

      return ui::dialog(layer, "Failure", ui::label("Panel"))
        .setup([](ManagedScene &, Node<Dialog>) -> std::expected<void, ManagedError> {
          return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::WOULD_BLOCK});
        });
    }));

    auto owner = ui::scene(ui::label(body, "Body"), ui::resource("\nFail{}", std::move(options)));
    const auto result = owner.prepareChecked();

    REQUIRE_FALSE(result);
    CHECK(cause(result.error(), TreeError::WOULD_BLOCK));
    CHECK(result.error().phase == ManagedPhase::BIND);
    REQUIRE(result.error().sceneResource);
    CHECK(result.error().sceneResource->line == 2);
    CHECK(result.error().sceneResource->column == 1);
    CHECK(factories == 1);
    CHECK(body.node() == Node<Label>{});
    CHECK(layer.node() == Node<Dialog>{});
  }

  SUBCASE("resource body hiding a host is not an explicit typed root override") {
    ui::Ref<OverlayHost> host;
    ui::Ref<Dialog> layer;
    ui::SceneResourceOptions options;

    REQUIRE(options.registry.add<OverlayHost>("Host", [host](ui::SceneResourceArguments) {
      return ui::overlayHost(host, ui::label("Body"));
    }));

    auto owner = ui::scene(ui::resource("Host{}", std::move(options)), ui::dialog(layer, "Layer", ui::label("Panel")));
    const auto result = owner.prepareChecked();

    REQUIRE_FALSE(result);
    CHECK(cause(result.error(), TreeError::INVALID_ARGUMENT));
    CHECK(host.node() == Node<OverlayHost>{});
    CHECK(layer.node() == Node<Dialog>{});
  }
}

TEST_CASE("key-help static validation precedes reachable resolver and custom factory effects") {
  struct Scenario {
    std::string_view body;
    Code code;
    std::string_view path;
  };

  const Scenario scenarios[]{
    {"KeyHints{}", Code::INVALID_VALUE, "KeyHints.help"},
    {"KeyHints{help=\"\"} HelpView{}", Code::INVALID_VALUE, "KeyHints.help"},
    {"KeyHints{help=\"missing\"} HelpView{}", Code::INVALID_VALUE, "KeyHints.help"},
    {"KeyHints{help=\"label\"} Label#label{}", Code::INVALID_VALUE, "KeyHints.help"},
    {"KeyHints{} HelpView{} HelpView{}", Code::INVALID_VALUE, "KeyHints.help"},
    {"KeyHints{overflowMarker=\"\"} HelpView{}", Code::INVALID_VALUE, "KeyHints.overflowMarker"},
    {"HelpView{format{maxBytesPerBinding=0}}", Code::INVALID_VALUE, "HelpView.format.maxBytesPerBinding"},
    {"HelpView{entrySeparator=\"bad\"}", Code::UNKNOWN_PROPERTY, "HelpView.entrySeparator"},
    {"KeyHints{format{unknownMarker=i18n(\"literal-only\")}} HelpView{}", Code::SYNTAX,
     "KeyHints.format.unknownMarker"},
    {"HelpView{emptyText=\"too many bytes\"}", Code::STRING_LIMIT, "HelpView.emptyText"},
  };
  for (const auto &scenario : scenarios) {
    INFO("body: ", scenario.body);
    int resolutions{}, factories{};
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.limits.maxStringBytes = 12;
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;
      return "resolved";
    };
    REQUIRE(options.registry.add<Plain>("Probe", [&](ui::SceneResourceArguments) {
      ++factories;
      return ui::custom<Plain>();
    }));
    const auto source = "scene{VBox{Label{text=i18n(\"early\")} Probe{} " + std::string(scenario.body) + "}}";
    const auto failure = rejected(source, options);
    CHECK(failure.sceneResource->code == scenario.code);
    REQUIRE(failure.sceneResource->propertyPath);
    CHECK(*failure.sceneResource->propertyPath == scenario.path);
    CHECK(resolutions == 0);
    CHECK(factories == 0);
    CHECK(Plain::alive == 0);
  }
}

TEST_CASE("key-help native names stay scoped and public custom schemas retain their exact allowlist") {
  for (const auto name : {"HelpView", "KeyHints"}) {
    INFO("name: ", name);
    CHECK(rejected(std::string(name) + "{}").sceneResource->code == Code::UNKNOWN_TYPE);
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    int callbacks{};
    REQUIRE(options.registry.add<Label>(name, [&](ui::SceneResourceArguments arguments) {
      ++callbacks;
      CHECK(arguments.text == "caller");
      return ui::label(arguments.text);
    }));
    auto owner = ui::scene("scene{" + std::string(name) + "#target{text=\"caller\"}}", options);
    auto target = owner.get<Label>("target");
    REQUIRE(owner.prepareChecked());
    REQUIRE(target.text().get());
    CHECK(*target.text().get() == "caller");
    CHECK(callbacks == 1);
    REQUIRE(owner.close());
    callbacks = 0;
    auto failure = rejected("scene{" + std::string(name) + "{emptyText=\"new property\"}}", options);
    CHECK(failure.sceneResource->code == Code::UNKNOWN_PROPERTY);
    CHECK(callbacks == 0);
  }
  ui::SceneResourceOptions options;
  options.format = ui::SceneResourceFormat::CONFIGURATION;
  int callbacks{};
  REQUIRE(options.registry.add<HelpView>("CustomHelp", [&](ui::SceneResourceArguments arguments) {
    ++callbacks;
    CHECK(arguments.width == 17);
    return ui::helpView(ui::Ref<HelpView>{}).width(3).visible(true);
  }));
  auto owner = ui::scene("scene{VBox{KeyHints{} CustomHelp#pane{width=17 visible=false classes=\"local\"}}}", options);
  auto pane = owner.get<HelpView>("pane");
  owner.configure([&](ManagedScene &, Application &app) {
    const auto info = app.tree().inspect(pane.node());
    REQUIRE(info);
    CHECK_FALSE(info->visible);
  });
  REQUIRE(owner.prepareChecked());
  CHECK(callbacks == 1);
  REQUIRE(owner.close());
  callbacks = 0;
  CHECK(rejected("scene{CustomHelp{initiallyOpen=true}}", options).sceneResource->code == Code::UNKNOWN_PROPERTY);
  CHECK(callbacks == 0);
}

TEST_CASE("format grouping remains native-schema scoped and preserves caller child names") {
  for (const bool configuration : {false, true}) {
    for (const bool empty : {false, true}) {
      ui::SceneResourceOptions options;
      options.format = configuration ? ui::SceneResourceFormat::CONFIGURATION : ui::SceneResourceFormat::LEGACY;
      int callbacks{};
      REQUIRE(options.registry.add<Label>("format", [&](ui::SceneResourceArguments arguments) {
        ++callbacks;
        return ui::label(arguments.text).id("child");
      }));
      const std::string body = empty ? "VBox{format{}}" : "VBox{format{text=\"ordinary\"}}";
      auto owner = ui::scene(configuration ? "scene{" + body + "}" : body, options);
      auto child = owner.get<Label>("child");
      REQUIRE(owner.prepareChecked());
      CHECK(callbacks == 1);
      REQUIRE(child.text().get());
      CHECK(*child.text().get() == (empty ? "" : "ordinary"));
      REQUIRE(owner.close());
      if (configuration) {
        callbacks = 0;
        CHECK(rejected("scene{HelpView{format{}}}", options).sceneResource->code == Code::CHILDREN_FORBIDDEN);
        CHECK(callbacks == 0);
        auto explicitGroup = ui::scene(
          "definitions{Guide=HelpView{format{strokeSeparator=\"->\"}}} scene{Guide{format={unknownMarker=\"?\"}}}",
          options);
        REQUIRE(explicitGroup.prepareChecked());
        CHECK(callbacks == 0);
        REQUIRE(explicitGroup.close());
      }
    }
  }
}

TEST_CASE("key-help materialized settings across bit64 are charged once and share scoped defaults") {
  for (const std::size_t budget : {1U, 2U}) {
    INFO("budget: ", budget);
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.controls.emplace(ui::ControlContextLimits{.maxMaterializedLeaves = budget});
    auto owner = ui::scene("scene{VBox{KeyHints{} HelpView{initiallyOpen=false emptyText=\"none\"}}}", options);
    const auto prepared = owner.prepareChecked();
    if (budget == 1) {
      REQUIRE_FALSE(prepared);
      REQUIRE(prepared.error().sceneResource);
      CHECK(prepared.error().sceneResource->code == Code::MATERIALIZATION_LIMIT);
    } else {
      REQUIRE(prepared);
    }
    REQUIRE(owner.close());
  }
}

TEST_CASE("key-help compact staging retains untouched resource origins and owns changed literal provenance") {
  auto source = std::make_shared<ui::ControlSourceOrigin>();
  source->sourceIdentity = "help-origin.scene";
  source->byteOffset = 19;
  source->line = 2;
  source->column = 4;
  for (const bool replace : {false, true}) {
    auto recipe = replace ? ui::helpView(ui::Ref<HelpView>{})
                              .settings({.origin = source, .format = {.maxBytesPerBinding = 1}})
                              .format({.maxBytesPerBinding = 0})
                          : ui::helpView(ui::Ref<HelpView>{})
                              .settings({.origin = source, .format = {.maxBytesPerBinding = 0}})
                              .emptyText("literal");
    auto owner = ui::scene(std::move(recipe));
    auto result = owner.prepareChecked();
    REQUIRE_FALSE(result);
    REQUIRE(result.error().sceneResource);
    const auto &failure = *result.error().sceneResource;
    REQUIRE(failure.propertyPath);
    CHECK(*failure.propertyPath == "HelpView.format.maxBytesPerBinding");
    if (replace) {
      CHECK_FALSE(failure.source);
      CHECK(failure.line == 0);
    } else {
      REQUIRE(failure.source);
      CHECK(*failure.source == "help-origin.scene");
      CHECK(failure.offset == 19);
    }
    REQUIRE(owner.close());
  }
}

TEST_CASE("native key-help effective caps and UTF8 reject before callbacks even with inherited literals") {
  for (const bool invalidUtf8 : {false, true}) {
    int resolutions{}, factories{};
    ui::SceneResourceOptions options;
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.controls.emplace();
    options.controls->defaults(
      "HelpView", {.emptyText = invalidUtf8 ? std::string(1, static_cast<char>(0xff)) : std::string(32, 's')});
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;
      return "early";
    };
    REQUIRE(options.registry.add<Plain>("Probe", [&](ui::SceneResourceArguments) {
      ++factories;
      return ui::custom<Plain>();
    }));
    ui::SceneOptions sceneOptions;
    sceneOptions.managed.maxKeyProjectionBytes = 16;
    auto owner = ui::scene("scene{VBox{Label{text=i18n(\"early\")} Probe{} HelpView{}}}", options, sceneOptions);
    const auto prepared = owner.prepareChecked();
    REQUIRE_FALSE(prepared);
    REQUIRE(prepared.error().sceneResource);
    REQUIRE(prepared.error().sceneResource->propertyPath);
    CHECK(*prepared.error().sceneResource->propertyPath == "HelpView.emptyText");
    CHECK(cause(prepared.error(), invalidUtf8 ? TreeError::INVALID_ARGUMENT : TreeError::NODE_LIMIT));
    CHECK(resolutions == 0);
    CHECK(factories == 0);
    REQUIRE(owner.close());
  }
}

TEST_CASE("provider-bound scalar validation and allowlists precede all reachable callbacks") {
  struct Scenario {
    std::string_view child;
    Code code;
    std::string_view path;
  };

  const Scenario cases[]{
    {"Rows{rowHeight=0}", Code::INVALID_VALUE, "Rows.rowHeight"},
    {"Rows{rowHeight=-1}", Code::INVALID_VALUE, "Rows.rowHeight"},
    {"Rows{rowHeight=2147483648}", Code::INVALID_VALUE, "Rows.rowHeight"},
    {"Rows{overscanRows=-1}", Code::INVALID_VALUE, "Rows.overscanRows"},
    {"Rows{overscanRows=18446744073709551616}", Code::INVALID_VALUE, "Rows.overscanRows"},
    {"Rows{maxColumns=1}", Code::UNKNOWN_PROPERTY, "Rows.maxColumns"},
    {"Prices{maxColumns=0}", Code::INVALID_VALUE, "Prices.maxColumns"},
    {"Prices{maxPreparedCells=0}", Code::INVALID_VALUE, "Prices.maxPreparedCells"},
    {"Prices{maxPreparedTextBytes=0}", Code::INVALID_VALUE, "Prices.maxPreparedTextBytes"},
    {"Prices{rowHieght=2}", Code::UNKNOWN_PROPERTY, "Prices.rowHieght"},
    {"Probe{rowHeight=2}", Code::UNKNOWN_PROPERTY, "Probe.rowHeight"},
    {"VirtualList{}", Code::UNKNOWN_TYPE, {}},
    {"VirtualTable{}", Code::UNKNOWN_TYPE, {}},
  };

  for (const auto &item : cases) {
    INFO("child: ", item.child);
    int factories{}, resolutions{};
    ui::SceneResourceOptions options;

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;
      return "early";
    };
    REQUIRE(options.registry.add<VirtualList<int>>("Rows", [&](ui::SceneResourceArguments) {
      ++factories;
      return ui::Description<VirtualList<int>>{};
    }));
    REQUIRE(options.registry.add<VirtualTable<int>>("Prices", [&](ui::SceneResourceArguments) {
      ++factories;
      return ui::Description<VirtualTable<int>>{};
    }));
    REQUIRE(options.registry.add<Plain>("Probe", [&](ui::SceneResourceArguments) {
      ++factories;
      return ui::custom<Plain>();
    }));
    const auto source = "scene{VBox{Label{text=i18n(\"early\")} Probe{} " + std::string(item.child) + "}}";
    const auto failure = rejected(source, options);

    CHECK(failure.sceneResource->code == item.code);
    if (!item.path.empty()) {
      REQUIRE(failure.sceneResource->propertyPath);
      CHECK(*failure.sceneResource->propertyPath == item.path);
    }
    CHECK(factories == 0);
    CHECK(resolutions == 0);
    CHECK(Plain::alive == 0);
  }
}

TEST_CASE("data-family custom schemas accept only old narrow or full expanded exact allowlists") {
  for (const bool table : {false, true}) {
    for (const bool narrow : {false, true}) {
      INFO("table: ", table, "; narrow: ", narrow);
      ui::SceneResourceOptions options;
      int factories{}, resolutions{};
      std::vector<std::string_view> paths{"text", "width", "height", "flex"};

      options.format = ui::SceneResourceFormat::CONFIGURATION;
      options.controls.emplace();
      if (!narrow) {
        paths.push_back("rowHeight");
      }
      options.controls->registerType("Bound", paths);
      options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
        ++resolutions;
        return "early";
      };
      if (table) {
        REQUIRE(options.registry.add<VirtualTable<int>>("Bound", [&](ui::SceneResourceArguments) {
          ++factories;
          return ui::Description<VirtualTable<int>>{};
        }));
      } else {
        REQUIRE(options.registry.add<VirtualList<int>>("Bound", [&](ui::SceneResourceArguments) {
          ++factories;
          return ui::Description<VirtualList<int>>{};
        }));
      }
      const auto failure = rejected("scene{VBox{Label{text=i18n(\"early\")} Bound{rowHeight=2}}}", options);

      CHECK(failure.sceneResource->code == (narrow ? Code::UNKNOWN_PROPERTY : Code::DUPLICATE_TYPE));
      CHECK(factories == 0);
      CHECK(resolutions == 0);
    }
  }
}

TEST_CASE("data scalar fluent modifiers preserve untouched origins and reset only replaced leaves") {
  auto origin = std::make_shared<ui::ControlSourceOrigin>();

  origin->sourceIdentity = "rows.scene";
  origin->byteOffset = 29;
  for (const bool replace : {false, true}) {
    auto recipe = replace ? ui::virtualList(ManagedModel<int>{}, ManagedListFormatter<int>{})
                              .settings({.origin = origin, .rowHeight = 1})
                              .rowHeight(0)
                          : ui::virtualList(ManagedModel<int>{}, ManagedListFormatter<int>{})
                              .settings({.origin = origin, .rowHeight = 0})
                              .overscanRows(0);
    auto owner = ui::scene(std::move(recipe));
    const auto prepared = owner.prepareChecked();

    REQUIRE_FALSE(prepared);
    REQUIRE(prepared.error().sceneResource);
    REQUIRE(prepared.error().sceneResource->propertyPath);
    CHECK(*prepared.error().sceneResource->propertyPath == "VirtualList.rowHeight");
    if (replace) {
      CHECK_FALSE(prepared.error().sceneResource->source);
    } else {
      REQUIRE(prepared.error().sceneResource->source);
      CHECK(*prepared.error().sceneResource->source == "rows.scene");
      CHECK(prepared.error().sceneResource->offset == 29);
    }
    REQUIRE(owner.close());
  }
}

TEST_CASE("derived virtual controls do not inherit the exact-family resource allowlist") {
  struct DerivedList : VirtualList<int> {
    using VirtualList<int>::VirtualList;
  };

  ui::SceneResourceOptions options;
  int factories{};

  options.format = ui::SceneResourceFormat::CONFIGURATION;
  REQUIRE(options.registry.add<DerivedList>("Derived", [&](ui::SceneResourceArguments) {
    ++factories;
    return ui::Description<DerivedList>{};
  }));
  const auto failure = rejected("scene{Derived{rowHeight=2}}", options);

  CHECK(failure.sceneResource->code == Code::UNKNOWN_PROPERTY);
  REQUIRE(failure.sceneResource->propertyPath);
  CHECK(*failure.sceneResource->propertyPath == "Derived.rowHeight");
  CHECK(factories == 0);
}

TEST_CASE("chart literals reject invalid reachable leaves before any resolver or code factory") {
  struct Scenario {
    std::string_view child;
    Code code;
    std::string_view path;
  };

  const Scenario cases[]{
    {"Prices{mode=AREA}", Code::INVALID_VALUE, "Prices.mode"},
    {"Prices{chartGlyphSet=UNICODE}", Code::INVALID_VALUE, "Prices.chartGlyphSet"},
    {"Prices{candleGap=-1}", Code::INVALID_VALUE, "Prices.candleGap"},
    {"Prices{candleGap=2147483648}", Code::INVALID_VALUE, "Prices.candleGap"},
    {"Prices{yRange{enabled=true minimum=4 maximum=3}}", Code::INVALID_VALUE, "Prices.yRange.maximum"},
    {"Prices{yRange{enabled=false minimum=nan}}", Code::INVALID_VALUE, "Prices.yRange.minimum"},
    {"Prices{yRange{maximum=inf}}", Code::INVALID_VALUE, "Prices.yRange.maximum"},
    {"Prices{yRange{minimum=1e309}}", Code::INVALID_VALUE, "Prices.yRange.minimum"},
    {"Prices{yRange{enabled=yes}}", Code::INVALID_VALUE, "Prices.yRange.enabled"},
    {"Prices{chartGlyphs=ASCII}", Code::UNKNOWN_PROPERTY, "Prices.chartGlyphs"},
    {"Prices{width=7}", Code::UNKNOWN_PROPERTY, "Prices.width"},
    {"Probe{mode=LINE}", Code::UNKNOWN_PROPERTY, "Probe.mode"},
    {"Chart{}", Code::UNKNOWN_TYPE, {}},
  };

  for (const auto &item : cases) {
    INFO("chart child: ", item.child);
    int factories{}, resolutions{};
    ui::SceneResourceOptions options;

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;

      return "early";
    };
    REQUIRE(options.registry.add<Chart<int>>("Bound", [&](ui::SceneResourceArguments) {
      ++factories;

      return ui::Description<Chart<int>>{};
    }));
    REQUIRE(options.registry.add<Plain>("Probe", [&](ui::SceneResourceArguments) {
      ++factories;

      return ui::custom<Plain>();
    }));
    const auto source =
      "definitions{Prices=Bound{}} scene{VBox{Label{text=i18n(\"early\")} Probe{} " + std::string(item.child) + "}}";
    const auto failure = rejected(source, options);

    CHECK(failure.sceneResource->code == item.code);
    if (!item.path.empty()) {
      REQUIRE(failure.sceneResource->propertyPath);
      CHECK(*failure.sceneResource->propertyPath == item.path);
    }
    CHECK(factories == 0);
    CHECK(resolutions == 0);
    CHECK(Plain::alive == 0);
  }
}

TEST_CASE("chart resource old narrow and nonexact bindings keep their existing allowlists") {
  for (const bool subclass : {false, true}) {
    int factories{}, resolutions{};
    ui::SceneResourceOptions options;

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.controls.emplace();
    if (!subclass) {
      constexpr std::string_view NARROW[]{"text"};

      options.controls->registerType("Bound", NARROW);
      REQUIRE(options.registry.add<Chart<int>>("Bound", [&](ui::SceneResourceArguments) {
        ++factories;

        return ui::Description<Chart<int>>{};
      }));
    } else {
      struct DerivedChart : Chart<int> {
        using Chart<int>::Chart;
      };

      REQUIRE(options.registry.add<DerivedChart>("Bound", [&](ui::SceneResourceArguments) {
        ++factories;

        return ui::Description<DerivedChart>{};
      }));
    }
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;

      return "early";
    };
    const auto failure = rejected("scene{VBox{Label{text=i18n(\"early\")} Bound{candleGap=1}}}", options);

    CHECK(failure.sceneResource->code == Code::UNKNOWN_PROPERTY);
    CHECK(factories == 0);
    CHECK(resolutions == 0);
  }
  ui::SceneResourceOptions incompatible;
  constexpr std::string_view PARTIAL[]{"text", "mode"};

  incompatible.format = ui::SceneResourceFormat::CONFIGURATION;
  incompatible.controls.emplace();
  incompatible.controls->registerType("Bound", PARTIAL);
  REQUIRE(incompatible.registry.add<Chart<int>>("Bound", [](ui::SceneResourceArguments) {
    return ui::Description<Chart<int>>{};
  }));
  CHECK(rejected("scene{Bound{}}", incompatible).sceneResource->code == Code::DUPLICATE_TYPE);
}

TEST_CASE("chart fluent provenance preserves untouched resource leaves and resets only replacements") {
  auto origin = std::make_shared<ui::ControlSourceOrigin>();

  origin->sourceIdentity = "chart.scene";
  origin->byteOffset = 37;
  for (const bool replace : {false, true}) {
    auto recipe = replace ? ui::chart(ManagedModel<int>{}, ManagedChartAccessor<int>{})
                              .settings({.origin = origin, .candleGap = 1})
                              .candleGap(-1)
                          : ui::chart(ManagedModel<int>{}, ManagedChartAccessor<int>{})
                              .settings({.origin = origin, .candleGap = -1})
                              .mode(ChartMode::LINE);
    auto owner = ui::scene(std::move(recipe));
    const auto prepared = owner.prepareChecked();

    REQUIRE_FALSE(prepared);
    REQUIRE(prepared.error().sceneResource);
    REQUIRE(prepared.error().sceneResource->propertyPath);
    CHECK(*prepared.error().sceneResource->propertyPath == "Chart.candleGap");
    if (replace) {
      CHECK_FALSE(prepared.error().sceneResource->source);
    } else {
      REQUIRE(prepared.error().sceneResource->source);
      CHECK(*prepared.error().sceneResource->source == "chart.scene");
      CHECK(prepared.error().sceneResource->offset == 37);
    }
    REQUIRE(owner.close());
  }
}

TEST_CASE("data-state configuration preflight rejects scalars schema and body count before reachable callbacks") {
  struct Scenario {
    std::string_view value;
    Code code;
    std::string_view path;
  };

  const Scenario cases[]{
    {"showStatus=false statusRows=0 Probe{}", Code::INVALID_VALUE, "DataStateView.statusRows"},
    {"statusRows=-1 Probe{}", Code::INVALID_VALUE, "DataStateView.statusRows"},
    {"statusRows=2147483648 Probe{}", Code::INVALID_VALUE, "DataStateView.statusRows"},
    {"spinnerInterval=0 Probe{}", Code::INVALID_VALUE, "DataStateView.spinnerInterval"},
    {"spinnerInterval=18446744073709551616 Probe{}", Code::INVALID_VALUE, "DataStateView.spinnerInterval"},
    {"contentPolicy=OTHER Probe{}", Code::INVALID_VALUE, "DataStateView.contentPolicy"},
    {"labels{unknown=\"not a label\"} Probe{}", Code::UNKNOWN_PROPERTY, "DataStateView.labels.unknown"},
    {"phase=READY Probe{}", Code::UNKNOWN_PROPERTY, "DataStateView.phase"},
    {"generation=1 Probe{}", Code::UNKNOWN_PROPERTY, "DataStateView.generation"},
    {"", Code::INVALID_VALUE, "DataStateView.children"},
    {"Probe{} Probe{}", Code::INVALID_VALUE, "DataStateView.children"},
  };

  for (const auto &item : cases) {
    INFO("data-state properties: ", item.value);
    int factories{}, resolutions{};
    ui::SceneResourceOptions options;

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    REQUIRE(options.registry.add<Plain>("Probe", [&](ui::SceneResourceArguments) {
      ++factories;

      return ui::custom<Plain>();
    }));
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;

      return "early";
    };
    const auto source = "scene{VBox{Label{text=i18n(\"early\")} DataStateView{" + std::string(item.value) + "}}}";
    const auto failure = rejected(source, options);

    CHECK(failure.sceneResource->code == item.code);
    REQUIRE(failure.sceneResource->propertyPath);
    CHECK(*failure.sceneResource->propertyPath == item.path);
    CHECK(factories == 0);
    CHECK(resolutions == 0);
    CHECK(Plain::alive == 0);
  }
}

TEST_CASE("data-state custom factories keep old allowlists and labels stay scoped") {
  ui::SceneResourceOptions options;
  int factories{}, resolutions{};

  options.format = ui::SceneResourceFormat::CONFIGURATION;
  REQUIRE(options.registry.add<DataStateView>("Custom", [&](ui::SceneResourceArguments) {
    ++factories;

    return ui::dataStateView(DataPresentation{}, ui::label("body"));
  }));
  options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
    ++resolutions;

    return "early";
  };
  const auto failure = rejected("scene{VBox{Label{text=i18n(\"early\")} Custom{showStatus=false}}}", options);

  CHECK(failure.sceneResource->code == Code::UNKNOWN_PROPERTY);
  CHECK(factories == 0);
  CHECK(resolutions == 0);
  ui::SceneResourceOptions legacy;

  CHECK(rejected("DataStateView{Label{\"body\"}}", legacy).sceneResource->code == Code::UNKNOWN_TYPE);
}

TEST_CASE("data-state high-word source leaves are charged before factories and not charged again on body mount") {
  for (const std::size_t budget : {1U, 2U}) {
    INFO("budget: ", budget);
    ui::SceneResourceOptions options;
    int factories{}, resolutions{};

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.controls.emplace(ui::ControlContextLimits{.maxMaterializedLeaves = budget});
    REQUIRE(options.registry.add<Widget>("Body", [&](ui::SceneResourceArguments) {
      ++factories;

      return ui::custom<Widget>();
    }));
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;

      return "unused";
    };
    auto owner = ui::scene("scene{DataStateView{showStatus=false labels{loading=i18n(\"unused\")} Body{}}}", options);
    const auto result = owner.prepareChecked();

    if (budget == 1) {
      REQUIRE_FALSE(result);
      REQUIRE(result.error().sceneResource);
      CHECK(result.error().sceneResource->code == Code::MATERIALIZATION_LIMIT);
      CHECK(factories == 0);
    } else {
      REQUIRE(result);
      CHECK(factories == 1);
    }
    CHECK(resolutions == 0);
    REQUIRE(owner.close());
  }
}

TEST_CASE("data-state compact patches preserve untouched source origins and replace their own scalar origin") {
  auto origin = std::make_shared<ui::ControlSourceOrigin>();

  origin->sourceIdentity = "state.scene";
  origin->byteOffset = 23;
  for (const bool replace : {false, true}) {
    auto recipe = replace ? ui::dataStateView(DataPresentation{}, ui::label("body"))
                              .settings({.origin = origin, .statusRows = 1})
                              .statusRows(0)
                          : ui::dataStateView(DataPresentation{}, ui::label("body"))
                              .settings({.origin = origin, .statusRows = 0})
                              .labels({.loading = text("Local")});
    auto owner = ui::scene(std::move(recipe));
    const auto result = owner.prepareChecked();

    REQUIRE_FALSE(result);
    REQUIRE(result.error().sceneResource);
    REQUIRE(result.error().sceneResource->propertyPath);
    CHECK(*result.error().sceneResource->propertyPath == "DataStateView.statusRows");
    if (replace) {
      CHECK_FALSE(result.error().sceneResource->source);
    } else {
      REQUIRE(result.error().sceneResource->source);
      CHECK(*result.error().sceneResource->source == "state.scene");
      CHECK(result.error().sceneResource->offset == 23);
    }
    REQUIRE(owner.close());
  }
}

TEST_CASE("native host and dialog whole-document structure is validated before factories and title resolution") {
  struct Scenario {
    std::string_view source;
    Code code;
    std::string_view path;
  };

  const Scenario cases[]{
    {"OverlayHost{maxLayers=0 Probe{}}", Code::INVALID_VALUE, "OverlayHost.maxLayers"},
    {"OverlayHost{maxLayers=18446744073709551616 Probe{}}", Code::INVALID_VALUE, "OverlayHost.maxLayers"},
    {"OverlayHost{}", Code::INVALID_VALUE, "OverlayHost.children"},
    {"OverlayHost{Dialog{title=\"body is a layer\" Probe{}}}", Code::INVALID_VALUE, "OverlayHost.children"},
    {"OverlayHost{Probe{} Probe{}}", Code::INVALID_VALUE, "OverlayHost.children"},
    {"OverlayHost{Probe{} Dialog{Probe{}}}", Code::INVALID_VALUE, "Dialog.title"},
    {"OverlayHost{Probe{} Dialog{title=\"zero\"}}", Code::INVALID_VALUE, "Dialog.children"},
    {"OverlayHost{Probe{} Dialog{title=\"two\" Probe{} Probe{}}}", Code::INVALID_VALUE, "Dialog.children"},
    {"Dialog{title=\"outside\" Probe{}}", Code::INVALID_VALUE, "Dialog.parent"},
    {"OverlayHost{maxLayers=1 Probe{} Dialog{title=\"one\" Probe{}} Dialog{title=\"two\" Probe{}}}", Code::NODE_LIMIT,
     "OverlayHost.maxLayers"},
    {"OverlayHost{Probe{} Dialog{title=\"wrong\" dialog{mode=LINE} Probe{}}}", Code::INVALID_VALUE,
     "Dialog.dialog.mode"},
    {"OverlayHost{Probe{} Dialog{title=\"wrong\" dialog{placement=OVERLAY} Probe{}}}", Code::INVALID_VALUE,
     "Dialog.dialog.placement"},
    {"OverlayHost{Probe{} Dialog{title=\"wrong\" dialog{maximumPanelSize{width=0}} Probe{}}}", Code::INVALID_VALUE,
     "Dialog.dialog.maximumPanelSize.width"},
    {"OverlayHost{Probe{} Dialog{title=\"wrong\" dialog{maximumPanelSize{height=2147483648}} Probe{}}}",
     Code::INVALID_VALUE, "Dialog.dialog.maximumPanelSize.height"},
    {"OverlayHost{Probe{} Dialog{title=\"wrong\" dialog{viewportMargin=-1} Probe{}}}", Code::INVALID_VALUE,
     "Dialog.dialog.viewportMargin"},
    {"OverlayHost{Probe{} Dialog{title=\"wrong\" dialog{anchorGap=-1} Probe{}}}", Code::INVALID_VALUE,
     "Dialog.dialog.anchorGap"},
    {"OverlayHost{Probe{} Dialog{title=\"wrong\" dialog{frame{enabled=false}} Probe{}}}", Code::UNKNOWN_PROPERTY,
     "Dialog.dialog.frame.enabled"},
    {"OverlayHost{Probe{} Dialog{title=\"wrong\" width=4 Probe{}}}", Code::UNKNOWN_PROPERTY, "Dialog.width"},
  };

  for (const auto &item : cases) {
    INFO(item.source);
    ui::SceneResourceOptions options;
    int factories{}, resolutions{};

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    REQUIRE(options.registry.add<Plain>("Probe", [&](ui::SceneResourceArguments) {
      ++factories;

      return ui::custom<Plain>();
    }));
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;

      return "early";
    };
    const auto failed = rejected("scene{VBox{Label{text=i18n(\"early\")} " + std::string(item.source) + "}}", options);

    CHECK(failed.sceneResource->code == item.code);
    REQUIRE(failed.sceneResource->propertyPath);
    CHECK(*failed.sceneResource->propertyPath == item.path);
    CHECK(factories == 0);
    CHECK(resolutions == 0);
    CHECK(Plain::alive == 0);
  }
}

TEST_CASE("resource dialog title is charged once while genuinely new internal Label defaults remain charged") {
  for (const std::size_t budget : {1U, 2U}) {
    for (const bool extraLabelDefault : {false, true}) {
      INFO("budget=", budget, ", extraLabelDefault=", extraLabelDefault);
      ui::SceneResourceOptions options;
      int factories{}, resolutions{};

      options.format = ui::SceneResourceFormat::CONFIGURATION;
      options.controls.emplace(ui::ControlContextLimits{.maxMaterializedLeaves = budget});
      if (extraLabelDefault) {
        options.controls->defaults("Label", {.width = 8});
      }
      REQUIRE(options.registry.add<Widget>("Body", [&](ui::SceneResourceArguments) {
        ++factories;

        return ui::custom<Widget>();
      }));
      options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
        ++resolutions;

        return "Caption";
      };
      auto owner = ui::scene("scene{OverlayHost{Body{} Dialog#layer{title=i18n(\"caption\") Body{}}}}", options);
      const auto layer = owner.get<Dialog>("layer");
      int inspected{};

      owner.configure([&](ManagedScene &, Application &app) {
        const auto panel = app.tree().inspect(layer.node()).value().children.front();
        const auto column = app.tree().inspect(panel).value().children.front();
        const auto heading = app.tree().inspect(column).value().children.front();

        REQUIRE(app.tree().read(heading, [&](const Widget &widget) {
          const auto &label = dynamic_cast<const Label &>(widget);

          CHECK(label.text().bytes() == "Caption");
          if (extraLabelDefault) {
            CHECK(label.sizePolicy().width.preferred == 8);
          }
          ++inspected;
        }));
      });
      const auto result = owner.prepareChecked();

      if (extraLabelDefault && budget == 1) {
        REQUIRE_FALSE(result);
        REQUIRE(result.error().sceneResource);
        CHECK(result.error().sceneResource->code == Code::MATERIALIZATION_LIMIT);
      } else {
        REQUIRE(result);
        CHECK(inspected == 1);
      }
      CHECK(factories == 2);
      CHECK(resolutions == 1);
      REQUIRE(owner.close());
    }
  }
}

TEST_CASE("Dialog scalar grammar does not widen existing custom registrations or reserve child names") {
  ui::SceneResourceOptions options;
  int factories{};

  options.format = ui::SceneResourceFormat::CONFIGURATION;
  REQUIRE(options.registry.add<Dialog>("Custom", [&](ui::SceneResourceArguments) {
    ++factories;

    return ui::dialog("Actual", ui::label("body"));
  }));
  const auto failed = rejected("scene{Custom{title=\"changed\"}}", options);

  CHECK(failed.sceneResource->code == Code::UNKNOWN_PROPERTY);
  CHECK(factories == 0);
  REQUIRE(options.registry.add<Label>("dialog", [&](ui::SceneResourceArguments) {
    ++factories;

    return ui::label("ordinary child");
  }));
  auto ordinary = ui::scene("scene{VBox{dialog{}}}", options);

  REQUIRE(ordinary.prepareChecked());
  CHECK(factories == 1);
  REQUIRE(ordinary.close());
  for (const bool explicitGroup : {false, true}) {
    const auto source = explicitGroup
                          ? "scene{OverlayHost{Label{} Dialog{title=\"Scope\" dialog={mode=NON_MODAL} dialog{}}}}"
                          : "scene{OverlayHost{Label{} Dialog{title=\"Scope\" dialog{}}}}";
    auto scoped = ui::scene(source, options);

    REQUIRE(scoped.prepareChecked());
    REQUIRE(scoped.close());
  }
  CHECK(factories == 3);
  auto emptyTitle = ui::scene("scene{OverlayHost{Label{} Dialog{title=\"\" Label{}}}}", options);

  REQUIRE(emptyTitle.prepareChecked());
  REQUIRE(emptyTitle.close());
  ui::SceneResourceOptions overrideOptions;

  overrideOptions.format = ui::SceneResourceFormat::CONFIGURATION;
  REQUIRE(overrideOptions.registry.add<Column>(
    "OverlayHost",
    [](ui::SceneResourceArguments) {
      return ui::column();
    },
    true));
  REQUIRE(overrideOptions.registry.add<Label>("Dialog", [](ui::SceneResourceArguments arguments) {
    return ui::label(arguments.text);
  }));
  auto overridden =
    ui::scene("scene{OverlayHost{Dialog{\"ordinary\"} Label{\"second ordinary child\"}}}", overrideOptions);

  REQUIRE(overridden.prepareChecked());
  REQUIRE(overridden.close());
  ui::SceneResourceOptions legacy;

  CHECK(rejected("OverlayHost{}", legacy).sceneResource->code == Code::UNKNOWN_TYPE);
}

TEST_CASE("scoped fallback title preserves typed text errors and does not resolve registered captions") {
  ui::SceneResourceOptions options;
  int factories{}, resolutions{};
  bool repaired{};

  options.format = ui::SceneResourceFormat::CONFIGURATION;
  REQUIRE(options.registry.add<Widget>("Body", [&](ui::SceneResourceArguments) {
    ++factories;

    return ui::custom<Widget>();
  }));
  options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
    ++resolutions;

    return repaired ? std::expected<std::string, TextResourceError>("Caption")
                    : std::unexpected(TextResourceError::MISSING_RESOURCE);
  };
  const auto source = "scene{OverlayHost{Body{} Dialog{title=i18n(\"private caption key\") Body{}}}}";
  const auto failed = rejected(source, options);

  CHECK(failed.resource == TextResourceError::MISSING_RESOURCE);
  REQUIRE(failed.sceneResource->propertyPath);
  CHECK(*failed.sceneResource->propertyPath == "Dialog.title");
  CHECK(factories == 0);
  CHECK(resolutions == 1);
  repaired = true;
  auto retry = ui::scene(source, options);

  REQUIRE(retry.prepareChecked());
  CHECK(factories == 2);
  CHECK(resolutions == 2);
  REQUIRE(retry.close());
  ui::SceneResourceOptions custom;

  custom.format = ui::SceneResourceFormat::CONFIGURATION;
  custom.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
    ++resolutions;

    return "must remain unused";
  };
  REQUIRE(custom.registry.add<Dialog>("Layer", [](ui::SceneResourceArguments) {
    return ui::dialog(i18n("own unavailable caption"), ui::label("body"));
  }));
  const auto before = resolutions;
  auto owner = ui::scene("scene{OverlayHost{Label{} Layer{}}}", custom);
  const auto actual = owner.prepareChecked();

  REQUIRE_FALSE(actual);
  CHECK(actual.error().resource == TextResourceError::RESOLVER_REQUIRED);
  CHECK(resolutions == before);
  REQUIRE(owner.close());

  auto native =
    ui::scene(ui::overlayHost(ui::label("body"), ui::dialog(i18n("own unavailable caption"), ui::label("body"))));
  const auto nativeResult = native.prepareChecked();

  REQUIRE_FALSE(nativeResult);
  CHECK(nativeResult.error().resource == TextResourceError::RESOLVER_REQUIRED);
  CHECK(actual.error().resource == nativeResult.error().resource);
  REQUIRE(native.close());
}

TEST_CASE("dialog compact title and option patches preserve per-leaf resource provenance") {
  auto origin = std::make_shared<ui::ControlSourceOrigin>();

  origin->sourceIdentity = "dialog.scene";
  origin->byteOffset = 31;
  for (const bool replace : {false, true}) {
    auto layer =
      replace
        ? ui::dialog("Actual", ui::label("body")).settings({.origin = origin, .dialog = {.anchorGap = 1}}).anchorGap(-1)
        : ui::dialog("Actual", ui::label("body"))
            .settings({.origin = origin, .dialog = {.anchorGap = -1}})
            .title("Local");
    auto owner = ui::scene(ui::overlayHost(ui::custom<Widget>(), std::move(layer)));
    const auto result = owner.prepareChecked();

    REQUIRE_FALSE(result);
    REQUIRE(result.error().sceneResource);
    REQUIRE(result.error().sceneResource->propertyPath);
    CHECK(*result.error().sceneResource->propertyPath == "Dialog.dialog.anchorGap");
    if (replace) {
      CHECK_FALSE(result.error().sceneResource->source);
    } else {
      REQUIRE(result.error().sceneResource->source);
      CHECK(*result.error().sceneResource->source == "dialog.scene");
      CHECK(result.error().sceneResource->offset == 31);
    }
    REQUIRE(owner.close());
  }
}

TEST_CASE("MessageBox semantic binding owns canonical metadata and preserves old narrow overloads") {
  ui::SceneResourceRegistry registry;
  auto plain = [](ui::SceneResourceArguments) {
    return ui::dialog("Plain", ui::label("body"));
  };

  REQUIRE(registry.add<Dialog>("OldBraces", plain, {}));
  REQUIRE(registry.add<Dialog>("OldBool", plain, false));
  CHECK(registry.add<Dialog>("Unknown", plain, ui::controlPropertySchema("Unknown")).error() == Code::INVALID_VALUE);
  CHECK(registry.add<Dialog>("Foreign", plain, ui::controlPropertySchema("Dialog")).error() == Code::INVALID_VALUE);
  auto canonical = ui::controlPropertySchema("MessageBox");
  auto temporary = ui::SceneResourceOptions{}.control<Dialog>("Temporary", plain, canonical);

  (void)temporary;
  std::vector<ui::ControlPropertyDescriptor> copy(canonical.begin(), canonical.end());

  REQUIRE(registry.add<Dialog>("Copied", plain, std::span<const ui::ControlPropertyDescriptor>(copy)));
  copy.front().depth = 99;
  CHECK(registry.add<Dialog>("Modified", plain, std::span<const ui::ControlPropertyDescriptor>(copy)).error() ==
        Code::INVALID_VALUE);
  CHECK(registry.add<Dialog>("Partial", plain, canonical.first(1)).error() == Code::INVALID_VALUE);
  ui::SceneResourceOptions options;
  int factories{};

  options.format = ui::SceneResourceFormat::CONFIGURATION;
  options.control<Dialog>("Old",
                          [&](ui::SceneResourceArguments) {
                            ++factories;

                            return ui::dialog("Plain", ui::label("body"));
                          },
                          {});
  const auto failure = rejected("scene{OverlayHost{Label{} Old{body=\"forbidden\"}}}", options);

  CHECK(failure.sceneResource->code == Code::UNKNOWN_PROPERTY);
  CHECK(factories == 0);
}

TEST_CASE("MessageBox semantic registration errors use the existing sticky runner boundary") {
  for (int error = 0; error < 3; ++error) {
    int factories{}, resolvers{}, reports{};
    ui::SceneResourceOptions options;
    const auto schema = ui::controlPropertySchema("MessageBox");
    auto factory = [&](ui::SceneResourceArguments) {
      ++factories;

      return ui::dialog("Wrong", ui::label("not reached"));
    };

    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolvers;

      return "not reached";
    };
    if (error == 0) {
      options.control<Dialog>("Duplicate", factory, schema).control<Dialog>("Duplicate", factory, schema);
    } else if (error == 1) {
      options.control<Dialog>("Foreign", factory, ui::controlPropertySchema("Dialog"));
    } else {
      bool reject = true;
      auto copies = std::make_shared<int>(0);
      RegistrationFactory throwing(copies, reject, true);

      options.control<Custom>("Allocation", throwing, schema);
    }
    const auto expected = error == 0 ? Code::DUPLICATE_TYPE : error == 1 ? Code::INVALID_VALUE : Code::NO_MEMORY;
    auto owner = ui::scene("this must not parse", options);

    owner.onError([&](const ManagedDiagnostic &diagnostic) {
      ++reports;
      REQUIRE(diagnostic.error.sceneResource);
      CHECK(diagnostic.error.sceneResource->code == expected);
      CHECK(diagnostic.error.sceneResource->line == 0);
    });
    example::ReplayTerminal terminal;

    CHECK(owner.runApp(terminal) == 1);
    CHECK(reports == 1);
    CHECK(factories == 0);
    CHECK(resolvers == 0);
    CHECK(terminal.output.empty());
    CHECK_FALSE(terminal.closed);
  }
}

TEST_CASE("MessageBox semantic schema rejects whole reachable invalid documents before callbacks") {
  struct Invalid {
    std::string_view source;
    Code code;
  };

  const Invalid cases[]{
    {R"(scene{VBox{Label{i18n("early")} Notice{message{defaultChoice{enabled=false value=INVALID}}}}})",
     Code::INVALID_VALUE},
    {R"(scene{VBox{Label{i18n("early")} Notice{message{textLimits{maxCells=0}}}}})", Code::INVALID_VALUE},
    {R"(scene{VBox{Label{i18n("early")} Notice{message{maxBodyLines=0}}}})", Code::INVALID_VALUE},
    {R"(scene{VBox{Label{i18n("early")} Notice{message{textLimits{maxInputBytes=18446744073709551616}}}}})",
     Code::INVALID_VALUE},
    {R"(scene{VBox{Label{i18n("early")} Notice{dialog{mode=NON_MODAL}}}})", Code::INVALID_VALUE},
    {R"(scene{VBox{Label{i18n("early")} Notice{dialog{closeOnEscape=false}}}})", Code::INVALID_VALUE},
    {R"(scene{VBox{Label{i18n("early")} Notice{message{unknown=1}}}})", Code::UNKNOWN_PROPERTY},
    {R"(scene{VBox{Label{i18n("early")} Notice{Label{"external child"}}}})", Code::CHILDREN_FORBIDDEN}};

  for (const auto &item : cases) {
    INFO(item.source);
    ui::SceneResourceOptions options;
    int factories{}, resolutions{};

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.control<Dialog>(
      "Notice",
      [&](ui::SceneResourceArguments) {
        ++factories;

        return ui::Description<Dialog>{};
      },
      ui::controlPropertySchema("MessageBox"));
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;

      return "early";
    };
    const auto failed = rejected(item.source, options);

    CHECK(failed.sceneResource->code == item.code);
    CHECK(factories == 0);
    CHECK(resolutions == 0);
  }
}

TEST_CASE("MessageBox binding verifies actual cold capability before mounting wrong composite") {
  for (const bool erased : {false, true}) {
    ui::SceneResourceOptions options;
    int factories{}, bodyCreates{};
    ui::Ref<Dialog> view;

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    auto factory = [&](ui::SceneResourceArguments) {
      ++factories;

      return ui::dialog("Plain", ui::custom<Widget>().setup(
                                   [&](ManagedScene &, Node<Widget>) -> std::expected<void, ManagedError> {
                                     ++bodyCreates;

                                     return {};
                                   }))
        .reference(view);
    };
    if (erased) {
      options.control<Widget>(
        "Wrong",
        [&](ui::SceneResourceArguments arguments) {
          return ui::detail::Access::erase(factory(std::move(arguments)));
        },
        ui::controlPropertySchema("MessageBox"));
    } else {
      options.control<Dialog>("Wrong", factory, ui::controlPropertySchema("MessageBox"));
    }
    auto owner = ui::scene("scene{OverlayHost{Label{} Wrong{body=\"must not be discarded\"}}}", options);
    const auto result = owner.prepareChecked();

    REQUIRE_FALSE(result);
    CHECK(cause(result.error(), TreeError::INVALID_ARGUMENT));
    REQUIRE(result.error().sceneResource);
    CHECK(factories == 1);
    CHECK(bodyCreates == 0);
    CHECK(view.node() == Node<Dialog>{});
    REQUIRE(owner.close());
  }
}

TEST_CASE("semantic MessageBox resolver fallback is active-only constructor scope and supports erased recipes") {
  for (const bool erased : {false, true}) {
    ui::Ref<MessageDialog> box;
    ui::Ref<Dialog> view;
    int calls{}, unused{}, factories{};
    bool repaired{};
    ui::SceneResourceOptions options;

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.resolver = [&](std::string_view key) -> std::expected<std::string, TextResourceError> {
      ++calls;
      if (key == "unused") {
        ++unused;
      }
      if (!repaired && key == "active") {
        return std::unexpected(TextResourceError::MISSING_RESOURCE);
      }

      return "Resolved";
    };
    auto factory = [&](ui::SceneResourceArguments) {
      ++factories;

      return ui::messageBox(box, "Native title", "Native body").reference(view);
    };
    if (erased) {
      options.control<Widget>(
        "Notice",
        [&](ui::SceneResourceArguments arguments) {
          return ui::detail::Access::erase(factory(std::move(arguments)));
        },
        ui::controlPropertySchema("MessageBox"));
    } else {
      options.control<Dialog>("Notice", factory, ui::controlPropertySchema("MessageBox"));
    }
    const auto source =
      R"(definitions{Inherited=Notice{message{labels{acknowledge=i18n("active") confirm=i18n("unused")}}}} scene{OverlayHost{Label{} Inherited{title="T" body="B"}}})";
    auto failed = ui::scene(source, options);
    const auto first = failed.prepareChecked();

    REQUIRE_FALSE(first);
    CHECK(first.error().resource == TextResourceError::MISSING_RESOURCE);
    CHECK(factories == 1);
    CHECK(calls == 1);
    CHECK(unused == 0);
    CHECK(view.node() == Node<Dialog>{});
    REQUIRE(failed.close());
    repaired = true;
    auto retry =
      ui::scene(source, options) | ui::style("Dialog.message-box.modal { bold:true; } Inherited { italic:true; }");
    Application *app{};
    ManagedScene *managed{};

    retry.configure([&](ManagedScene &owner, Application &application) {
      managed = &owner;
      app = &application;
    });
    REQUIRE(retry.prepareChecked());
    CHECK(calls == 2);
    CHECK(unused == 0);
    REQUIRE(app->tree().read(view.node(), [&](const Dialog &dialog) {
      REQUIRE(dialog.computedStyle());
      CHECK(dialog.computedStyle()->normal.bold);
      CHECK(dialog.computedStyle()->normal.italic);
      CHECK(dialog.options().mode == DialogMode::MODAL);
    }));
    REQUIRE(box
              .open([] {
              })
              .result());
    REQUIRE(box.cancel().result());
    REQUIRE(managed->afterDispatch(*app));
    REQUIRE(app->scheduler().dispatch());
    REQUIRE(managed->afterDispatch(*app));
    REQUIRE(retry.close());
    CHECK_FALSE(box.lastResult());
  }
}

TEST_CASE("semantic MessageBox source leaves charge once and preserve new factory and Label defaults") {
  for (const std::size_t budget : {2U, 3U, 4U}) {
    for (const bool factoryLeaf : {false, true}) {
      for (const bool labelDefault : {false, true}) {
        INFO("budget=", budget, ", factoryLeaf=", factoryLeaf, ", labelDefault=", labelDefault);
        ui::Ref<MessageDialog> box;
        ui::Ref<Dialog> view;
        ui::SceneResourceOptions options;
        int factories{}, inspected{};

        options.format = ui::SceneResourceFormat::CONFIGURATION;
        options.controls.emplace(ui::ControlContextLimits{.maxMaterializedLeaves = budget});
        if (labelDefault) {
          options.controls->defaults("Label", {.width = 8});
        }
        options.control<Widget>("Body", [](ui::SceneResourceArguments) {
          return ui::custom<Widget>();
        });
        options.control<Dialog>(
          "Notice",
          [&](ui::SceneResourceArguments) {
            ++factories;
            auto recipe = ui::messageBox(box, "Factory title", "Factory body").reference(view);

            if (factoryLeaf) {
              (void)std::move(recipe).messageSettings({.maxBodyLines = 6});
            }

            return recipe;
          },
          ui::controlPropertySchema("MessageBox"));
        auto owner = ui::scene("scene{OverlayHost{Body{} Notice{title=\"T\" body=\"B\"}}}", options);

        owner.configure([&](ManagedScene &, Application &app) {
          const auto panel = app.tree().inspect(view.node()).value().children.front();
          const auto column = app.tree().inspect(panel).value().children.front();
          const auto heading = app.tree().inspect(column).value().children.front();

          REQUIRE(app.tree().read(heading, [&](const Widget &widget) {
            const auto &label = dynamic_cast<const Label &>(widget);

            CHECK(label.text().bytes() == "T");
            if (labelDefault) {
              CHECK(label.sizePolicy().width.preferred == 8);
            }
            ++inspected;
          }));
        });
        const auto result = owner.prepareChecked();
        const auto required = 2U + static_cast<unsigned>(factoryLeaf) + static_cast<unsigned>(labelDefault);

        if (budget < required) {
          REQUIRE_FALSE(result);
          REQUIRE(result.error().sceneResource);
          CHECK(result.error().sceneResource->code == Code::MATERIALIZATION_LIMIT);
          CHECK(view.node() == Node<Dialog>{});
        } else {
          REQUIRE(result);
          CHECK(inspected == 1);
        }
        CHECK(factories == 1);
        REQUIRE(owner.close());
      }
    }
  }
}

TEST_CASE("MessageBox constructor resource resolver does not leak into ordinary custom Dialog siblings") {
  ui::Ref<MessageDialog> box;
  ui::Ref<Dialog> message, ordinary;
  ui::SceneResourceOptions options;
  int resolutions{}, factories{};

  options.format = ui::SceneResourceFormat::CONFIGURATION;
  options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
    ++resolutions;

    return "OK";
  };
  options.control<Dialog>(
    "Notice",
    [&](ui::SceneResourceArguments) {
      ++factories;
      ui::MessageOptions native;

      native.labels.acknowledge = i18n("message action");

      return ui::messageBox(box, "T", "B", native).reference(message);
    },
    ui::controlPropertySchema("MessageBox"));
  options.control<Dialog>("Plain", [&](ui::SceneResourceArguments) {
    ++factories;

    return ui::dialog(i18n("plain unavailable title"), ui::label("ordinary body")).reference(ordinary);
  });
  auto owner = ui::scene("scene{OverlayHost{Label{} Notice{} Plain{}}}", options);
  const auto result = owner.prepareChecked();

  REQUIRE_FALSE(result);
  CHECK(result.error().resource == TextResourceError::RESOLVER_REQUIRED);
  CHECK(factories == 2);
  CHECK(resolutions == 1);
  CHECK(message.node() == Node<Dialog>{});
  CHECK(ordinary.node() == Node<Dialog>{});
  REQUIRE(owner.close());
}

TEST_CASE("MessageBox packet merge retains factory-only origins and replaces source-present origins") {
  auto factoryOrigin = std::make_shared<ui::ControlSourceOrigin>();

  factoryOrigin->sourceIdentity = "factory.cfg";
  factoryOrigin->byteOffset = 41;
  factoryOrigin->line = 7;
  factoryOrigin->column = 3;
  for (int scenario = 0; scenario < 3; ++scenario) {
    INFO("scenario=", scenario);
    ui::Ref<MessageDialog> box;
    ui::Ref<Dialog> view;
    ui::SceneResourceOptions options;
    int factories{}, configured{};
    const std::string source = scenario == 0 ? "scene{OverlayHost{Label{} Notice{title=\"T\" body=\"B\"}}}"
                               : scenario == 1
                                 ? "scene{OverlayHost{Label{} Notice{title=\"T\" body=\"B\" message{maxBodyLines=3}}}}"
                                 : "scene{OverlayHost{Label{} Notice{title=\"T\" body=\"B\" message{maxBodyLines=0}}}}";

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.control<Dialog>(
      "Notice",
      [&](ui::SceneResourceArguments) {
        ++factories;
        auto recipe = ui::messageBox(box, "Factory title", "Factory body")
                        .settings({.origins = {{"message.maxBodyLines", factoryOrigin},
                                               {"message.textLimits.maxInputBytes", factoryOrigin}},
                                   .message = {.textLimits = {.maxInputBytes = 64}, .maxBodyLines = 0}})
                        .reference(view);

        return ui::detail::Access::controlSetup(
          std::move(recipe),
          [&](ManagedScene &, Node<Dialog>, const ui::ControlSettings &effective) -> std::expected<void, ManagedError> {
            ++configured;
            const auto winning =
              std::ranges::find(effective.origins, "message.maxBodyLines", &ui::ControlPropertyOrigin::path);
            const auto retained = std::ranges::find(effective.origins, "message.textLimits.maxInputBytes",
                                                    &ui::ControlPropertyOrigin::path);

            REQUIRE(winning != effective.origins.end());
            REQUIRE(winning->source);
            CHECK(winning->source != factoryOrigin);
            CHECK(winning->source->byteOffset == source.find("maxBodyLines"));
            CHECK(effective.message.maxBodyLines == 3);
            REQUIRE(retained != effective.origins.end());
            CHECK(retained->source == factoryOrigin);
            CHECK(effective.message.textLimits.maxInputBytes == 64);

            return {};
          });
      },
      ui::controlPropertySchema("MessageBox"));
    auto owner = ui::scene(source, options);
    const auto result = owner.prepareChecked();

    if (scenario == 1) {
      REQUIRE(result);
      CHECK(factories == 1);
      CHECK(configured == 1);
      REQUIRE(box
                .open([] {
                })
                .result());
      REQUIRE(box.cancel().result());
    } else {
      REQUIRE_FALSE(result);
      REQUIRE(result.error().sceneResource);
      const auto &diagnostic = *result.error().sceneResource;

      CHECK(diagnostic.code == Code::INVALID_VALUE);
      REQUIRE(diagnostic.propertyPath);
      CHECK(diagnostic.propertyPath->ends_with(".message.maxBodyLines"));
      REQUIRE(diagnostic.source);
      if (scenario == 0) {
        CHECK(*diagnostic.source == "factory.cfg");
        CHECK(diagnostic.offset == 41);
        CHECK(diagnostic.line == 7);
        CHECK(diagnostic.column == 3);
        CHECK(factories == 1);
      } else {
        CHECK(*diagnostic.source != "factory.cfg");
        CHECK(diagnostic.offset == source.find("maxBodyLines"));
        CHECK(diagnostic.line == 1);
        CHECK(diagnostic.column == diagnostic.offset + 1);
        CHECK(factories == 0);
      }
      CHECK(configured == 0);
      CHECK(view.node() == Node<Dialog>{});
    }
    REQUIRE(owner.close());
  }
}

TEST_CASE("Notifications semantic schema owns canonical metadata and keeps old narrow registrations") {
  ui::SceneResourceRegistry registry;
  auto plain = [](ui::SceneResourceArguments) {
    return ui::overlay(ui::custom<Widget>());
  };
  const auto canonical = ui::controlPropertySchema("Notifications");

  REQUIRE(canonical.size() == 25);
  REQUIRE(registry.add<Overlay>("OldBraces", plain, {}));
  REQUIRE(registry.add<Overlay>("OldBool", plain, false));
  REQUIRE(registry.add<Overlay>("Selected", plain, canonical));
  CHECK(registry.add<Overlay>("Unknown", plain, ui::controlPropertySchema("Unknown")).error() == Code::INVALID_VALUE);
  CHECK(registry.add<Overlay>("Foreign", plain, ui::controlPropertySchema("Overlay")).error() == Code::INVALID_VALUE);
  CHECK(registry.add<Overlay>("Partial", plain, canonical.first(1)).error() == Code::INVALID_VALUE);
  std::vector<ui::ControlPropertyDescriptor> copy(canonical.begin(), canonical.end());

  REQUIRE(registry.add<Overlay>("Copied", plain, std::span<const ui::ControlPropertyDescriptor>(copy)));
  copy.back().depth = 99;
  CHECK(registry.add<Overlay>("Modified", plain, std::span<const ui::ControlPropertyDescriptor>(copy)).error() ==
        Code::INVALID_VALUE);
  ui::SceneResourceOptions old;
  int factories{};

  old.format = ui::SceneResourceFormat::CONFIGURATION;
  old.control<Overlay>("Old",
                       [&](ui::SceneResourceArguments) {
                         ++factories;

                         return ui::overlay(ui::custom<Widget>());
                       },
                       {});
  CHECK(rejected("scene{Old{notification{capacity=4}}}", old).sceneResource->code == Code::CHILDREN_FORBIDDEN);
  CHECK(factories == 0);
  CHECK(rejected("scene{Notifications{}}", {.format = ui::SceneResourceFormat::CONFIGURATION}).sceneResource->code ==
        Code::UNKNOWN_TYPE);
  ui::SceneResourceOptions caller;

  caller.format = ui::SceneResourceFormat::CONFIGURATION;
  caller.control<Label>("Notifications", [](ui::SceneResourceArguments arguments) {
    return ui::label(arguments.text);
  });
  auto custom = ui::scene("scene{Notifications{text=\"caller wins\"}}", caller);

  REQUIRE(custom.prepareChecked());
  REQUIRE(custom.close());
}

TEST_CASE("Notifications semantic registration failures keep sticky runner collection") {
  for (int error = 0; error != 3; ++error) {
    ui::SceneResourceOptions options;
    int factories{}, resolvers{}, reports{};
    auto factory = [&](ui::SceneResourceArguments) {
      ++factories;

      return ui::overlay(ui::custom<Widget>());
    };
    const auto canonical = ui::controlPropertySchema("Notifications");

    if (error == 0) {
      options.control<Overlay>("Duplicate", factory, canonical).control<Overlay>("Duplicate", factory, canonical);
    } else if (error == 1) {
      options = ui::SceneResourceOptions{}.control<Overlay>("Foreign", factory, ui::controlPropertySchema("Overlay"));
    } else {
      bool reject = true;
      RegistrationFactory throwing(std::make_shared<int>(0), reject, true);

      options.control<Custom>("Allocation", throwing, canonical);
    }
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolvers;

      return "unused";
    };
    const auto expected = error == 0 ? Code::DUPLICATE_TYPE : error == 1 ? Code::INVALID_VALUE : Code::NO_MEMORY;
    auto owner = ui::scene("must fail before parsing", options);

    owner.onError([&](const ManagedDiagnostic &diagnostic) {
      ++reports;
      REQUIRE(diagnostic.error.sceneResource);
      CHECK(diagnostic.error.sceneResource->code == expected);
    });
    example::ReplayTerminal terminal;

    CHECK(owner.runApp(terminal) == 1);
    CHECK(reports == 1);
    CHECK(factories == 0);
    CHECK(resolvers == 0);
    CHECK(terminal.output.empty());
    CHECK_FALSE(terminal.closed);
  }
}

TEST_CASE("Notifications rejects reachable schema failures before factories and localization") {
  struct Invalid {
    std::string_view body;
    Code code;
  };

  const Invalid cases[]{{"notification{capacity=0}", Code::INVALID_VALUE},
                        {"notification{maxVisible=0}", Code::INVALID_VALUE},
                        {"notification{capacity=2 maxVisible=3}", Code::INVALID_VALUE},
                        {"notification{capacity=18446744073709551616}", Code::INVALID_VALUE},
                        {"notification{maximumWidth=0}", Code::INVALID_VALUE},
                        {"notification{maximumWidth=2147483648}", Code::INVALID_VALUE},
                        {"notification{margin=-1}", Code::INVALID_VALUE},
                        {"notification{gap=-1}", Code::INVALID_VALUE},
                        {"notification{placement=MODAL}", Code::INVALID_VALUE},
                        {"notification{overflow=SCROLL}", Code::INVALID_VALUE},
                        {"notification{textLimits{maxInputBytes=0}}", Code::INVALID_VALUE},
                        {"notification{textLimits{maxClusterBytes=0}}", Code::INVALID_VALUE},
                        {"notification{textLimits{maxGraphemes=0}}", Code::INVALID_VALUE},
                        {"notification{textLimits{maxCells=0}}", Code::INVALID_VALUE},
                        {"notification{actionLabelBytes=0}", Code::INVALID_VALUE},
                        {"notification{dedupKeyBytes=0}", Code::INVALID_VALUE},
                        {"notification{maxPaintCells=0}", Code::INVALID_VALUE},
                        {"notification{maxPaintCell=1}", Code::UNKNOWN_PROPERTY},
                        {"text=\"captured body stays code-owned\"", Code::UNKNOWN_PROPERTY},
                        {"title=\"not a caption\"", Code::UNKNOWN_PROPERTY},
                        {"Label{}", Code::CHILDREN_FORBIDDEN},
                        {"width=-1", Code::INVALID_VALUE}};

  for (const auto &item : cases) {
    INFO(item.body);
    ui::SceneResourceOptions options;
    int factories{}, resolutions{};

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.control<Overlay>(
      "Center",
      [&](ui::SceneResourceArguments) {
        ++factories;

        return ui::Description<Overlay>{};
      },
      ui::controlPropertySchema("Notifications"));
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;

      return "early";
    };
    const auto failure =
      rejected("scene{VBox{Label{text=i18n(\"early\")} Center{" + std::string(item.body) + "}}}", options);

    CHECK(failure.sceneResource->code == item.code);
    CHECK(factories == 0);
    CHECK(resolutions == 0);
  }
}

TEST_CASE("Notifications checks the actual semantic recipe before captured body callbacks") {
  for (int kind = 0; kind != 3; ++kind) {
    ui::NotificationCenter center;
    ui::Ref<MessageDialog> box;
    ui::Ref<Overlay> wrapper;
    ui::SceneResourceOptions options;
    int factories{}, bodyCreates{};

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.control<Widget>(
      "Bound",
      [&](ui::SceneResourceArguments) {
        ++factories;
        auto body = ui::custom<Widget>().setup([&](ManagedScene &, Node<Widget>) -> std::expected<void, ManagedError> {
          ++bodyCreates;

          return {};
        });

        if (kind == 0) {
          return ui::detail::Access::erase(ui::overlay(std::move(body)).reference(wrapper));
        }
        if (kind == 1) {
          return ui::detail::Access::erase(ui::messageBox(box, "T", "B"));
        }
        return ui::detail::Access::erase(ui::notifications(center, std::move(body)).reference(wrapper));
      },
      ui::controlPropertySchema(kind == 2 ? "MessageBox" : "Notifications"));
    auto owner = ui::scene("scene{Bound{}}", options);
    const auto result = owner.prepareChecked();

    REQUIRE_FALSE(result);
    CHECK(cause(result.error(), TreeError::INVALID_ARGUMENT));
    CHECK(factories == 1);
    CHECK(bodyCreates == 0);
    CHECK(wrapper.node() == Node<Overlay>{});
    CHECK_FALSE(center.snapshot());
    REQUIRE(owner.close());
  }
}

TEST_CASE("Notifications precharged third-word leaves preserve source priority and factory-only budgets") {
  for (const std::size_t budget : {3U, 4U}) {
    for (const bool extra : {false, true}) {
      INFO("budget=", budget, ", extra=", extra);
      ui::NotificationCenter center;
      ui::Ref<Overlay> wrapper;
      ui::SceneResourceOptions options;
      int factories{};

      options.format = ui::SceneResourceFormat::CONFIGURATION;
      options.controls.emplace(ui::ControlContextLimits{.maxMaterializedLeaves = budget});
      options.control<Overlay>(
        "Center",
        [&](ui::SceneResourceArguments) {
          ++factories;
          auto recipe = ui::notifications(center, ui::custom<Widget>())
                          .reference(wrapper)
                          .notificationSettings({.textLimits = {.maxCells = 90}});
          if (extra) {
            (void)std::move(recipe).notificationSettings({.dedupKeyBytes = 3});
          }
          return recipe;
        },
        ui::controlPropertySchema("Notifications"));
      auto owner =
        ui::scene("scene{Center{notification{textLimits{maxCells=8} actionLabelBytes=2 maxPaintCells=128}}}", options);
      const auto result = owner.prepareChecked();

      if (extra && budget == 3) {
        REQUIRE_FALSE(result);
        REQUIRE(result.error().sceneResource);
        CHECK(result.error().sceneResource->code == Code::MATERIALIZATION_LIMIT);
        CHECK(wrapper.node() == Node<Overlay>{});
        CHECK_FALSE(center.snapshot());
      } else {
        REQUIRE(result);
        const auto record = ui::detail::NotificationAccess::lock(center);

        REQUIRE(record);
        CHECK((*record)->options->textLimits.max_cells == 8);
        CHECK((*record)->options->actionLabelBytes == 2);
        CHECK((*record)->options->maxPaintCells == 128);
        CHECK((*record)->options->dedupKeyBytes == (extra ? 3 : 128));
        CHECK_FALSE(center.post(ui::toast("123456789")));
        CHECK_FALSE(center.post(ui::toast("x").action("123", [] {
        })));
        REQUIRE(center.snapshot());
        CHECK(center.snapshot().value()->items.empty());
      }
      CHECK(factories == 1);
      REQUIRE(owner.close());
    }
  }
}

TEST_CASE("Notifications source sizing wins once over typed and erased factory setup") {
  for (const bool erased : {false, true}) {
    ui::NotificationCenter center;
    ui::Ref<Overlay> wrapper;
    ui::SceneResourceOptions options;
    int factories{};

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.controls.emplace(ui::ControlContextLimits{.maxMaterializedLeaves = 2});
    auto factory = [&](ui::SceneResourceArguments) {
      ++factories;

      return ui::notifications(center, ui::custom<Widget>()).reference(wrapper).width(30);
    };
    if (erased) {
      options.control<Widget>(
        "Center",
        [&](ui::SceneResourceArguments args) {
          return ui::detail::Access::erase(factory(std::move(args)));
        },
        ui::controlPropertySchema("Notifications"));
    } else {
      options.control<Overlay>("Center", factory, ui::controlPropertySchema("Notifications"));
    }
    auto owner = ui::scene("scene{Center{width=12 notification{actionLabelBytes=2}}}", options);
    int inspected{};

    owner.configure([&](ManagedScene &, Application &app) {
      std::int32_t preferred{};
      REQUIRE(app.tree().read(wrapper.node(), [&](const Overlay &overlay) {
        preferred = overlay.sizePolicy().width.preferred;
      }));
      CHECK(preferred == 12);
      ++inspected;
    });
    REQUIRE(owner.prepareChecked());
    CHECK(factories == 1);
    CHECK(inspected == 1);
    REQUIRE(owner.close());
    CHECK_FALSE(center.snapshot());
  }
}

TEST_CASE("Notifications packet origins retain factory-only failures and replace winning source leaves") {
  auto origin = std::make_shared<ui::ControlSourceOrigin>();

  origin->sourceIdentity = "notification-factory.cfg";
  origin->byteOffset = 41;
  origin->line = 7;
  origin->column = 3;
  for (int scenario = 0; scenario != 3; ++scenario) {
    ui::NotificationCenter center;
    ui::SceneResourceOptions options;
    int factories{}, inspected{};
    const auto source = scenario == 0   ? "scene{Center{notification{margin=2}}}"
                        : scenario == 1 ? "scene{Center{notification{margin=2 actionLabelBytes=2}}}"
                                        : "scene{Center{notification{margin=2 actionLabelBytes=0}}}";

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.control<Overlay>(
      "Center",
      [&](ui::SceneResourceArguments) {
        ++factories;
        auto recipe =
          ui::notifications(center, ui::custom<Widget>())
            .settings({.origins = {{"notification.actionLabelBytes", origin}, {"notification.dedupKeyBytes", origin}},
                       .notification = {.actionLabelBytes = 0, .dedupKeyBytes = 5}});

        return ui::detail::Access::controlSetup(
          std::move(recipe),
          [&](ManagedScene &, Node<Overlay>,
              const ui::ControlSettings &effective) -> std::expected<void, ManagedError> {
            const auto winning =
              std::ranges::find(effective.origins, "notification.actionLabelBytes", &ui::ControlPropertyOrigin::path);
            const auto retained =
              std::ranges::find(effective.origins, "notification.dedupKeyBytes", &ui::ControlPropertyOrigin::path);

            REQUIRE(winning != effective.origins.end());
            CHECK(winning->source != origin);
            REQUIRE(retained != effective.origins.end());
            CHECK(retained->source == origin);
            ++inspected;

            return {};
          });
      },
      ui::controlPropertySchema("Notifications"));
    auto owner = ui::scene(source, options);
    const auto result = owner.prepareChecked();

    if (scenario == 1) {
      REQUIRE(result);
      CHECK(inspected == 1);
      CHECK_FALSE(center.post(ui::toast("x").action("123", [] {
      })));
    } else {
      REQUIRE_FALSE(result);
      REQUIRE(result.error().sceneResource);
      const auto &error = *result.error().sceneResource;

      CHECK(error.code == Code::INVALID_VALUE);
      REQUIRE(error.propertyPath);
      CHECK(error.propertyPath->ends_with(".notification.actionLabelBytes"));
      REQUIRE(error.source);
      if (scenario == 0) {
        CHECK(*error.source == "notification-factory.cfg");
        CHECK(error.offset == 41);
        CHECK(error.line == 7);
        CHECK(error.column == 3);
      } else {
        CHECK(*error.source != "notification-factory.cfg");
        CHECK(error.offset == std::string_view(source).find("actionLabelBytes"));
      }
      CHECK(inspected == 0);
      CHECK_FALSE(center.snapshot());
    }
    CHECK(factories == (scenario == 2 ? 0 : 1));
    REQUIRE(owner.close());
  }
}

TEST_CASE("Notifications never retains the prepare-only resource resolver for future posts") {
  for (const bool explicitResolver : {false, true}) {
    ui::NotificationCenter center;
    int resourceCalls{}, codeCalls{};
    auto token = std::make_shared<int>(1);
    std::weak_ptr<int> borrowedLifetime = token;
    {
      ui::SceneResourceOptions options;

      options.format = ui::SceneResourceFormat::CONFIGURATION;
      options.resolver = [token, &resourceCalls](std::string_view) -> std::expected<std::string, TextResourceError> {
        ++resourceCalls;

        return "resource";
      };
      options.control<Overlay>(
        "Center",
        [&](ui::SceneResourceArguments) {
          NotificationOptions captured;

          if (explicitResolver) {
            captured.textResolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
              ++codeCalls;

              return "own";
            };
          }

          return ui::notifications(center, ui::custom<Widget>(), captured);
        },
        ui::controlPropertySchema("Notifications"));
      auto owner = ui::scene("scene{Center{notification{maximumWidth=12}}}", std::move(options));

      token.reset();
      REQUIRE(owner.prepareChecked());
      const auto record = ui::detail::NotificationAccess::lock(center);

      REQUIRE(record);
      CHECK(bool((*record)->options->textResolver) == explicitResolver);
      const auto posted = center.post(ui::toast(i18n("after prepare")));

      if (explicitResolver) {
        REQUIRE(posted);
        CHECK(center.snapshot().value()->items.front().data->text.bytes() == "own");
        CHECK(codeCalls == 1);
      } else {
        REQUIRE_FALSE(posted);
        CHECK(posted.error().resource == TextResourceError::RESOLVER_REQUIRED);
        CHECK(codeCalls == 0);
      }
      CHECK(resourceCalls == 0);
      REQUIRE(owner.close());
      CHECK_FALSE(center.snapshot());
    }
    CHECK(borrowedLifetime.expired());
  }
}

TEST_CASE("Notifications captured cross-validation and invalid factory sizing roll back before retry") {
  for (const bool invalidSize : {false, true}) {
    ui::NotificationCenter center;
    ui::Ref<Overlay> wrapper;
    bool repaired{};
    int factories{};
    ui::SceneResourceOptions options;

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.control<Widget>(
      "Center",
      [&](ui::SceneResourceArguments) {
        ++factories;
        NotificationOptions captured;

        captured.maxVisible = 5;
        auto recipe = ui::notifications(center, ui::custom<Widget>(), captured).reference(wrapper);

        if (invalidSize && !repaired) {
          (void)std::move(recipe).width(-1);
        }
        return ui::detail::Access::erase(std::move(recipe));
      },
      ui::controlPropertySchema("Notifications"));
    const auto failing =
      invalidSize ? "scene{Center{notification{capacity=6}}}" : "scene{Center{notification{capacity=2}}}";
    auto failed = ui::scene(failing, options);
    const auto result = failed.prepareChecked();

    REQUIRE_FALSE(result);
    CHECK(cause(result.error(), TreeError::INVALID_ARGUMENT));
    CHECK(factories == 1);
    CHECK(wrapper.node() == Node<Overlay>{});
    REQUIRE_FALSE(center.snapshot());
    CHECK(cause(center.snapshot().error(), TreeError::CLOSED));
    REQUIRE(failed.close());
    repaired = true;
    auto retry = ui::scene("scene{Center{width=12 notification{capacity=6}}}", options);

    REQUIRE(retry.prepareChecked());
    CHECK(factories == 2);
    REQUIRE(center.post(ui::toast("Retry")));
    REQUIRE(retry.close());
    CHECK_FALSE(center.snapshot());
  }
}

TEST_CASE("Notifications inherited invalid limits fail before early reachable callbacks") {
  for (const bool instanceOverride : {false, true}) {
    ui::SceneResourceOptions options;
    int factories{}, resolutions{};

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.control<Overlay>(
      "Center",
      [&](ui::SceneResourceArguments) {
        ++factories;

        return ui::Description<Overlay>{};
      },
      ui::controlPropertySchema("Notifications"));
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;

      return "early";
    };
    const auto source =
      instanceOverride
        ? R"(defaults{Center{notification{capacity=2}}} definitions{Inbox=Center{notification{maxVisible=2}}} scene{VBox{Label{text=i18n("early")} Inbox{notification{maxVisible=3}}}})"
        : R"(definitions{Inbox=Center{notification{capacity=2 maxVisible=3}}} scene{VBox{Label{text=i18n("early")} Inbox{}}})";
    const auto failed = rejected(source, options);

    CHECK(failed.sceneResource->code == Code::INVALID_VALUE);
    CHECK(factories == 0);
    CHECK(resolutions == 0);
  }
}

TEST_CASE("Notifications property groups do not reserve a caller child type named notification") {
  ui::NotificationCenter center;
  ui::Ref<Label> child;
  ui::SceneResourceOptions options;
  int children{}, centers{};

  options.format = ui::SceneResourceFormat::CONFIGURATION;
  options.control<Label>("notification", [&](ui::SceneResourceArguments) {
    ++children;

    return ui::label("Child").reference(child);
  });
  options.control<Overlay>(
    "Center",
    [&](ui::SceneResourceArguments) {
      ++centers;

      return ui::notifications(center, ui::custom<Widget>());
    },
    ui::controlPropertySchema("Notifications"));
  auto owner = ui::scene("scene{VBox{notification{} Center{notification={capacity=4}}}}", options);

  REQUIRE(owner.prepareChecked());
  CHECK(children == 1);
  CHECK(centers == 1);
  CHECK(child.node() != Node<Label>{});
  const auto record = ui::detail::NotificationAccess::lock(center);

  REQUIRE(record);
  CHECK((*record)->options->capacity == 4);
  REQUIRE(owner.close());
  const auto failed = rejected("scene{Center{notification{}}}", options);

  CHECK(failed.sceneResource->code == Code::CHILDREN_FORBIDDEN);
  CHECK(children == 1);
  CHECK(centers == 1);
}

TEST_CASE("ThemeSelector canonical binding keeps old registrations and rejects wrong native capability") {
  ui::SceneResourceRegistry registry;
  auto plain = [](ui::SceneResourceArguments) {
    return ui::dialog("Plain", ui::label("Body"));
  };
  const auto canonical = ui::controlPropertySchema("ThemeSelector");

  REQUIRE(canonical.size() == 22);
  REQUIRE(registry.add<Dialog>("Old", plain, {}));
  REQUIRE(registry.add<Dialog>("Selected", plain, canonical));
  std::vector<ui::ControlPropertyDescriptor> copy(canonical.begin(), canonical.end());

  REQUIRE(registry.add<Dialog>("Copied", plain, std::span<const ui::ControlPropertyDescriptor>(copy)));
  copy.back().kind = ui::ControlPropertyKind::TEXT;
  CHECK(registry.add<Dialog>("Modified", plain, std::span<const ui::ControlPropertyDescriptor>(copy)).error() ==
        Code::INVALID_VALUE);
  CHECK(registry.add<Dialog>("Partial", plain, canonical.first(1)).error() == Code::INVALID_VALUE);
  CHECK(registry.add<Dialog>("Foreign", plain, ui::controlPropertySchema("Dialog")).error() == Code::INVALID_VALUE);
  CHECK(registry.add<Dialog>("Unknown", plain, ui::controlPropertySchema("Unknown")).error() == Code::INVALID_VALUE);
  CHECK(rejected("scene{ThemeSelector{}}", {.format = ui::SceneResourceFormat::CONFIGURATION}).sceneResource->code ==
        Code::UNKNOWN_TYPE);
  for (const bool selected : {false, true}) {
    ui::SceneResourceOptions options;
    int factories{};

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    auto factory = [&](ui::SceneResourceArguments) {
      ++factories;

      return ui::detail::Access::erase(plain({}));
    };
    if (selected) {
      options.control<Widget>("View", factory, canonical);
    } else {
      options.control<Widget>("View", factory, {});
    }
    auto owner = ui::scene(
      selected ? "scene{OverlayHost{Label{} View{}}}" : "scene{View{themeSelector={textLimits{maxCells=8}}}}", options);
    const auto result = owner.prepareChecked();

    REQUIRE_FALSE(result);
    REQUIRE(result.error().sceneResource);
    if (selected) {
      CHECK(cause(result.error(), TreeError::INVALID_ARGUMENT));
      CHECK(factories == 1);
    } else {
      CHECK(result.error().sceneResource->code == Code::UNKNOWN_PROPERTY);
      CHECK(factories == 0);
    }
    REQUIRE(owner.close());
  }
  ui::SceneResourceOptions caller;

  caller.format = ui::SceneResourceFormat::CONFIGURATION;
  caller.control<Label>("ThemeSelector", [](ui::SceneResourceArguments args) {
    return ui::label(args.text);
  });
  auto custom = ui::scene("scene{ThemeSelector{text=\"Caller wins\"}}", caller);

  REQUIRE(custom.prepareChecked());
  REQUIRE(custom.close());
  ui::SceneResourceOptions sticky;
  int factories{}, reports{};

  sticky
    .control<Dialog>(
      "Duplicate",
      [&](ui::SceneResourceArguments) {
        ++factories;

        return plain({});
      },
      canonical)
    .control<Dialog>("Duplicate", plain, canonical);
  auto owner = ui::scene("must not parse", sticky);

  owner.onError([&](const ManagedDiagnostic &failure) {
    ++reports;
    REQUIRE(failure.error.sceneResource);
    CHECK(failure.error.sceneResource->code == Code::DUPLICATE_TYPE);
  });
  example::ReplayTerminal terminal;

  CHECK(owner.runApp(terminal) == 1);
  CHECK(reports == 1);
  CHECK(factories == 0);
  CHECK(terminal.output.empty());
  CHECK_FALSE(terminal.closed);
}

TEST_CASE("ThemeSelector schema and shortcut syntax fail before early reachable callbacks") {
  struct Invalid {
    std::string_view body;
    Code code;
  };

  const Invalid cases[]{
    {"themeSelector{openingShortcut{enabled=false value=\"Ctrl+DefinitelyNotAKey\"}}", Code::INVALID_VALUE},
    {"themeSelector{openingShortcut{value=\"Ctrl+\"}}", Code::INVALID_VALUE},
    {"themeSelector{openingShortcut{value=99}}", Code::SYNTAX},
    {"themeSelector{textLimits{maxInputBytes=0}}", Code::INVALID_VALUE},
    {"themeSelector{textLimits{maxClusterBytes=0}}", Code::INVALID_VALUE},
    {"themeSelector{textLimits{maxGraphemes=0}}", Code::INVALID_VALUE},
    {"themeSelector{textLimits{maxCells=0}}", Code::INVALID_VALUE},
    {"themeSelector{textLimits{maxCells=18446744073709551616}}", Code::INVALID_VALUE},
    {"themeSelector{labels{aply=\"wrong\"}}", Code::UNKNOWN_PROPERTY},
    {"dialog{mode=NON_MODAL}", Code::INVALID_VALUE},
    {"dialog{maximumPanelSize{width=0}}", Code::INVALID_VALUE},
    {"width=8", Code::UNKNOWN_PROPERTY},
    {"body=\"not a custom body\"", Code::UNKNOWN_PROPERTY},
    {"Label{}", Code::CHILDREN_FORBIDDEN}};

  for (const auto &item : cases) {
    INFO(item.body);
    ui::SceneResourceOptions options;
    int factories{}, resolutions{};

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.control<Dialog>(
      "View",
      [&](ui::SceneResourceArguments) {
        ++factories;

        return ui::Description<Dialog>{};
      },
      ui::controlPropertySchema("ThemeSelector"));
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;

      return "early";
    };
    const auto failure =
      rejected("scene{VBox{Label{text=i18n(\"early\")} View{" + std::string(item.body) + "}}}", options);

    CHECK(failure.sceneResource->code == item.code);
    CHECK(factories == 0);
    CHECK(resolutions == 0);
  }
}

TEST_CASE(
  "ThemeSelector view source captions charge once across default catalog delegation and inherit local defaults") {
  for (const bool defaults : {false, true}) {
    for (const std::size_t budget : {6U, 7U, 13U, 14U}) {
      INFO("defaults=", defaults, ", budget=", budget);
      ui::ThemeSelectorRef selector;
      ui::Ref<Dialog> view;
      ui::SceneResourceOptions options;
      int factories{}, inspected{};

      options.format = ui::SceneResourceFormat::CONFIGURATION;
      options.controls.emplace(ui::ControlContextLimits{.maxMaterializedLeaves = budget});
      if (defaults) {
        options.controls->defaults("Label", {.width = 9});
        options.controls->defaults("Button", {.width = 11});
      }
      options.control<Widget>("Body", [](ui::SceneResourceArguments) {
        return ui::custom<Widget>();
      });
      options.control<Dialog>(
        "View",
        [&](ui::SceneResourceArguments) {
          ++factories;

          return ui::themeSelector(selector, ui::ThemeSelectorCatalog{.choices = {{"red", "Red",
                                                                                   [] {
                                                                                     return StyleSheet{};
                                                                                   }}}})
            .reference(view);
        },
        ui::controlPropertySchema("ThemeSelector"));
      auto owner = ui::scene(
        R"(scene{OverlayHost{Body{} View{title="T" themeSelector{labels{apply="A" cancel="C" closeKeepingPreview="K" emptyStatus="E" errorStatus="F" conflictStatus="S"}}}}})",
        options);

      owner.configure([&](ManagedScene &, Application &app) {
        const auto panel = app.tree().inspect(view.node()).value().children.front();
        const auto column = app.tree().inspect(panel).value().children.front();
        const auto heading = app.tree().inspect(column).value().children.front();
        std::int32_t width{};

        REQUIRE(app.tree().read(heading, [&](const Widget &widget) {
          width = dynamic_cast<const Label &>(widget).sizePolicy().width.preferred;
        }));
        if (defaults) {
          CHECK(width == 9);
        }
        ++inspected;
      });
      const auto result = owner.prepareChecked();
      const auto required = defaults ? 14U : 7U;

      if (budget < required) {
        REQUIRE_FALSE(result);
        REQUIRE(result.error().sceneResource);
        CHECK(result.error().sceneResource->code == Code::MATERIALIZATION_LIMIT);
        CHECK(view.node() == Node<Dialog>{});
        CHECK_FALSE(selector.open());
      } else {
        REQUIRE(result);
        CHECK(inspected == 1);
      }
      CHECK(factories == (budget < 7 ? 0 : 1));
      REQUIRE(owner.close());
    }
  }
}

TEST_CASE("ThemeSelector packet keeps winning source origins and clears only modified fluent leaves") {
  auto origin = std::make_shared<ui::ControlSourceOrigin>();

  origin->sourceIdentity = "selector-factory.cfg";
  origin->byteOffset = 41;
  origin->line = 7;
  origin->column = 3;
  for (int scenario = 0; scenario != 3; ++scenario) {
    ui::ThemeSelectorRef selector;
    ui::SceneResourceOptions options;
    int factories{}, inspected{};

    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.control<Dialog>(
      "View",
      [&](ui::SceneResourceArguments) {
        ++factories;
        ui::ThemeSelectorOptions captured;

        captured.title = captured.apply = captured.cancel = captured.closeKeepingPreview = TextValue("x");
        captured.emptyStatus = captured.errorStatus = captured.conflictStatus = TextValue("x");
        auto recipe =
          ui::themeSelector(selector,
                            ui::ThemeSelectorCatalog{.choices = {{"red", "Red",
                                                                  [] {
                                                                    return StyleSheet{};
                                                                  }}}},
                            captured)
            .settings(
              {.origins = {{"themeSelector.textLimits.maxInputBytes", origin}, {"themeSelector.labels.cancel", origin}},
               .themeSelector = {.labels = {.cancel = text("x")}, .textLimits = {.maxInputBytes = 0}}})
            .themeSelectorSettings({.labels = {.apply = text("A")}});

        return ui::detail::Access::controlSetup(
          std::move(recipe),
          [&](ManagedScene &, Node<Dialog>, const ui::ControlSettings &effective) -> std::expected<void, ManagedError> {
            const auto winning = std::ranges::find(effective.origins, "themeSelector.textLimits.maxInputBytes",
                                                   &ui::ControlPropertyOrigin::path);
            const auto retained =
              std::ranges::find(effective.origins, "themeSelector.labels.cancel", &ui::ControlPropertyOrigin::path);

            REQUIRE(winning != effective.origins.end());
            CHECK(winning->source != origin);
            REQUIRE(retained != effective.origins.end());
            CHECK(retained->source == origin);
            ++inspected;

            return {};
          });
      },
      ui::controlPropertySchema("ThemeSelector"));
    const auto source = scenario == 0 ? "scene{OverlayHost{Label{} View{title=\"T\"}}}"
                        : scenario == 1
                          ? "scene{OverlayHost{Label{} View{themeSelector{textLimits{maxInputBytes=2}}}}}"
                          : "scene{OverlayHost{Label{} View{themeSelector{textLimits{maxInputBytes=0}}}}}";
    auto owner = ui::scene(source, options);
    const auto result = owner.prepareChecked();

    if (scenario == 1) {
      REQUIRE(result);
      CHECK(inspected == 1);
    } else {
      REQUIRE_FALSE(result);
      REQUIRE(result.error().sceneResource);
      CHECK(result.error().sceneResource->code == Code::INVALID_VALUE);
      REQUIRE(result.error().sceneResource->source);
      if (scenario == 0) {
        CHECK(*result.error().sceneResource->source == "selector-factory.cfg");
        CHECK(result.error().sceneResource->offset == 41);
        CHECK(result.error().sceneResource->line == 7);
        CHECK(result.error().sceneResource->column == 3);
      } else {
        CHECK(*result.error().sceneResource->source != "selector-factory.cfg");
      }
      CHECK(inspected == 0);
    }
    CHECK(factories == (scenario == 2 ? 0 : 1));
    REQUIRE(owner.close());
  }
}

TEST_CASE("ThemeSelector fluent replacement clears its own origin without losing retained provenance") {
  auto origin = std::make_shared<ui::ControlSourceOrigin>();

  origin->sourceIdentity = "selector.cfg";
  origin->byteOffset = 41;
  for (const bool replace : {false, true}) {
    ui::ThemeSelectorRef selector;
    auto recipe = ui::themeSelector(selector, ui::ThemeSelectorCatalog{.choices = {{"red", "Red",
                                                                                    [] {
                                                                                      return StyleSheet{};
                                                                                    }}}})
                    .settings({.origins = {{"themeSelector.textLimits.maxInputBytes", origin}},
                               .themeSelector = {.textLimits = {.maxInputBytes = replace ? 64U : 0U}}});

    if (replace) {
      (void)std::move(recipe).themeSelectorSettings({.textLimits = {.maxInputBytes = 0}});
    } else {
      (void)std::move(recipe).themeSelectorSettings({.labels = {.apply = text("Use")}});
    }
    auto owner = ui::scene(ui::custom<Widget>(), std::move(recipe));
    const auto result = owner.prepareChecked();

    REQUIRE_FALSE(result);
    REQUIRE(result.error().sceneResource);
    CHECK(result.error().sceneResource->code == Code::INVALID_VALUE);
    if (replace) {
      CHECK_FALSE(result.error().sceneResource->source);
    } else {
      REQUIRE(result.error().sceneResource->source);
      CHECK(*result.error().sceneResource->source == "selector.cfg");
      CHECK(result.error().sceneResource->offset == 41);
    }
    REQUIRE(owner.close());
  }
}

TEST_CASE("ThemeSelector resource fallback resolver is not ambient to ordinary Dialog siblings") {
  ui::ThemeSelectorRef selector;
  ui::Ref<Dialog> view, plain;
  ui::SceneResourceOptions options;
  int calls{}, factories{};

  options.format = ui::SceneResourceFormat::CONFIGURATION;
  options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
    ++calls;

    return "Use";
  };
  options.control<Dialog>(
    "View",
    [&](ui::SceneResourceArguments) {
      ++factories;

      return ui::themeSelector(selector, ui::ThemeSelectorCatalog{.choices = {{"red", "Red",
                                                                               [] {
                                                                                 return StyleSheet{};
                                                                               }}}})
        .reference(view);
    },
    ui::controlPropertySchema("ThemeSelector"));
  options.control<Dialog>("Plain", [&](ui::SceneResourceArguments) {
    ++factories;

    return ui::dialog(i18n("ordinary unavailable title"), ui::custom<Widget>()).reference(plain);
  });
  auto owner =
    ui::scene(R"(scene{OverlayHost{Label{} View{themeSelector{labels{apply=i18n("apply")}}} Plain{}}})", options);
  const auto result = owner.prepareChecked();

  REQUIRE_FALSE(result);
  CHECK(result.error().resource == TextResourceError::RESOLVER_REQUIRED);
  CHECK(calls == 1);
  CHECK(factories == 2);
  CHECK(view.node() == Node<Dialog>{});
  CHECK(plain.node() == Node<Dialog>{});
  REQUIRE(owner.close());
}

TEST_CASE("actual catalog re-resolves original sparse view inputs with winning origins") {
  auto viewOrigin = std::make_shared<ui::ControlSourceOrigin>();
  auto catalogOrigin = std::make_shared<ui::ControlSourceOrigin>();
  viewOrigin->sourceIdentity = "view-default.cfg";
  catalogOrigin->sourceIdentity = "catalog-default.cfg";
  for (const bool explicitValue : {false, true}) {
    ui::ThemeSelectorRef selector;
    ui::Ref<Dialog> view;
    ui::SceneResourceOptions options;
    int factories{}, inspected{};
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.controls.emplace();
    options.controls->defaults("ThemeSelector",
                               {.origin = viewOrigin, .themeSelector = {.labels = {.apply = text("A")}}});
    options.controls->defaults(
      "ThemeSelectorCatalog",
      {.origin = catalogOrigin, .themeSelector = {.labels = {.apply = text("B")}}, .themeCatalog = {.maxChoices = 2}});
    options.control<Dialog>(
      "View",
      [&](ui::SceneResourceArguments args) {
        ++factories;
        if (explicitValue) {
          REQUIRE(args.settings.themeSelector.labels.apply);
          CHECK(args.settings.themeSelector.labels.apply->resolve({}).value().bytes() == "C");
        } else {
          // E12 registered View is an independent schema root in public factory arguments.
          CHECK_FALSE(args.settings.themeSelector.labels.apply);
        }
        CHECK_FALSE(args.settings.themeCatalog.maxChoices);
        auto recipe = ui::themeSelector(selector, ui::ThemeSelectorCatalog{.choices = {{"red", "Red",
                                                                                        [] {
                                                                                          return StyleSheet{};
                                                                                        }}}})
                        .themeSelectorSettings({.labels = {.cancel = text("Factory cancel")}})
                        .reference(view);
        return ui::detail::Access::controlSetup(
          std::move(recipe),
          [&](ManagedScene &, Node<Dialog>, const ui::ControlSettings &effective) -> std::expected<void, ManagedError> {
            REQUIRE(effective.themeSelector.labels.apply);
            REQUIRE(effective.themeSelector.labels.cancel);
            CHECK(effective.themeSelector.labels.apply->resolve({}).value().bytes() == (explicitValue ? "C" : "B"));
            CHECK(effective.themeSelector.labels.cancel->resolve({}).value().bytes() == "Factory cancel");
            CHECK(effective.themeCatalog.maxChoices == 2);
            const auto winning =
              std::ranges::find(effective.origins, "themeSelector.labels.apply", &ui::ControlPropertyOrigin::path);
            REQUIRE(winning != effective.origins.end());
            if (explicitValue) {
              REQUIRE(winning->source);
              CHECK(winning->source != catalogOrigin);
              CHECK(winning->source != viewOrigin);
            } else {
              CHECK(winning->source == catalogOrigin);
            }
            ++inspected;
            return {};
          });
      },
      ui::controlPropertySchema("ThemeSelector"));
    auto owner = ui::scene(
      explicitValue
        ? R"(definitions{Picker=View{}} scene{OverlayHost{Button{"Body"} Picker{themeSelector{labels{apply="C"}}}}})"
        : R"(definitions{Picker=View{}} scene{OverlayHost{Button{"Body"} Picker{}}})",
      options);
    const auto result = owner.prepareChecked();
    if (!result) {
      ui::reportToStderr({.failure = result.error()});
    }
    REQUIRE(result);
    CHECK(factories == 1);
    CHECK(inspected == 1);
    const auto published = view.node();
    CHECK(published != Node<Dialog>{});
    REQUIRE(owner.close());
    CHECK_FALSE(selector.snapshot());
    CHECK(view.node() == published);
    CHECK_FALSE(view.geometry());
  }
}

TEST_CASE("actual catalog reservation replaces one view leaf with two before constructor captions") {
  for (const std::size_t budget : {1U, 2U}) {
    INFO("budget=", budget);
    ui::ThemeSelectorRef selector;
    ui::Ref<Dialog> view;
    ui::SceneResourceOptions options;
    int factories{}, resolutions{}, previews{};
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.controls.emplace(ui::ControlContextLimits{.maxMaterializedLeaves = budget});
    options.controls->defaults("ThemeSelectorCatalog", {.themeCatalog = {.maxChoices = 1}});
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;
      return std::unexpected(TextResourceError::MISSING_RESOURCE);
    };
    options.control<Widget>("Body", [](ui::SceneResourceArguments) {
      return ui::custom<Widget>();
    });
    options.control<Dialog>(
      "View",
      [&](ui::SceneResourceArguments args) {
        ++factories;
        CHECK(ui::detail::controlSettingsPresenceSet(args.settings).count() == 1);
        ui::ThemeSelectorOptions captured;
        captured.title = i18n("constructor-boundary");
        return ui::themeSelector(selector,
                                 ui::ThemeSelectorCatalog{.choices = {{"red", "Red",
                                                                       [&] {
                                                                         ++previews;
                                                                         return StyleSheet{};
                                                                       }}}},
                                 captured)
          .reference(view);
      },
      ui::controlPropertySchema("ThemeSelector"));
    auto owner = ui::scene(R"(scene{OverlayHost{Body{} View{dialog{closeOnEscape=false}}}})", options);
    const auto result = owner.prepareChecked();
    REQUIRE_FALSE(result);
    CHECK(factories == 1);
    if (budget == 1) {
      REQUIRE(result.error().sceneResource);
      CHECK(result.error().sceneResource->code == Code::MATERIALIZATION_LIMIT);
      CHECK(resolutions == 0);
    } else {
      // Reaching the real constructor resolver proves replacement admitted exactly two, not old+new=three.
      CHECK(result.error().resource == TextResourceError::MISSING_RESOURCE);
      CHECK(resolutions == 1);
    }
    CHECK(previews == 0);
    CHECK_FALSE(selector.snapshot());
    CHECK(view.node() == Node<Dialog>{});
    REQUIRE(owner.close());
  }
}

TEST_CASE("ThemeSelectorCatalog numeric and narrow-schema errors precede all reachable callbacks") {
  const std::string_view invalid[]{"maxChoices=0",
                                   "maxIdBytes=0",
                                   "maxLabelBytes=0",
                                   "maxDescriptionBytes=0",
                                   "maxTextBytes=0",
                                   "textLimits{maxInputBytes=0}",
                                   "textLimits{maxClusterBytes=0}",
                                   "textLimits{maxGraphemes=0}",
                                   "textLimits{maxCells=0}",
                                   "layers{enabled=false maxLayers=1}",
                                   "layers{maxRules=0}",
                                   "layers{maxDeclarations=0}",
                                   "layers{maxOwnedBytes=0}",
                                   "maxChoices=18446744073709551616"};
  for (const auto body : invalid) {
    INFO(body);
    ui::SceneResourceOptions options;
    int factories{}, resolutions{};
    options.format = ui::SceneResourceFormat::CONFIGURATION;
    options.control<Dialog>(
      "Catalog",
      [&](ui::SceneResourceArguments) {
        ++factories;
        return ui::Description<Dialog>{};
      },
      ui::controlPropertySchema("ThemeSelectorCatalog"));
    options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      ++resolutions;
      return "early";
    };
    const auto failure =
      rejected("scene{VBox{Label{text=i18n(\"early\")} Catalog{themeCatalog{" + std::string(body) + "}}}}", options);
    CHECK(failure.sceneResource->code == Code::INVALID_VALUE);
    CHECK(factories == 0);
    CHECK(resolutions == 0);
  }
  ui::SceneResourceOptions narrow;
  int factories{};
  narrow.format = ui::SceneResourceFormat::CONFIGURATION;
  narrow.control<Dialog>(
    "View",
    [&](ui::SceneResourceArguments) {
      ++factories;
      return ui::Description<Dialog>{};
    },
    ui::controlPropertySchema("ThemeSelector"));
  CHECK(rejected("scene{View{themeCatalog={maxChoices=2}}}", narrow).sceneResource->code == Code::UNKNOWN_PROPERTY);
  CHECK(factories == 0);
}

TEST_CASE("catalog metadata requires owning capability and view metadata admits erased owning recipes") {
  for (const bool owning : {false, true}) {
    for (const bool catalogSchema : {false, true}) {
      INFO("owning=", owning, ", catalogSchema=", catalogSchema);
      ui::ThemeSelectorRef selector;
      ui::ThemeSelectionRef external;
      ui::Ref<Dialog> view;
      ui::SceneResourceOptions options;
      int factories{};
      options.format = ui::SceneResourceFormat::CONFIGURATION;
      options.control<Widget>(
        "Bound",
        [&](ui::SceneResourceArguments) {
          ++factories;
          auto recipe = owning
                          ? ui::themeSelector(selector, ui::ThemeSelectorCatalog{.choices = {{"red", "Red",
                                                                                              [] {
                                                                                                return StyleSheet{};
                                                                                              }}}})
                          : ui::themeSelector(selector, external);
          return ui::detail::Access::erase(std::move(recipe).reference(view));
        },
        ui::controlPropertySchema(catalogSchema ? "ThemeSelectorCatalog" : "ThemeSelector"));
      auto owner = ui::scene("scene{OverlayHost{Button{\"Body\"} Bound{}}}", options);
      if (!owning) {
        owner | ui::themeSelection(external, {{"red", "Red", [] {
                                                 return StyleSheet{};
                                               }}});
      }
      const auto result = owner.prepareChecked();
      if (owning || !catalogSchema) {
        REQUIRE(result);
        REQUIRE(selector.snapshot());
      } else {
        REQUIRE_FALSE(result);
        CHECK(cause(result.error(), TreeError::INVALID_ARGUMENT));
        CHECK_FALSE(selector.snapshot());
        CHECK(view.node() == Node<Dialog>{});
      }
      CHECK(factories == 1);
      REQUIRE(owner.close());
      CHECK_FALSE(selector.snapshot());
    }
  }
}
