// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

#include <array>
#include <cstdlib>
#include <iostream>
#include <new>
#include <source_location>
#include <stdexcept>
#include <thread>

namespace allocation_test {
thread_local std::ptrdiff_t remaining = -1;
thread_local const void *reclaimTarget{};
thread_local std::function<void()> reclaimAction;

void release(void *memory) noexcept {
  if (memory && memory == reclaimTarget) {
    reclaimTarget = nullptr;
    auto action = std::move(reclaimAction);
    if (action) {
      action();
    }
  }
  std::free(memory);
}
} // namespace allocation_test

void *operator new(std::size_t bytes) {
  if (allocation_test::remaining == 0) {
    throw std::bad_alloc();
  }
  if (allocation_test::remaining > 0) {
    --allocation_test::remaining;
  }
  if (auto memory = std::malloc(bytes ? bytes : 1)) {
    return memory;
  }
  throw std::bad_alloc();
}

void operator delete(void *memory) noexcept {
  allocation_test::release(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
  allocation_test::release(memory);
}

void *operator new[](std::size_t bytes) {
  return ::operator new(bytes);
}

void operator delete[](void *memory) noexcept {
  allocation_test::release(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
  allocation_test::release(memory);
}

using namespace portals;

namespace {
template <class T, class E>
T take(std::expected<T, E> result, std::source_location location = std::source_location::current()) {
  if (!result) {
    std::cerr << "checked failure at " << location.file_name() << ':' << location.line() << '\n';
    if constexpr (std::same_as<E, ManagedError>) {
      ui::reportToStderr(ui::SceneReport{.failure = result.error()});
    } else if constexpr (std::is_enum_v<E>) {
      std::cerr << "error=" << static_cast<int>(result.error()) << '\n';
    }
  }
  REQUIRE(result.has_value());
  return std::move(*result);
}

template <class E>
void take(std::expected<void, E> result, std::source_location location = std::source_location::current()) {
  if (!result) {
    std::cerr << "checked failure at " << location.file_name() << ':' << location.line() << '\n';
    if constexpr (std::same_as<E, ManagedError>) {
      ui::reportToStderr(ui::SceneReport{.failure = result.error()});
    } else if constexpr (std::is_enum_v<E>) {
      std::cerr << "error=" << static_cast<int>(result.error()) << '\n';
    }
  }
  REQUIRE(result.has_value());
}

RadioGroupValue choices(std::optional<RowId> selected = RowId{11, 1}) {
  const std::array rows{ModelRow<RadioItem>{{11, 1}, {take(GraphemeText::from("Fast"))}},
                        ModelRow<RadioItem>{{22, 1}, {take(GraphemeText::from("Disabled")), false}},
                        ModelRow<RadioItem>{{33, 1}, {take(GraphemeText::from("Hidden")), true, false}},
                        ModelRow<RadioItem>{{44, 1}, {take(GraphemeText::from("Quiet"))}}};

  return take(RadioGroupValue::from(take(RowCollection<RadioItem>::from(rows)), selected));
}

struct Fixture {
  std::unique_ptr<Scheduler> scheduler;
  std::unique_ptr<WidgetTree> tree;
  Node<Widget> root;
  std::unique_ptr<EventRouter> router;
  Node<RadioGroup> node;
  std::shared_ptr<Property<RadioGroupValue>> model;

  explicit Fixture(bool readOnly = false, SchedulerOptions options = {})
      : scheduler(take(Scheduler::create(options))), tree(take(WidgetTree::create(*scheduler))),
        root(take(tree->root())), router(take(EventRouter::create(*tree))) {
    node = take(tree->emplace<RadioGroup>(root, ControlStyle{}, DEFAULT_CONTENT_POLICY, readOnly));
    model = take(Property<RadioGroupValue>::create(*tree, root, choices(), Invalidation::PAINT));
  }
};
} // namespace

TEST_CASE("radio values preserve stable identities and share row storage across selection") {
  auto original = choices();
  const auto changed = take(original.withSelection(RowId{44, 1}));

  CHECK(original.selectedText() == "Fast");
  CHECK(changed.selectedText() == "Quiet");
  CHECK(original.rows().data() == changed.rows().data());
  const auto transferred = std::move(original);
  CHECK(transferred.selectedText() == "Fast");
  CHECK(original.selectedText() == "Fast");
  CHECK(original.rows().data() == transferred.rows().data());
  CHECK_FALSE(original.withSelection(RowId{44, 2}));
  CHECK(take(original.withSelection(RowId{22, 1})).selectedText() == "Disabled");
  CHECK(take(original.withSelection(RowId{33, 1})).selectedText() == "Hidden");
  CHECK_FALSE(take(original.withSelection({})).selectedId());
  CHECK_FALSE(RadioGroupValue{}.firstEligible());

  const std::array reordered{original.rows()[3], original.rows()[0]};
  const auto moved = take(RadioGroupValue::from(take(RowCollection<RadioItem>::from(reordered)), RowId{44, 1}));

  CHECK(moved.selectedText() == "Quiet");
  CHECK_FALSE(RadioGroupValue::from(take(RowCollection<RadioItem>::from(reordered)), RowId{22, 1}));
}

TEST_CASE("radio binding navigates eligible IDs and preserves exact commit receipts") {
  Fixture f;
  std::optional<RadioGroupResult> last;
  int calls{};
  auto binding = take(bindRadioGroup(*f.tree, *f.router, f.node, f.model, [&](RadioGroupResult result) {
    ++calls;
    last = result;
  }));

  take(f.tree->consumeFrameRequest());
  take(f.router->invoke(f.node, ActionEvent{Action::MOVE_DOWN}));
  REQUIRE(last);
  REQUIRE(*last);
  CHECK((*last)->changed);
  CHECK((*last)->snapshot->value.selectedId() == RowId{44, 1});
  CHECK(take(binding.value()).selectedText() == "Quiet");
  take(f.tree->consumeFrameRequest());
  take(f.router->invoke(f.node, ActionEvent{Action::ACTIVATE}));
  CHECK_FALSE((*last)->changed);
  CHECK(take(f.tree->consumeFrameRequest()) == Invalidation::NONE);
  take(f.router->invoke(f.node, ActionEvent{Action::MOVE_DOWN}));
  CHECK((*last)->snapshot->value.selectedId() == RowId{11, 1});
  take(binding.select({}));
  take(f.router->invoke(f.node, ActionEvent{Action::MOVE_DOWN}));
  CHECK((*last)->snapshot->value.selectedId() == RowId{11, 1});
  CHECK(calls == 4);

  auto foreign = take(f.tree->emplace<RadioGroup>(f.root));
  CHECK_FALSE(bindRadioGroup(*f.tree, *f.router, f.node, f.model, [](auto) {
  }));
  auto second = take(bindRadioGroup(*f.tree, *f.router, foreign, f.model, [](auto) {
  }));
  take(binding.select(RowId{44, 1}));
  take(f.scheduler->dispatch());
  take(f.tree->read(foreign, [](const RadioGroup &group) {
    CHECK(group.value().selectedText() == "Quiet");
  }));
}

TEST_CASE("readOnly radio rejects user mutation but accepts programmatic selection") {
  Fixture f(true);
  int calls{};
  auto binding = take(bindRadioGroup(*f.tree, *f.router, f.node, f.model, [&](auto) {
    ++calls;
  }));

  take(f.router->invoke(f.node, ActionEvent{Action::MOVE_DOWN}));
  CHECK(calls == 0);
  CHECK(take(binding.value()).selectedText() == "Fast");
  CHECK(take(binding.select(RowId{33, 1})).changed);
  CHECK(take(binding.value()).selectedText() == "Hidden");
  CHECK(calls == 0);
}

TEST_CASE("radio admitted callback preserves its receipt through reentry and removal") {
  Fixture f;
  std::optional<RadioGroupBinding> binding;
  int calls{};

  binding.emplace(take(bindRadioGroup(*f.tree, *f.router, f.node, f.model, [&](RadioGroupResult result) {
    REQUIRE(result);
    ++calls;
    CHECK(result->snapshot->value.selectedText() == "Quiet");
    take(f.model->set(choices(RowId{11, 1})));
    take(f.tree->remove(f.node));
    binding.reset();
    CHECK(result->snapshot->value.selectedText() == "Quiet");
  })));
  take(f.router->invoke(f.node, ActionEvent{Action::MOVE_END}));
  CHECK(calls == 1);
  CHECK(take(f.model->snapshot())->value.selectedText() == "Fast");
}

TEST_CASE("radio scene shorthand and model client use unified Ref and user-only onChange") {
  ui::Ref<RadioGroup> group;
  ManagedState<RadioGroupValue> model;
  Application *application{};
  int calls{};
  auto scene = ui::scene([&](ManagedScene &owner, Application &app) {
                 application = &app;
                 model = take(owner.radioState({"Fast", "Balanced", "Quiet"}));
                 return ui::radioGroup(group, model).id("mode").classes({"settings"});
               }) |
               ui::onChange(group, [&](const RadioGroupValue &value) {
                 ++calls;
                 CHECK(value.selectedText() == "Balanced");
               });

  scene.prepare();
  take(scene.checked());
  CHECK(take(group.value()).selectedText() == "Fast");
  take(application->router().invoke(group.node(), ActionEvent{Action::MOVE_DOWN}));
  CHECK(calls == 1);
  take(group.select(RowId{3, 1}));
  CHECK(calls == 1);
  CHECK(take(group.value()).selectedText() == "Quiet");
  CHECK(take(model.snapshot())->value.selectedText() == "Quiet");

  ui::Ref<RadioGroup> simple;
  auto other = ui::scene(ui::radioGroup(simple, {"One", "Two"}));
  other.prepare();
  take(other.checked());
  CHECK(take(simple.value()).selectedText() == "One");
}

TEST_CASE("radio caption ingress uses actual scene policy and retains typed failures") {
  ui::Ref<RadioGroup> group;
  ui::SceneOptions options;
  options.application.widthPolicy.ambiguous = AmbiguousWidth::WIDE;
  options.application.widthPolicy.revision = 7;
  auto scene = ui::scene(ui::radioGroup(std::vector<TextValue>{TextValue("\xe7\x95\x8c"), i18n("quiet")},
                                        [](std::string_view) -> std::expected<std::string, TextResourceError> {
                                          return "Quiet";
                                        })
                           .reference(group),
                         options);
  scene.prepare();
  take(scene.checked());
  CHECK(take(group.value()).selectedText() == "\xe7\x95\x8c");
  CHECK(take(group.value()).rows().front().value.text.policy() == options.application.widthPolicy);

  auto invalid = ui::scene(ui::radioGroup({std::string_view("\xff", 1)}));
  invalid.prepare();
  CHECK_FALSE(invalid.checked());
}

TEST_CASE("pointer admission preserves explicit identity across a source reorder") {
  Fixture f;
  int calls{};
  auto binding = take(bindRadioGroup(*f.tree, *f.router, f.node, f.model, [&](auto) {
    ++calls;
  }));
  const auto area = take(CellRect::from({}, {20, 3}));

  take(f.tree->frame([&] {
    take(f.tree->arrange(f.root, area, area));
    take(f.tree->arrange(f.node, area, area));
  }));
  const auto original = choices();
  const std::array reordered{original.rows()[3], original.rows()[0], original.rows()[1]};
  take(f.model->set(take(RadioGroupValue::from(take(RowCollection<RadioItem>::from(reordered)), RowId{11, 1}))));
  take(f.router->dispatch(MouseInputEvent{1, 2, MouseButton::LEFT, MouseAction::PRESS}));
  CHECK(calls == 1);
  CHECK(take(binding.value()).selectedId() == RowId{44, 1});
}

TEST_CASE("removed pointer candidate is consumed without choosing a replacement ordinal") {
  Fixture f;
  int calls{};
  auto binding = take(bindRadioGroup(*f.tree, *f.router, f.node, f.model, [&](auto) {
    ++calls;
  }));
  const auto area = take(CellRect::from({}, {20, 3}));

  take(f.tree->frame([&] {
    take(f.tree->arrange(f.root, area, area));
    take(f.tree->arrange(f.node, area, area));
  }));
  const auto original = choices();
  const std::array replacement{original.rows()[0], original.rows()[1]};
  take(f.model->set(take(RadioGroupValue::from(take(RowCollection<RadioItem>::from(replacement)), RowId{11, 1}))));
  const auto result = take(f.router->dispatch(MouseInputEvent{1, 2, MouseButton::LEFT, MouseAction::PRESS}));
  CHECK(result.status == DispatchStatus::HANDLED);
  CHECK(calls == 0);
  CHECK(take(binding.value()).selectedId() == RowId{11, 1});
  CHECK_FALSE(binding.select(RowId{44, 1}));
}

TEST_CASE("radio paints only rows inside its cumulative ancestor clip") {
  struct Sink : PaintContext {
    std::size_t textCommands{};

    std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
      for (const auto &command : commands) {
        textCommands += std::holds_alternative<TextCommand>(command);
      }
      return {};
    }
  } sink;

  Fixture f;
  auto binding = take(bindRadioGroup(*f.tree, *f.router, f.node, f.model, [](auto) {
  }));
  const auto area = take(CellRect::from({}, {20, 3}));
  const auto clipped = take(CellRect::from({0, 1}, {20, 1}));

  take(f.tree->frame([&] {
    take(f.tree->arrange(f.root, area, clipped));
    take(f.tree->arrange(f.node, area, clipped));
    take(f.tree->paint(f.node, sink));
  }));
  CHECK(sink.textCommands == 2);
  take(f.tree->read(f.node, [&](const RadioGroup &group) {
    CHECK(group.value().rows().size() == 4);
  }));

  RadioGroup unbound;
  CHECK(take(unbound.measure({})) == CellSize{});
  take(unbound.arrange(area));
  take(unbound.paint(sink));
  CHECK(sink.textCommands == 2);
}

TEST_CASE("loaded radio factories preserve named Ref, scalar settings and natural style identity") {
  ui::SceneResourceOptions options;
  options.format = ui::SceneResourceFormat::CONFIGURATION;
  take(options.registry.add<RadioGroup>("Modes", [](ui::SceneResourceArguments) {
    return ui::radioGroup({"Fast", "Balanced", "Quiet"});
  }));
  auto scene =
    ui::scene(R"(scene { Modes#mode { radioInitial.index=1 readOnly=true classes="settings" width=18 } })", options) |
    ui::style("Control { italic:true; } RadioGroup.settings { foreground:#123456; } #mode { underline:true; }");
  const auto mode = scene.get<RadioGroup>("mode");
  scene.configure([&](ManagedScene &owner, Application &app) {
    take(owner.prepare(app, {20, 4}));
    take(app.tree().read(mode.node(), [](const RadioGroup &widget) {
      REQUIRE(widget.computedStyle());
      CHECK(widget.computedStyle()->normal.foreground == RgbColor{0x12, 0x34, 0x56});
      CHECK(widget.computedStyle()->normal.italic);
      CHECK(widget.computedStyle()->normal.underline);
    }));
  });

  scene.prepare();
  take(scene.checked());
  CHECK(take(mode.value()).selectedText() == "Balanced");
  take(mode.select(RowId{3, 1}));
  CHECK(take(mode.value()).selectedText() == "Quiet");

  ui::Ref<RadioGroup> controlled;
  auto invalid = ui::scene([&](ManagedScene &owner) {
    return ui::radioGroup(controlled, owner.radioState({"Fast", "Quiet"})).settings({.radioInitial = {.index = 1}});
  });
  invalid.prepare();
  CHECK_FALSE(invalid.checked());
}

TEST_CASE("failed radio projection allocation preserves its immutable input") {
  const auto original = choices();
  const auto source = take(RowCollection<RadioItem>::from(original.rows()));
  const auto rows = source.rows().data();

  allocation_test::remaining = 0;
  const auto rejected = RadioGroupValue::from(source, RowId{11, 1});
  allocation_test::remaining = -1;

  REQUIRE_FALSE(rejected);
  CHECK(rejected.error() == TreeError::NO_MEMORY);
  CHECK(source.rows().data() == rows);
  CHECK(original.selectedText() == "Fast");
  const auto accepted = take(RadioGroupValue::from(source, RowId{44, 1}));
  CHECK(accepted.selectedText() == "Quiet");
}

TEST_CASE("replacing generated shorthand does not reinterpret an old pointer ordinal") {
  auto app = take(Application::create());
  auto owner = take(ManagedScene::create(*app));
  auto model = take(owner->radioState({"Old first", "Old second"}));
  const auto replacement = take(owner->radioState({"New first", "New second"}));
  const auto node = take(owner->emplace<RadioGroup>(owner->root()));
  int calls{};

  take(owner->bind(node, model));
  take(owner->onChange(node, [&](const RadioGroupValue &) {
    ++calls;
  }));
  const auto area = take(CellRect::from({}, {20, 2}));
  take(app->tree().frame([&] {
    take(app->tree().arrange(app->root(), area, area));
    take(app->tree().arrange(owner->root(), area, area));
    take(app->tree().arrange(node, area, area));
  }));
  take(model.set(take(replacement.snapshot())->value));
  const auto result = take(app->router().dispatch(MouseInputEvent{1, 1, MouseButton::LEFT, MouseAction::PRESS}));

  CHECK(result.status == DispatchStatus::HANDLED);
  CHECK(calls == 0);
  CHECK(take(model.snapshot())->value.selectedText() == "New first");
}

TEST_CASE("hidden source rows do not create paint work or an extra focus stop") {
  Fixture f;
  constexpr std::size_t HIDDEN_OPTIONS = 512;
  const auto caption = take(GraphemeText::from("Hidden"));
  std::vector<ModelRow<RadioItem>> rows(std::initializer_list<ModelRow<RadioItem>>{});

  rows.reserve(HIDDEN_OPTIONS + 1);
  for (std::size_t index = 0; index < HIDDEN_OPTIONS; ++index) {
    rows.push_back({RowId{static_cast<std::uint64_t>(index) + 1, 1}, RadioItem{caption, true, false}});
  }
  rows.push_back({RowId{HIDDEN_OPTIONS + 1, 1}, RadioItem{take(GraphemeText::from("Visible"))}});
  const auto source = take(RadioGroupValue::from(take(RowCollection<RadioItem>::from(rows))));
  take(f.model->set(source));
  auto binding = take(bindRadioGroup(*f.tree, *f.router, f.node, f.model, [](auto) {
  }));
  CHECK(take(f.tree->measure(f.node, {})) == CellSize{11, 1});
  take(f.router->setFocus(f.node));
  take(f.router->dispatch(KeyEvent{InputKey::DOWN}));
  CHECK(take(binding.value()).selectedText() == "Visible");
  const auto button = take(f.tree->emplace<Button>(f.root, take(GraphemeText::from("Next"))));
  auto action = take(bindButton(*f.router, button, [] {
  }));
  take(f.router->dispatch(KeyEvent{InputKey::TAB}));
  CHECK(take(f.router->focused()) == button);
}

TEST_CASE("radio geometry requires current arranged frame and survives resize admission") {
  struct Sink : PaintContext {
    std::size_t textCommands{};

    std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
      for (const auto &command : commands) {
        textCommands += std::holds_alternative<TextCommand>(command);
      }
      return {};
    }
  } sink;

  Fixture f;
  auto binding = take(bindRadioGroup(*f.tree, *f.router, f.node, f.model, [](auto) {
  }));
  std::expected<void, TreeError> painted;
  take(f.tree->frame([&] {
    painted = f.tree->paint(f.node, sink);
  }));
  REQUIRE_FALSE(painted);
  CHECK(painted.error() == TreeError::INVALID_GEOMETRY);

  const auto initial = take(CellRect::from({}, {20, 3}));
  take(f.tree->frame([&] {
    take(f.tree->arrange(f.root, initial, initial));
    take(f.tree->arrange(f.node, initial, initial));
    take(f.tree->paint(f.node, sink));
  }));
  CHECK(sink.textCommands == 6);

  take(f.tree->invalidate(f.node, Invalidation::LAYOUT));
  take(f.tree->frame([&] {
    painted = f.tree->paint(f.node, sink);
  }));
  REQUIRE_FALSE(painted);
  CHECK(painted.error() == TreeError::INVALID_GEOMETRY);
  const auto resized = take(CellRect::from({}, {8, 1}));
  take(f.tree->frame([&] {
    take(f.tree->arrange(f.root, resized, resized));
    take(f.tree->arrange(f.node, resized, resized));
    take(f.tree->paint(f.node, sink));
  }));
  CHECK(sink.textCommands == 8);
  take(f.tree->edit(f.node, [&](RadioGroup &group) {
    painted = group.paint(sink);
  }));
  REQUIRE_FALSE(painted);
  CHECK(painted.error() == TreeError::WOULD_BLOCK);

  std::thread worker([&] {
    painted = syncRadioGroup(*f.tree, f.node, {});
  });
  worker.join();
  REQUIRE_FALSE(painted);
  CHECK(painted.error() == TreeError::WRONG_THREAD);
  take(f.tree->remove(f.node));
  CHECK(binding.sync().error() == TreeError::STALE_NODE);
}

TEST_CASE("radio user receipt retains successful commit when observer publication is rejected") {
  Fixture f(false, {.data_capacity = 1});
  std::optional<RadioGroupResult> receipt;
  auto binding = take(bindRadioGroup(*f.tree, *f.router, f.node, f.model, [&](auto result) {
    receipt = result;
  }));
  take(f.model->set(choices(RowId{44, 1})));
  take(f.router->invoke(f.node, ActionEvent{Action::MOVE_DOWN}));

  REQUIRE(receipt);
  REQUIRE(*receipt);
  CHECK((*receipt)->changed);
  CHECK((*receipt)->rejected == 1);
  CHECK((*receipt)->snapshot->value.selectedText() == "Fast");
  CHECK((*receipt)->notification_error.has_value());
  take(f.scheduler->dispatch());
  take(f.tree->read(f.node, [](const RadioGroup &group) {
    CHECK(group.value().selectedText() == "Fast");
  }));
}

TEST_CASE("radio callback failure does not roll back its committed value") {
  Fixture f;
  auto binding = take(bindRadioGroup(*f.tree, *f.router, f.node, f.model, [](auto) {
    throw std::runtime_error("business");
  }));
  const auto result = f.router->invoke(f.node, ActionEvent{Action::MOVE_DOWN});

  REQUIRE_FALSE(result);
  CHECK(result.error() == TreeError::CALLBACK_FAILED);
  CHECK(take(binding.value()).selectedText() == "Quiet");
}

TEST_CASE("radio rejects foreign scene models without occupying its event binding") {
  auto app = take(Application::create());
  auto first = take(ManagedScene::create(*app));
  auto second = take(ManagedScene::create(*app));
  const auto foreign = take(first->radioState({"One", "Two"}));
  const auto local = take(second->radioState({"Local", "Other"}));
  const auto node = take(second->emplace<RadioGroup>(second->root()));
  const auto rejected = second->bind(node, foreign);

  REQUIRE_FALSE(rejected);
  CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::INVALID_ARGUMENT);
  take(second->bind(node, local));
  take(app->router().invoke(node, ActionEvent{Action::MOVE_DOWN}));
  CHECK(take(second->radioValue(node)).selectedText() == "Other");
}

TEST_CASE("radio late handlers appended during delivery start at the next operation") {
  auto app = take(Application::create());
  auto owner = take(ManagedScene::create(*app));
  const auto model = take(owner->radioState({"One", "Two"}));
  const auto node = take(owner->emplace<RadioGroup>(owner->root()));
  int primary{}, late{};

  take(owner->bind(node, model));
  take(owner->onChange(node, [&](const RadioGroupValue &) {
    ++primary;
    take(owner->onChange(node, [&](const RadioGroupValue &) {
      ++late;
    }));
  }));
  take(app->router().invoke(node, ActionEvent{Action::MOVE_DOWN}));
  CHECK(primary == 1);
  CHECK(late == 0);
  take(app->router().invoke(node, ActionEvent{Action::MOVE_DOWN}));
  CHECK(primary == 2);
  CHECK(late == 1);
}

TEST_CASE("loaded authoritative radio model rejects initial override without replacing its value") {
  for (const bool overrideInitial : {false, true}) {
    ManagedState<RadioGroupValue> model;
    auto scene = ui::scene([&](ManagedScene &owner) {
      model = take(owner.radioState({"Fast", "Quiet"}));
      take(model.set(take(take(model.snapshot())->value.withSelection({}))));
      ui::SceneResourceOptions options;

      options.format = ui::SceneResourceFormat::CONFIGURATION;
      take(options.registry.add<RadioGroup>("BoundModes", [model](ui::SceneResourceArguments) {
        return ui::radioGroup(model);
      }));
      return ui::resource(overrideInitial ? "scene { BoundModes#mode { radioInitial.index=1 } }"
                                          : "scene { BoundModes#mode { readOnly=true } }",
                          options);
    });
    const auto mode = scene.get<RadioGroup>("mode");
    scene.prepare();
    if (overrideInitial) {
      const auto checked = scene.checked();
      REQUIRE_FALSE(checked);
      CHECK(std::get<TreeError>(checked.error().cause) == TreeError::INVALID_ARGUMENT);
    } else {
      take(scene.checked());
      CHECK_FALSE(take(mode.value()).selectedId());
    }
    CHECK_FALSE(take(model.snapshot())->value.selectedId());
  }
}

TEST_CASE("radio caption clipping preserves whole wide and combining graphemes") {
  struct Sink : PaintContext {
    std::vector<RenderCommand> commands = std::vector<RenderCommand>(std::initializer_list<RenderCommand>{});

    std::expected<void, TreeError> submit(std::span<const RenderCommand> values) override {
      for (const auto &value : values) {
        commands.emplace_back(value);
      }
      return {};
    }
  };

  Fixture f;
  const std::array rows{ModelRow<RadioItem>{{1, 1}, {take(GraphemeText::from("\xe7\x95\x8c e\xcc\x81"))}}};
  take(f.model->set(take(RadioGroupValue::from(take(RowCollection<RadioItem>::from(rows)), RowId{1, 1}))));
  auto binding = take(bindRadioGroup(*f.tree, *f.router, f.node, f.model, [](auto) {
  }));

  for (const auto width : {5, 6, 8}) {
    const auto area = take(CellRect::from({}, {width, 1}));
    std::optional<CellBuffer> buffer;
    take(f.tree->frame([&] {
      take(f.tree->arrange(f.root, area, area));
      take(f.tree->arrange(f.node, area, area));
      Sink sink;

      take(f.tree->paint(f.node, sink));
      buffer.emplace(take(renderFullFrame({width, 1}, sink.commands)));
    }));
    REQUIRE(buffer);
    CHECK(take(buffer->at({4, 0})).glyph == (width == 5 ? " " : "\xe7\x95\x8c"));
    if (width >= 6) {
      CHECK(take(buffer->at({5, 0})).cell.kind == CellKind::CONTINUATION);
    }
    if (width == 8) {
      CHECK(take(buffer->at({7, 0})).glyph == "e\xcc\x81");
    }
  }
}

TEST_CASE("radio sync pins its owner while reclaiming the previous source reenters UI") {
  Fixture f;
  const std::array rows{ModelRow<RadioItem>{{11, 1}, {take(GraphemeText::from(std::string(80, 'x')))}}};
  take(f.model->set(take(RadioGroupValue::from(take(RowCollection<RadioItem>::from(rows)), RowId{11, 1}))));
  std::optional<RadioGroupBinding> binding;
  binding.emplace(take(bindRadioGroup(*f.tree, *f.router, f.node, f.model, [](auto) {
  })));
  const auto reclaimedText = take(binding->value()).rows().front().value.text.bytes().data();
  take(f.model->set(choices(RowId{44, 1})));
  int reclaimed{};
  bool complete{}, readable{};

  allocation_test::reclaimAction = [&] {
    ++reclaimed;
    readable = f.tree
                 ->read(f.node,
                        [&](const RadioGroup &group) {
                          complete = group.value().selectedText() == "Quiet";
                        })
                 .has_value();
    binding.reset();
  };
  allocation_test::reclaimTarget = reclaimedText;
  const auto result = binding->sync();
  allocation_test::reclaimTarget = nullptr;
  allocation_test::reclaimAction = {};

  CHECK(result.has_value());
  CHECK(reclaimed == 1);
  CHECK(readable);
  CHECK(complete);
  CHECK_FALSE(binding);
}

TEST_CASE("radio programmatic admission rejects incompatible width policy before model commit") {
  Fixture f;
  auto binding = take(bindRadioGroup(*f.tree, *f.router, f.node, f.model, [](auto) {
  }));
  WidthPolicy wide;

  wide.ambiguous = AmbiguousWidth::WIDE;
  const std::array rows{ModelRow<RadioItem>{{11, 1}, {take(GraphemeText::from("One", wide))}},
                        ModelRow<RadioItem>{{44, 1}, {take(GraphemeText::from("Two", wide))}}};
  take(f.model->set(take(RadioGroupValue::from(take(RowCollection<RadioItem>::from(rows)), RowId{11, 1}))));
  const auto before = take(f.model->snapshot())->revision;
  const auto result = binding.select(RowId{44, 1});

  REQUIRE_FALSE(result);
  CHECK(result.error() == TreeError::INVALID_ARGUMENT);
  CHECK(take(f.model->snapshot())->revision == before);
}

TEST_CASE("promoting generated rows to explicit identities preserves subsequent pointer admission") {
  auto app = take(Application::create());
  auto owner = take(ManagedScene::create(*app));
  auto model = take(owner->radioState({"One", "Two"}));
  const auto initial = take(model.snapshot());
  const auto explicitValue = take(
    RadioGroupValue::from(take(RowCollection<RadioItem>::from(initial->value.rows())), initial->value.selectedId()));

  CHECK(take(model.set(explicitValue)).changed);
  const auto node = take(owner->emplace<RadioGroup>(owner->root()));
  take(owner->bind(node, model));
  const auto area = take(CellRect::from({}, {20, 2}));
  take(app->tree().frame([&] {
    take(app->tree().arrange(app->root(), area, area));
    take(app->tree().arrange(owner->root(), area, area));
    take(app->tree().arrange(node, area, area));
  }));
  const std::array reordered{explicitValue.rows()[1], explicitValue.rows()[0]};
  take(model.set(take(RadioGroupValue::from(take(RowCollection<RadioItem>::from(reordered)), RowId{1, 1}))));
  take(app->router().dispatch(MouseInputEvent{1, 1, MouseButton::LEFT, MouseAction::PRESS}));
  CHECK(take(model.snapshot())->value.selectedText() == "Two");
}

TEST_CASE("radio resolver teardown returns captured CLOSED for success and error paths") {
  enum class Completion {
    VALUE,
    FAILED_RESULT,
    THROW_ERROR,
    THROW_ALLOCATION
  };
  for (const auto completion :
       {Completion::VALUE, Completion::FAILED_RESULT, Completion::THROW_ERROR, Completion::THROW_ALLOCATION}) {
    auto app = take(Application::create());
    auto owner = take(ManagedScene::create(*app));
    const auto node = owner->root();
    const std::vector<RadioOption> options{{i18n("first")}, {i18n("second")}};
    int calls{};

    const auto result =
      owner->radioState(options, [&](std::string_view) -> std::expected<std::string, TextResourceError> {
        ++calls;
        owner.reset();
        switch (completion) {
        case Completion::VALUE:
          return "Resolved";
        case Completion::FAILED_RESULT:
          return std::unexpected(TextResourceError::RESOLVER_FAILED);
        case Completion::THROW_ERROR:
          throw std::runtime_error("resolver");
        case Completion::THROW_ALLOCATION:
          throw std::bad_alloc();
        }
        return "";
      });

    REQUIRE_FALSE(owner);
    REQUIRE_FALSE(result);
    CHECK(calls == 1);
    CHECK(result.error().phase == ManagedPhase::STATE);
    CHECK(result.error().node == node.id());
    CHECK(std::get<TreeError>(result.error().cause) == TreeError::CLOSED);
    const auto retired = app->tree().inspect(node);
    REQUIRE_FALSE(retired);
    CHECK(retired.error() == TreeError::STALE_NODE);
  }
}

TEST_CASE("radio ignored configuration select failure is collected once and preserves selection") {
  ui::Ref<RadioGroup> group;
  auto scene = ui::scene(ui::radioGroup(group, {"Fast", "Quiet"}));
  int errors{};
  bool continued{};
  std::string observed;

  scene.onError([&](const ManagedDiagnostic &diagnostic) {
    ++errors;
    CHECK(diagnostic.error.operation == std::string_view("radio.select"));
  });
  scene.configure([&](ManagedScene &, Application &app) {
    (void)group.select(RowId{999, 1});
    take(app.tree().read(group.node(), [&](const RadioGroup &view) {
      observed = view.value().selectedText();
    }));
  });
  scene.configure([&](ManagedScene &) {
    continued = true;
  });

  CHECK_FALSE(scene.prepareChecked());
  const auto diagnostics = take(scene.diagnostics());

  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(errors == 1);
  CHECK(std::get<TreeError>(diagnostics.errors.front().error.cause) == TreeError::INVALID_ARGUMENT);
  CHECK(diagnostics.errors.front().error.setupStopped);
  CHECK(observed == "Fast");
  CHECK_FALSE(continued);
}

TEST_CASE("radio ignored void callback select failure reports once without changing the model") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  ui::Ref<RadioGroup> group;
  ui::Ref<Button> button;
  int errors{};

  take(scene->onError([&](const ManagedDiagnostic &diagnostic) {
    ++errors;
    CHECK(diagnostic.error.operation == std::string_view("radio.select"));
  }));
  take(ui::mount(
    *scene, ui::vbox(ui::radioGroup(group, {"Fast", "Quiet"}), ui::button(button, "Invalid selection").onClick([=] {
      (void)group.select(RowId{999, 1});
    }))));
  take(app->tree().consumeFrameRequest());
  const auto invoked = app->router().invoke(button.node(), {Action::ACTIVATE});
  const auto diagnostics = take(scene->diagnostics());

  REQUIRE(invoked);
  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(errors == 1);
  CHECK(std::get<TreeError>(diagnostics.errors.front().error.cause) == TreeError::INVALID_ARGUMENT);
  CHECK_FALSE(diagnostics.errors.front().error.setupStopped);
  CHECK(take(group.value()).selectedText() == "Fast");
  CHECK(take(app->tree().consumeFrameRequest()) == Invalidation::NONE);
}

TEST_CASE("radio Ref select preserves exact success receipts and checked failure provenance") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  const auto model = take(scene->radioState({"Fast", "Quiet"}));
  ui::Ref<RadioGroup> group;
  take(ui::mount(*scene, ui::radioGroup(group, model)));
  const auto initial = take(model.snapshot());
  const auto committed = take(group.select(initial->value.rows()[1].id));

  CHECK(committed.changed);
  CHECK(committed.snapshot == take(model.snapshot()));
  CHECK(committed.snapshot->value.selectedText() == "Quiet");
  const auto equal = take(group.select(committed.snapshot->value.selectedId()));

  CHECK_FALSE(equal.changed);
  CHECK(equal.snapshot == committed.snapshot);
  CHECK(equal.accepted == 0);
  CHECK(equal.rejected == 0);
  CHECK(equal.cancelled == 0);
  CHECK_FALSE(equal.notification_error);
  CHECK(take(scene->diagnostics()).errors.empty());
  const auto lowLevel = scene->selectRadio(group.node(), RowId{999, 1});

  REQUIRE_FALSE(lowLevel);
  CHECK(take(scene->diagnostics()).errors.empty());
  const auto origin = std::source_location::current();
  const auto rejected = group.select(RowId{999, 1}, origin);

  REQUIRE_FALSE(rejected);
  CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::INVALID_ARGUMENT);
  CHECK(rejected.error().operation == std::string_view("radio.select"));
  REQUIRE(rejected.error().operationOrigin);
  CHECK(rejected.error().operationOrigin->line() == origin.line());
  CHECK(rejected.error().node == group.node().id());
  const auto diagnostics = take(scene->diagnostics());

  REQUIRE(diagnostics.errors.size() == 1);
  REQUIRE(diagnostics.errors.front().error.operationOrigin);
  CHECK(diagnostics.errors.front().error.operationOrigin->line() == origin.line());
  CHECK(take(model.snapshot()) == committed.snapshot);
}
