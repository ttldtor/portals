// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/ui.hpp>
#include <portals/ui_scene_resource.hpp>

#include <atomic>
#include <cstdlib>
#include <limits>
#include <thread>

namespace {
std::atomic<std::ptrdiff_t> remaining{-1};
}

// This executable injects allocation failure only around the checked mount under test.
void *operator new(std::size_t size) {
  if (remaining == 0) {
    throw std::bad_alloc();
  }
  if (remaining > 0) {
    --remaining;
  }
  if (auto *memory = std::malloc(size ? size : 1)) {
    return memory;
  }
  throw std::bad_alloc();
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
  try {
    return ::operator new[](size);
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void *memory, const std::nothrow_t &) noexcept {
  std::free(memory);
}

void operator delete[](void *memory, const std::nothrow_t &) noexcept {
  std::free(memory);
}

namespace {
using namespace portals;
using namespace portals::ui;

template <class T, class E> T take(std::expected<T, E> result) {
  REQUIRE(result);
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));
};

bool cause(const ManagedError &error, TreeError expected) {
  const auto *value = std::get_if<TreeError>(&error.cause);
  return value && *value == expected;
}

void set(const Ref<LineEdit> &editor, std::string_view value) {
  take(editor.text().set(value).result());
}

void emptyRoot(Fixture &fixture) {
  CHECK(take(fixture.app->tree().inspect(fixture.scene->root())).children.empty());
}
} // namespace

TEST_CASE("context defaults and sparse false zero builtin-equal overrides preserve full-config semantics") {
  Fixture fixture;
  ControlContext controls;
  controls.defaults(
    "LineEdit",
    {.width = 17, .readOnly = true, .history = {.maxOperations = 0, .maxBytes = 2048, .groupTyping = false}});
  Ref<LineEdit> inherited, patched, full, fullPatched;
  LineEditConfig fullConfig;
  fullConfig.policy.width = {.preferred = 9, .mode = SizeMode::FIXED};
  take(mount(
    *fixture.scene,
    column(lineEdit(inherited, "default"),
           lineEdit(patched, "patch",
                    ControlSettings{.readOnly = false, .history = {.maxOperations = DEFAULT_TEXT_HISTORY_OPERATIONS}}),
           lineEdit(full, "legacy", fullConfig),
           lineEdit(fullPatched, "legacy", fullConfig)
             .settings({.text = text("final patch"), .readOnly = true, .history = {.enabled = false}})),
    controls));
  CHECK(inherited.clear().result() == std::unexpected(TreeError::WOULD_BLOCK));
  set(patched, "changed");
  CHECK(patched.canUndo());
  CHECK(full.clear());
  CHECK(full.canUndo());
  CHECK(take(fullPatched.text().get()) == "final patch");
  set(fullPatched, "programmatic");
  CHECK_FALSE(fullPatched.canUndo());
  CHECK(fullPatched.clear().result() == std::unexpected(TreeError::WOULD_BLOCK));
  take(fixture.app->tree().read(inherited.node(), [](const LineEdit &value) {
    CHECK(value.sizePolicy().width.preferred == 17);
  }));
  take(fixture.app->tree().read(full.node(), [](const LineEdit &value) {
    CHECK(value.sizePolicy().width.preferred == 9);
  }));
}

TEST_CASE("named editors share settings but retain independent histories and exact instance precedence") {
  Fixture fixture;
  ControlContext controls;
  controls.defaults("LineEdit", {.history = {.maxOperations = 5, .groupTyping = false}})
    .define("ParentEdit", "LineEdit", {.history = {.maxOperations = 1}})
    .define("MyEdit", "ParentEdit")
    .defaults("MyEdit", {.history = {.maxBytes = 1024}});
  Ref<LineEdit> first, second, override;
  take(mount(*fixture.scene,
             column(lineEdit(first, "A").variant("MyEdit"), lineEdit(second, "B").variant("MyEdit"),
                    lineEdit(override, "C").variant("MyEdit").settings({.history = {.maxOperations = 2}})),
             controls));
  set(first, "A1");
  set(first, "A2");
  set(override, "C1");
  set(override, "C2");
  REQUIRE(first.undo());
  CHECK(take(first.text().get()) == "A1");
  CHECK_FALSE(first.canUndo());
  CHECK(take(second.text().get()) == "B");
  CHECK_FALSE(second.canUndo());
  REQUIRE(override.undo());
  REQUIRE(override.undo());
  CHECK(take(override.text().get()) == "C");
  REQUIRE(first.redo());
  CHECK(take(first.text().get()) == "A2");
}

TEST_CASE("scene options snapshot reaches recipes and factory output without cross-scene mutation") {
  ControlContext original;
  original.define("MyEdit", "LineEdit", {.history = {.maxOperations = 0}})
    .defaults("LineEdit", {.text = text("inherited")});
  auto qualified = take(original.prepared());
  Ref<LineEdit> before, after;
  auto first = scene(lineEdit(before).variant("MyEdit"), {.controls = qualified});
  original.define("Other", "LineEdit");
  ControlContext different;
  different.define("MyEdit", "LineEdit", {.history = {.maxOperations = 2}});
  int factoryCalls{};
  auto second = scene(
    [&](ManagedScene &) {
      ++factoryCalls;
      return lineEdit(after, "second").variant("MyEdit");
    },
    {.controls = different});
  take(first.prepareChecked());
  take(second.prepareChecked());
  CHECK(factoryCalls == 1);
  CHECK(take(before.text().get()) == "inherited");
  CHECK(take(after.text().get()) == "second");
  set(before, "no history");
  set(after, "retained");
  CHECK_FALSE(before.canUndo());
  CHECK(after.canUndo());
  take(first.close());
  take(second.close());
}

TEST_CASE("typed variant identity preserves alias ancestors ID classes and ordinary style order") {
  Fixture fixture;
  ControlContext controls;
  controls.define("ParentEdit", "LineEdit").define("MyEdit", "ParentEdit").define("MyColumn", "VBox");
  Ref<LineEdit> classes, renamed;
  Ref<Column> layout;
  take(mount(*fixture.scene,
             column(layout, lineEdit(classes, "classes").variant("MyEdit").id("target").style({.classes = {"accent"}}),
                    lineEdit(renamed, "renamed").variant("MyEdit").style({.type = "VisualEdit", .classes = {"accent"}}))
               .variant("MyColumn")
               .id("layout")
               .style({.classes = {"layout"}}),
             controls));
  take(fixture.scene->theme(
    take(parsePss("Control { background: #010203; } MyEdit { italic: true; foreground: #000001; }"
                  "ParentEdit { underline: true; } LineEdit.accent { bold: true; foreground: #000002; }"
                  "VisualEdit { background: #040506; } #target { foreground: #000003; }"
                  "MyColumn { bold: true; } Column { italic: true; } VBox { underline: true; }"
                  "#layout { foreground: #040506; }"))));
  take(fixture.scene->prepare(*fixture.app, {50, 12}));
  take(fixture.app->tree().read(classes.node(), [](const LineEdit &value) {
    REQUIRE(value.computedStyle());
    const auto &style = value.computedStyle()->normal;
    CHECK(style.foreground == RgbColor{0, 0, 3});
    CHECK(style.background == RgbColor{1, 2, 3});
    CHECK(style.bold);
    CHECK(style.italic);
    CHECK(style.underline);
  }));
  take(fixture.app->tree().read(renamed.node(), [](const LineEdit &value) {
    REQUIRE(value.computedStyle());
    const auto &style = value.computedStyle()->normal;
    CHECK(style.foreground == RgbColor{0, 0, 2});
    CHECK(style.bold);
    CHECK(style.italic);
    CHECK(style.underline);
    CHECK(style.background == RgbColor{4, 5, 6});
  }));
  take(fixture.app->tree().read(layout.node(), [](const Column &value) {
    REQUIRE(value.computedStyle());
    const auto &style = value.computedStyle()->normal;
    CHECK(style.bold);
    CHECK(style.italic);
    CHECK(style.underline);
    CHECK(style.foreground == RgbColor{4, 5, 6});
  }));
}

TEST_CASE("whole context validation precedes resolvers user factories and node creation") {
  ControlContext invalid;
  invalid.define("CycleA", "CycleB").define("CycleB", "CycleA");
  Fixture fixture;
  Ref<LineEdit> reference;
  int resolved{};
  TextResolver resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
    ++resolved;
    return "resolved";
  };
  auto failed =
    mount(*fixture.scene, lineEdit(i18n("initial"), ControlSettings{}, resolver).reference(reference), invalid);
  REQUIRE_FALSE(failed);
  CHECK(failed.error().sceneResource->code == SceneResourceErrorCode::CYCLIC_TYPE);
  CHECK(resolved == 0);
  CHECK(reference.node() == Node<LineEdit>{});
  emptyRoot(fixture);
  int factories{};
  auto owner = scene(
    [&](ManagedScene &) {
      ++factories;
      return label("unused");
    },
    {.controls = invalid});
  CHECK_FALSE(owner.prepareChecked());
  CHECK(factories == 0);
}

TEST_CASE("typed variant rejects missing names and mismatched bases without publishing references") {
  Fixture fixture;
  ControlContext controls;
  controls.define("MyButton", "Button");
  Ref<LineEdit> reference;
  for (const auto name : {"Missing", "MyButton"}) {
    auto failed = mount(*fixture.scene, lineEdit(reference, "value").variant(name), controls);
    REQUIRE_FALSE(failed);
    CHECK(cause(failed.error(), TreeError::INVALID_ARGUMENT));
    CHECK(reference.node() == Node<LineEdit>{});
    emptyRoot(fixture);
  }
  auto unsupported = mount(*fixture.scene, custom<Column>().settings({.width = 1}), controls);
  REQUIRE_FALSE(unsupported);
  CHECK(cause(unsupported.error(), TreeError::INVALID_ARGUMENT));
  emptyRoot(fixture);
}

TEST_CASE("common defaults reach linear captions checkbox state and fixed ThreeState lineage") {
  Fixture fixture;
  ControlContext controls;
  controls.defaults("VBox", {.width = 20, .gap = 2})
    .defaults("Label", {.width = 7})
    .defaults("CheckBox", {.state = CheckState::MIXED, .cycle = CheckCycle::THREE_STATE})
    .define("MyThree", "ThreeState", {.state = CheckState::CHECKED});
  Ref<Column> layout, explicitLayout;
  Ref<Label> caption;
  Ref<CheckBox> checkboxRef;
  Ref<ThreeState> three;
  take(mount(*fixture.scene,
             column(layout, label(caption, "explicit"), checkbox(checkboxRef, "check"),
                    threeState(three, "three").variant("MyThree"), column(explicitLayout, LinearOptions{})),
             controls));
  take(fixture.app->tree().read(caption.node(), [](const Label &value) {
    CHECK(value.text().bytes() == "explicit");
    CHECK(value.sizePolicy().width.preferred == 7);
  }));
  take(fixture.app->tree().read(layout.node(), [](const Column &value) {
    CHECK(value.sizePolicy().width.preferred == 20);
    CHECK(value.options().gap == 2);
  }));
  take(fixture.app->tree().read(explicitLayout.node(), [](const Column &value) {
    CHECK(value.options().gap == 0);
  }));
  take(fixture.app->tree().read(checkboxRef.node(), [](const CheckBox &value) {
    CHECK(value.checkState() == CheckState::MIXED);
  }));
  take(fixture.app->tree().read(three.node(), [](const ThreeState &value) {
    CHECK(value.checkState() == CheckState::CHECKED);
  }));
  take(fixture.app->router().invoke(three.node(), {portals::Action::ACTIVATE}));
  take(fixture.app->tree().read(three.node(), [](const ThreeState &value) {
    CHECK(value.checkState() == CheckState::MIXED);
  }));
}

TEST_CASE("complete mount leaf budget includes nested constructor-generated children and checks overflow") {
  Fixture fixture;
  ControlContext controls({.maxMaterializedLeaves = 2});
  Ref<Label> first, generated;
  auto factory = ui::detail::Access::cold<Column>(
    [&](ManagedScene &owner, Node<Widget> parent,
        ui::detail::MountContext &context) -> std::expected<Node<Column>, ManagedError> {
      auto child =
        ui::detail::Access::mount(column(label(generated, "generated").settings({.width = 3})), owner, parent, context);
      if (!child) {
        return std::unexpected(child.error());
      }
      return *child;
    });
  auto failed = mount(*fixture.scene, column(label(first, "first"), std::move(factory)), controls);
  REQUIRE_FALSE(failed);
  REQUIRE(failed.error().sceneResource);
  CHECK(failed.error().sceneResource->code == SceneResourceErrorCode::MATERIALIZATION_LIMIT);
  CHECK(first.node() == Node<Label>{});
  CHECK(generated.node() == Node<Label>{});
  emptyRoot(fixture);

  auto overflow =
    ui::detail::Access::transaction([](ManagedScene &owner, Node<Widget> parent,
                                       ui::detail::MountContext &context) -> std::expected<Node<Widget>, ManagedError> {
      context.controlLeaves = std::numeric_limits<std::size_t>::max();
      auto child = ui::detail::Access::mount(label("bounded"), owner, parent, context);
      if (!child) {
        return std::unexpected(child.error());
      }
      return Node<Widget>(*child);
    });
  CHECK_FALSE(mount(*fixture.scene, std::move(overflow), controls));
  emptyRoot(fixture);
}

TEST_CASE("resource prepared marking is constructor-only and cannot hide generated child costs") {
  Fixture fixture;
  ControlContext controls({.maxMaterializedLeaves = 1});
  Ref<Label> first, second;
  auto recipe = ui::detail::Access::cold<Column>(
    [&](ManagedScene &owner, Node<Widget> parent,
        ui::detail::MountContext &context) -> std::expected<Node<Column>, ManagedError> {
      CHECK(context.controlPrepared);
      auto one = ui::detail::Access::mount(label(first, "one"), owner, parent, context);
      if (!one) {
        return std::unexpected(one.error());
      }
      CHECK(context.controlPrepared);
      auto two = ui::detail::Access::mount(label(second, "two"), owner, parent, context);
      if (!two) {
        return std::unexpected(two.error());
      }
      return owner.emplace<Column>(parent);
    });
  ui::detail::Access::preparedControl(recipe);
  auto failed = mount(*fixture.scene, std::move(recipe), controls);
  REQUIRE_FALSE(failed);
  CHECK(first.node() == Node<Label>{});
  CHECK(second.node() == Node<Label>{});
  emptyRoot(fixture);
}

TEST_CASE("variant mount allocation failures roll back tracked nodes and allow fresh recipes") {
  constexpr auto COUNT = std::numeric_limits<std::ptrdiff_t>::max();
  const std::string longName(180, 'A');
  ControlContext controls;
  controls.define(longName, "LineEdit", {.history = {.maxOperations = 2}});
  auto qualified = take(controls.prepared());
  std::ptrdiff_t required{};
  {
    Fixture fixture;
    Ref<LineEdit> editor;
    auto recipe = column(lineEdit(editor, "value").variant(longName).id("editor").style({.classes = {"accent"}}));
    remaining = COUNT;
    auto mounted = mount(*fixture.scene, std::move(recipe), qualified);
    required = COUNT - remaining.load();
    remaining = -1;
    REQUIRE(mounted);
  }
  REQUIRE(required > 0);
  std::size_t failures{}, successes{};
  const auto step = std::max<std::ptrdiff_t>(1, required / 8);
  for (std::ptrdiff_t budget = 0; budget <= required + step; budget += step) {
    Fixture fixture;
    Ref<LineEdit> editor;
    auto recipe = column(lineEdit(editor, "value").variant(longName).id("editor").style({.classes = {"accent"}}));
    remaining = budget;
    auto mounted = mount(*fixture.scene, std::move(recipe), qualified);
    remaining = -1;
    if (mounted) {
      ++successes;
      CHECK(take(editor.text().get()) == "value");
    } else {
      ++failures;
      const auto *styleError = std::get_if<StyleError>(&mounted.error().cause);
      const bool noMemory =
        cause(mounted.error(), TreeError::NO_MEMORY) || (styleError && styleError->cause == TreeError::NO_MEMORY);
      CHECK(noMemory);
      CHECK(editor.node() == Node<LineEdit>{});
      emptyRoot(fixture);
      take(mount(*fixture.scene, lineEdit(editor, "retry").variant(longName), qualified));
      CHECK(take(editor.text().get()) == "retry");
    }
  }
  CHECK(failures > 0);
  CHECK(successes > 0);
}

TEST_CASE("prepared custom resource factories retain explicit sparse settings and count only new leaves") {
  for (const int mode : {0, 1, 2}) {
    SceneResourceOptions options;
    if (mode != 0) {
      options.format = SceneResourceFormat::CONFIGURATION;
      options.controls = ControlContext({.maxMaterializedLeaves = mode == 1 ? 4U : 3U});
    }
    REQUIRE(options.registry.add<LineEdit>("FactoryEdit", [](SceneResourceArguments arguments) {
      LineEditConfig full;
      full.options.readOnly = true;
      full.options.history.maxOperations = 5;
      return lineEdit(arguments.text, full).settings({.width = 7, .readOnly = false, .history = {.maxOperations = 1}});
    }));
    const auto source = mode == 0 ? "FactoryEdit#editor{text=\"initial\",width=4}"
                                  : "scene{FactoryEdit#editor{text=\"initial\",width=4}}";
    auto owner = scene(source, options);
    auto editor = owner.get<LineEdit>("editor");
    auto prepared = owner.prepareChecked();
    if (mode == 2) {
      REQUIRE_FALSE(prepared);
      REQUIRE(prepared.error().sceneResource);
      CHECK(prepared.error().sceneResource->code == SceneResourceErrorCode::MATERIALIZATION_LIMIT);
      CHECK(editor.node() == Node<LineEdit>{});
    } else {
      REQUIRE(prepared);
      CHECK(editor.clear());
      set(editor, "one");
      set(editor, "two");
      REQUIRE(editor.undo());
      CHECK(take(editor.text().get()) == "one");
      CHECK_FALSE(editor.canUndo());
      auto managed = take(owner.managed());
      take(ui::detail::SceneAccess::application(managed.get()).tree().read(editor.node(), [](const LineEdit &value) {
        CHECK(value.sizePolicy().width.preferred == 4);
      }));
    }
    take(owner.close());
  }
}

TEST_CASE("prepared custom resource factories retain typed variant identity and its settings") {
  SceneResourceOptions options;
  options.format = SceneResourceFormat::CONFIGURATION;
  options.controls = ControlContext({.maxMaterializedLeaves = 5});
  options.controls->defaults("LineEdit", {.readOnly = true, .history = {.maxOperations = 0}})
    .define("NamedEdit", "LineEdit", {.history = {.maxOperations = 1, .groupTyping = false}});
  REQUIRE(options.registry.add<LineEdit>("FactoryEdit", [](SceneResourceArguments arguments) {
    return lineEdit(arguments.text).variant("NamedEdit").settings({.readOnly = false});
  }));
  auto owner = scene("scene{FactoryEdit#editor{text=\"initial\",width=4}}", options);
  auto editor = owner.get<LineEdit>("editor");
  take(owner.prepareChecked());
  CHECK(editor.clear());
  set(editor, "one");
  set(editor, "two");
  REQUIRE(editor.undo());
  CHECK(take(editor.text().get()) == "one");
  CHECK_FALSE(editor.canUndo());
  auto managed = take(owner.managed());
  auto &application = ui::detail::SceneAccess::application(managed.get());
  take(managed.get().theme(take(parsePss("NamedEdit { italic: true; } FactoryEdit { bold: true; }"
                                         "LineEdit { underline: true; } #editor { foreground: #010203; }"))));
  take(managed.get().prepare(application, {40, 10}));
  take(application.tree().read(editor.node(), [](const LineEdit &value) {
    REQUIRE(value.computedStyle());
    const auto &style = value.computedStyle()->normal;
    CHECK(style.bold);
    CHECK(style.italic);
    CHECK(style.underline);
    CHECK(style.foreground == RgbColor{1, 2, 3});
  }));
  take(owner.close());
}

TEST_CASE("explicit full editor options never count discarded defaults against an instance patch") {
  for (const bool variant : {false, true}) {
    Fixture fixture;
    ControlContext controls({.maxMaterializedLeaves = 1});
    controls.defaults("LineEdit", {.readOnly = true}).define("Edit", "LineEdit");
    Ref<LineEdit> field;
    auto recipe = lineEdit(field, "value", LineEditConfig{}).settings({.history = {.maxOperations = 1}});

    if (variant) {
      recipe = std::move(recipe).variant("Edit");
    }

    take(mount(*fixture.scene, std::move(recipe), controls));
    CHECK(field.clear());
    set(field, "one");
    set(field, "two");
    take(field.undo().result());
    CHECK(take(field.text().get()) == "one");
    CHECK_FALSE(field.canUndo());
  }
}
