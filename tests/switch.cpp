// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <iostream>
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>
#include <source_location>
#include <thread>
using namespace portals;

namespace {
template <class T, class E>
T take(std::expected<T, E> value, std::source_location origin = std::source_location::current()) {
  if (!value) {
    std::cerr << "checked failure at " << origin.line() << '\n';
    if constexpr (std::same_as<E, ManagedError>) {
      ui::reportToStderr(ui::SceneReport{.failure = value.error()});
    } else if constexpr (std::is_enum_v<E>) {
      std::cerr << "enum cause=" << static_cast<int>(value.error()) << '\n';
    }
  }
  REQUIRE(value);
  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

bool cause(const ManagedError &error, TreeError expected) {
  auto p = std::get_if<TreeError>(&error.cause);
  return p && *p == expected;
}

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> owner = take(ManagedScene::create(*app));
  ui::Ref<Switch> toggle;
  ManagedState<bool> model = take(owner->state(false));

  Fixture(bool readOnly = false) {
    take(ui::mount(*owner, ui::toggle(toggle, "Network", model, {.readOnly = readOnly})));
  }
};
} // namespace

TEST_CASE("switch bool input and property writes preserve exact receipts without user notifications") {
  Fixture f;
  int calls{};
  take(f.owner->onChange(f.toggle.node(), [&](const PropertyUpdate<bool> &value) {
    ++calls;
    CHECK(value.snapshot->value);
  }));
  take(f.app->router().invoke(f.toggle.node(), {Action::ACTIVATE}));
  CHECK(calls == 1);
  CHECK(take(f.toggle.checked().get()));
  const auto changed = take(f.toggle.checked().set(false));
  CHECK(changed.changed);
  CHECK(changed.snapshot == take(f.model.snapshot()));
  CHECK_FALSE(changed.snapshot->value);
  take(f.app->tree().consumeFrameRequest());
  const auto equal = take((f.toggle.checked() = false).result());
  CHECK_FALSE(equal.changed);
  CHECK(equal.snapshot == changed.snapshot);
  CHECK(equal.accepted == 0);
  CHECK(equal.rejected == 0);
  CHECK(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  CHECK(calls == 1);
}

TEST_CASE("readonly switch remains focusable but blocks user writes and permits model writes") {
  Fixture f(true);
  take(f.app->router().invoke(f.toggle.node(), {Action::ACTIVATE}));
  CHECK_FALSE(take(f.toggle.checked().get()));
  take((f.toggle.checked() = true).result());
  CHECK(take(f.toggle.checked().get()));
  take(f.app->tree().read(f.toggle.node(), [](const Switch &view) {
    CHECK(view.readOnly());
  }));
  take(f.model.set(false));
  CHECK_FALSE(take(f.toggle.checked().get()));
}

TEST_CASE("switch native chrome validates complete policy width and rejects enum bindings") {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  const auto text = take(GraphemeText::from("Network"));
  const auto node = take(scene->emplace<Switch>(scene->root(), text));
  take(app->tree().read(node, [&](const Switch &view) {
    CHECK(view.checked() == false);
  }));
  CHECK_FALSE(SwitchChrome::from("off", "on"));
  CHECK_FALSE(SwitchChrome::from("off\n", "on  "));
  auto model = take(scene->state(CheckState::MIXED));
  CHECK_FALSE(scene->bind(Node<CheckBox>{node}, model));
  auto plain = take(scene->state(CheckState::UNCHECKED));
  CHECK_FALSE(scene->bind(Node<CheckBox>{node}, plain, {}, {.cycle = CheckCycle::THREE_STATE}));
  take(app->tree().frame([&] {
    const auto size = take(app->tree().measure(node, {80, 24}));
    CHECK(size == CellSize{13, 1});
  }));
}

TEST_CASE("switch property observes publication and retires with its original scene") {
  ui::Ref<Switch> toggle;
  const auto property = toggle.checked();
  CHECK_FALSE(property.get());
  auto app = take(Application::create());
  auto owner = take(ManagedScene::create(*app));
  take(ui::mount(*owner, ui::toggle(toggle, "Network", true)));
  CHECK(take(property.get()));
  std::optional<ManagedError> foreign;
  std::thread worker([&] {
    auto value = property.get();
    if (!value) {
      foreign = value.error();
    }
  });
  worker.join();
  REQUIRE(foreign);
  CHECK(cause(*foreign, TreeError::WRONG_THREAD));
  owner.reset();
  CHECK(cause(property.get().error(), TreeError::CLOSED));
}

TEST_CASE("switch ignored property failure enters configure collector once") {
  ui::Ref<Switch> toggle;
  auto scene = ui::scene(ui::label("No switch"));
  int errors{};
  bool continued{};
  scene.onError([&](const ManagedDiagnostic &d) {
    ++errors;
    CHECK(d.error.operation == std::string_view("switch.checked"));
  });
  scene.configure([&](ManagedScene &) {
    toggle.checked() = true;
  });
  scene.configure([&](ManagedScene &) {
    continued = true;
  });
  CHECK_FALSE(scene.prepareChecked());
  CHECK(errors == 1);
  CHECK_FALSE(continued);
  CHECK(take(scene.diagnostics()).errors.size() == 1);
}

TEST_CASE("switch model factory and user only onChange use the ordinary scene pipeline") {
  ui::Ref<Switch> toggle;
  Application *app{};
  int calls{};
  auto scene = ui::scene([&](ManagedScene &owner, Application &application) {
                 app = &application;
                 return ui::toggle(toggle, "Network", owner.state(false));
               }) |
               ui::onChange(toggle, [&](bool value) {
                 ++calls;
                 CHECK(value);
               });
  take(scene.prepareChecked());
  take((toggle.checked() = true).result());
  CHECK(calls == 0);
  take((toggle.checked() = false).result());
  take(app->router().invoke(toggle.node(), {Action::ACTIVATE}));
  CHECK(calls == 1);
}

TEST_CASE("loaded Switch preserves checked readonly and natural style identity") {
  ui::SceneResourceOptions options;
  options.format = ui::SceneResourceFormat::CONFIGURATION;
  auto scene =
    ui::scene(R"(scene { Switch#network { checked=true text="Network" readOnly=true classes="settings" width=20 } })",
              options) |
    ui::style("Control { italic:true; } CheckBox { underline:true; } Switch.settings { foreground:#123456; }");
  const auto toggle = scene.get<Switch>("network");
  scene.configure([&](ManagedScene &owner, Application &app) {
    take(owner.prepare(app, {30, 4}));
    take(app.tree().read(toggle.node(), [](const Switch &view) {
      CHECK(view.readOnly());
      REQUIRE(view.computedStyle());
      CHECK(view.computedStyle()->normal.italic);
      CHECK(view.computedStyle()->normal.underline);
      CHECK(view.computedStyle()->normal.foreground == RgbColor{0x12, 0x34, 0x56});
    }));
  });
  take(scene.prepareChecked());
  CHECK(take(toggle.checked().get()));
}

TEST_CASE("Switch explicit initial settings never override external authoritative bool models") {
  for (bool initial : {false, true}) {
    ui::Ref<Switch> toggle;
    auto scene = ui::scene([&](ManagedScene &owner) {
      return ui::toggle(toggle, "Network", owner.state(initial))
        .settings({.state = initial ? CheckState::CHECKED : CheckState::UNCHECKED});
    });
    CHECK_FALSE(scene.prepareChecked());
  }
}

TEST_CASE("readonly switch focus and real keyboard mouse routes cannot mutate its bool") {
  Fixture f(true);
  take(f.owner->prepare(*f.app, {40, 4}));
  const auto bounds = take(CellRect::from({}, {40, 4}));
  take(f.app->tree().frame([&] {
    take(layoutTree(f.app->tree(), f.app->root(), bounds, bounds));
  }));
  take(f.app->router().setFocus(f.toggle.node()));
  CHECK(take(f.app->router().focused()) == f.toggle.node());
  take(f.app->router().dispatch(TextInputEvent{U' '}));
  take(f.app->router().dispatch(KeyEvent{InputKey::ENTER}));
  take(f.app->router().dispatch(MouseInputEvent{1, 0, MouseButton::LEFT, MouseAction::PRESS, {}}));
  CHECK_FALSE(take(f.toggle.checked().get()));
  CHECK(take(f.app->router().focused()) == f.toggle.node());
}

TEST_CASE("switch routed input toggles once and disabled hidden ancestors prevent writes") {
  Fixture f;
  take(f.owner->prepare(*f.app, {40, 4}));
  const auto bounds = take(CellRect::from({}, {40, 4}));
  take(f.app->tree().frame([&] {
    take(layoutTree(f.app->tree(), f.app->root(), bounds, bounds));
  }));
  take(f.app->router().setFocus(f.toggle.node()));
  take(f.app->router().dispatch(MouseInputEvent{1, 0, MouseButton::LEFT, MouseAction::PRESS, {}}));
  CHECK(take(f.toggle.checked().get()));
  take(f.app->router().dispatch(MouseInputEvent{1, 0, MouseButton::LEFT, MouseAction::RELEASE, {}}));
  CHECK(take(f.toggle.checked().get()));
  take(f.app->router().dispatch(TextInputEvent{U' '}));
  CHECK_FALSE(take(f.toggle.checked().get()));
  take(f.app->router().dispatch(KeyEvent{InputKey::ENTER}));
  CHECK(take(f.toggle.checked().get()));
  take(f.app->tree().setEnabled(f.owner->root(), false));
  take(f.app->router().invoke(f.toggle.node(), {Action::ACTIVATE}));
  CHECK(take(f.toggle.checked().get()));
  take((f.toggle.checked() = false).result());
  CHECK_FALSE(take(f.toggle.checked().get()));
  take(f.app->tree().setEnabled(f.owner->root(), true));
  take(f.app->tree().setVisible(f.owner->root(), false));
  take(f.app->router().invoke(f.toggle.node(), {Action::ACTIVATE}));
  CHECK_FALSE(take(f.toggle.checked().get()));
}

TEST_CASE("switch plain chrome paints actual model state and preserves one line width") {
  Fixture f;
  auto draw = [&] {
    std::optional<CellBuffer> buffer;
    take(f.owner->prepare(*f.app, {30, 1}));
    take(f.app->tree().frame([&] {
      struct Sink : PaintContext {
        std::vector<RenderCommand> commands;
        std::expected<void, TreeError> submit(std::span<const RenderCommand> values) override {
          for (const auto &value : values) {
            commands.emplace_back(value);
          }
          return {};
        }
      } sink;
      const auto bounds = take(CellRect::from({}, {30, 1}));
      take(layoutTree(f.app->tree(), f.app->root(), bounds, bounds));
      take(f.app->tree().paint(f.toggle.node(), sink));
      buffer = take(renderFullFrame({30, 1}, sink.commands));
    }));
    std::string result;
    for (int x = 0; x < 13; ++x) {
      auto cell = take(buffer->at({x, 0}));
      result += cell.glyph;
    }
    return result;
  };
  CHECK(draw() == "[off] Network");
  take((f.toggle.checked() = true).result());
  CHECK(draw() == "[on ] Network");
}

TEST_CASE("switch callback reentry preserves exact admitted receipt after node removal") {
  Fixture f;
  int later{};
  std::optional<PropertyUpdate<bool>> original;
  take(f.owner->onChange(f.toggle.node(), [&](const PropertyUpdate<bool> &receipt) {
    original = receipt;
    take(f.model.set(false));
    take(f.owner->remove(f.toggle.node()));
    CHECK(receipt.snapshot->value);
  }));
  take(f.owner->onChange(f.toggle.node(), [&](bool) {
    ++later;
  }));
  take(f.app->router().invoke(f.toggle.node(), {Action::ACTIVATE}));
  REQUIRE(original);
  CHECK(original->snapshot->value);
  CHECK_FALSE(take(f.model.snapshot())->value);
  CHECK(later == 0);
}

TEST_CASE("checked proxy assignment copies bool for lvalue rvalue and self without rebinding") {
  auto app = take(Application::create());
  auto owner = take(ManagedScene::create(*app));
  ui::Ref<Switch> source, destination;
  take(ui::mount(*owner, ui::vbox(ui::toggle(source, "Source", true), ui::toggle(destination, "Destination", false))));
  int calls{};
  take(owner->onChange(destination.node(), [&](bool) {
    ++calls;
  }));
  auto a = destination.checked();
  auto b = source.checked();
  const auto copied = take((a = b).result());
  CHECK(copied.changed);
  CHECK(take(a.get()));
  CHECK(take(b.get()));
  CHECK(take(destination.checked().get()));
  take(a.set(false));
  CHECK_FALSE(take(destination.checked().get()));
  CHECK(take(source.checked().get()));
  take((destination.checked() = source.checked()).result());
  const auto self = take((a = a).result());
  CHECK_FALSE(self.changed);
  CHECK(take(a.get()));
  CHECK(calls == 0);
}

TEST_CASE("failed checked proxy source leaves destination unchanged and reports once") {
  Fixture f;
  ui::Ref<Switch> absent;
  auto destination = f.toggle.checked();
  const auto failed = destination = absent.checked();
  REQUIRE_FALSE(failed.result());
  CHECK(cause(failed.result().error(), TreeError::CLOSED));
  CHECK_FALSE(take(destination.get()));
  CHECK_FALSE(take(f.model.snapshot())->value);
  const auto diagnostics = take(f.owner->diagnostics());
  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(diagnostics.errors.front().error.operation == std::string_view("switch.checked"));
}

TEST_CASE("checked assignment retains its observation when error callback destroys the original proxy") {
  Fixture f;
  auto property = std::make_unique<ui::CheckedProperty>(f.toggle.checked());
  ui::Ref<Switch> absent;
  take(f.owner->onError([&](const ManagedDiagnostic &) {
    property.reset();
  }));
  const auto failed = (*property = absent.checked());
  CHECK_FALSE(property);
  REQUIRE_FALSE(failed.result());
  CHECK(cause(failed.result().error(), TreeError::CLOSED));
  CHECK_FALSE(take(f.model.snapshot())->value);
}

TEST_CASE("resource Switch source defaults preserve model absence and reject explicit code conflicts") {
  for (bool explicitInitial : {false, true}) {
    ui::Ref<Switch> toggle;
    auto scene = ui::scene([&](ManagedScene &owner) {
      const auto model = take(owner.state(true));
      ui::SceneResourceOptions options;
      options.format = ui::SceneResourceFormat::CONFIGURATION;
      take(options.registry.add<Switch>("Bound", [=](ui::SceneResourceArguments) {
        return ui::toggle(toggle, "Network", model);
      }));
      return ui::resource(explicitInitial ? "scene { Bound { checked=true } }" : "scene { Bound {} }", options);
    });
    const auto ready = scene.prepareChecked();
    CHECK(bool(ready) == !explicitInitial);
    if (ready) {
      CHECK(take(toggle.checked().get()));
    } else {
      CHECK(cause(ready.error(), TreeError::INVALID_ARGUMENT));
      REQUIRE(ready.error().sceneResource);
      CHECK(ready.error().sceneResource->code == SceneResourceErrorCode::FACTORY);
    }
  }
}

TEST_CASE("Switch cold ingress resolves unicode with receiving policy and validates custom chrome") {
  int calls{};
  ui::Ref<Switch> toggle;
  ui::SwitchOptions options;
  options.offText = "<off> ";
  options.onText = "<on > ";
  options.resolver = [&](std::string_view key) -> std::expected<std::string, TextResourceError> {
    ++calls;
    CHECK(key == "network");
    return "Сеть 界";
  };
  auto scene = ui::scene(ui::toggle(toggle, i18n("network"), true, options));
  take(scene.prepareChecked());
  CHECK(calls == 1);
  CHECK(take(toggle.text().get()) == "Сеть 界");
  CHECK(take(toggle.checked().get()));
  ui::Ref<Switch> invalid;
  auto rejected = ui::scene(ui::toggle(invalid, "Network", false, {.offText = "X", .onText = "Long"}));
  CHECK_FALSE(rejected.prepareChecked());
  CHECK(invalid.node() == Node<Switch>{});
  const auto wide = WidthPolicy{.ambiguous = AmbiguousWidth::WIDE};
  auto text = take(GraphemeText::from("Caption", wide));
  Switch mismatched(text, take(SwitchChrome::from("off", "on ")));
  CHECK(mismatched.measure({}).error() == TreeError::INVALID_GEOMETRY);
}

TEST_CASE("switch low level failures stay manual while ignored callback failures report once") {
  Fixture f;
  const auto node = f.toggle.node();
  take(f.owner->remove(node));
  const auto manual = f.owner->setChecked(Node<CheckBox>{node}, true);
  REQUIRE_FALSE(manual);
  CHECK(cause(manual.error(), TreeError::STALE_NODE));
  CHECK(take(f.owner->diagnostics()).errors.empty());
  ui::Ref<Button> button;
  int errors{};
  take(f.owner->onError([&](const ManagedDiagnostic &) {
    ++errors;
  }));
  take(ui::mount(*f.owner, ui::button(button, "Set removed switch").onClick([ref = f.toggle] {
    ref.checked() = true;
  })));
  take(f.app->router().invoke(button.node(), {Action::ACTIVATE}));
  CHECK(errors == 1);
  CHECK(take(f.owner->diagnostics()).errors.size() == 1);
  CHECK_FALSE(take(f.model.snapshot())->value);
}

TEST_CASE("Switch checked alias shares state presence and keeps unknown controls closed") {
  ui::SceneResourceOptions options;
  options.format = ui::SceneResourceFormat::CONFIGURATION;
  for (auto source :
       {"scene { Switch { checked=true state=CHECKED } }", "scene { Switch { state=UNCHECKED checked=false } }"}) {
    auto duplicate = ui::scene(source, options);
    const auto rejected = duplicate.prepareChecked();
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().sceneResource);
    CHECK(rejected.error().sceneResource->code == SceneResourceErrorCode::DUPLICATE_PROPERTY);
  }
  auto wrong = ui::scene("scene { Label { checked=true } }", options);
  const auto rejected = wrong.prepareChecked();
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().sceneResource);
  CHECK(rejected.error().sceneResource->code == SceneResourceErrorCode::UNKNOWN_PROPERTY);
}

TEST_CASE("Switch caption and chrome validation use the actual receiving wide policy") {
  auto app = take(Application::create({.widthPolicy = {.ambiguous = AmbiguousWidth::WIDE}}));
  auto owner = take(ManagedScene::create(*app));
  ui::Ref<Switch> good;
  take(ui::mount(*owner, ui::toggle(good, "·")));
  take(app->tree().frame([&] {
    CHECK(take(app->tree().measure(good.node(), {30, 1})) == CellSize{8, 1});
  }));
  ui::Ref<Switch> rejected;
  const auto invalid = ui::mount(*owner, ui::toggle(rejected, "Caption", false, {.offText = "·", .onText = "x"}));
  REQUIRE_FALSE(invalid);
  CHECK(cause(invalid.error(), TreeError::INVALID_ARGUMENT));
  CHECK(rejected.node() == Node<Switch>{});
  CHECK_FALSE(take(good.checked().get()));
}

TEST_CASE("failed Switch state creation wins before a competing caption resolver failure") {
  ui::Ref<Switch> toggle;
  const ManagedError original{ManagedPhase::STATE, {}, TreeError::NODE_LIMIT, {}, {}};
  const std::expected<ManagedState<bool>, ManagedError> failed = std::unexpected(original);
  int resolverCalls{};
  ui::SwitchOptions options;
  options.resolver = [&](std::string_view) -> std::expected<std::string, TextResourceError> {
    ++resolverCalls;
    return std::unexpected(TextResourceError::RESOLVER_FAILED);
  };
  auto scene = ui::scene(ui::toggle(toggle, i18n("missing"), failed, options));
  const auto rejected = scene.prepareChecked();
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().phase == ManagedPhase::STATE);
  CHECK(cause(rejected.error(), TreeError::NODE_LIMIT));
  CHECK_FALSE(rejected.error().resource);
  CHECK(resolverCalls == 0);
  CHECK(toggle.node() == Node<Switch>{});
}

TEST_CASE("named Switch resource overlays preserve factory readonly and accept explicit false") {
  for (const auto body : {"", "checked=true", "readOnly=false"}) {
    ui::Ref<Switch> toggle;
    auto scene = ui::scene([&](ManagedScene &owner) {
      const auto model = take(owner.state(false));
      ui::SceneResourceOptions options;
      options.format = ui::SceneResourceFormat::CONFIGURATION;
      take(options.registry.add<Switch>("Bound", [=](ui::SceneResourceArguments) {
        return ui::toggle(toggle, "Network", model).settings({.readOnly = true});
      }));
      return ui::resource(std::string("scene { Bound { ") + body + " } }", options);
    });
    const auto ready = scene.prepareChecked();
    if (std::string_view(body) == "checked=true") {
      REQUIRE_FALSE(ready);
      CHECK(cause(ready.error(), TreeError::INVALID_ARGUMENT));
    } else {
      REQUIRE(ready);
      scene.configure([&](ManagedScene &, Application &app) {
        take(app.tree().read(toggle.node(), [&](const Switch &view) {
          CHECK(view.readOnly() == (std::string_view(body) != "readOnly=false"));
        }));
      });
    }
  }
}

TEST_CASE("named Switch factory initial conflicts survive empty and partial resource overlays") {
  for (bool modelValue : {false, true}) {
    for (const auto body : {"", "readOnly=false"}) {
      ui::Ref<Switch> toggle;
      auto scene = ui::scene([&](ManagedScene &owner) {
        const auto model = take(owner.state(modelValue));
        ui::SceneResourceOptions options;
        options.format = ui::SceneResourceFormat::CONFIGURATION;
        take(options.registry.add<Switch>("Bound", [=](ui::SceneResourceArguments) {
          return ui::toggle(toggle, "Network", model).settings({.state = CheckState::CHECKED, .readOnly = true});
        }));
        return ui::resource(std::string("scene { Bound { ") + body + " } }", options);
      });
      const auto ready = scene.prepareChecked();
      REQUIRE_FALSE(ready);
      CHECK(cause(ready.error(), TreeError::INVALID_ARGUMENT));
    }
  }
}

TEST_CASE("failed Switch prerequisite outranks settings and competing frames in both factory forms") {
  const ManagedError original{ManagedPhase::STATE, {}, TreeError::NODE_LIMIT, {}, {}};
  const std::expected<ManagedState<bool>, ManagedError> failed = std::unexpected(original);
  for (bool named : {false, true}) {
    for (int decoration : {0, 1, 2}) {
      ui::Ref<Switch> toggle;
      auto recipe = named ? ui::toggle(toggle, "Caption", failed) : ui::toggle("Caption", failed);
      if (decoration != 1) {
        recipe = std::move(recipe).settings({.readOnly = true});
      }
      if (decoration != 0) {
        recipe = std::move(recipe).framed("not-a-frame-pattern");
      }
      auto scene = ui::scene(std::move(recipe));
      const auto ready = scene.prepareChecked();
      REQUIRE_FALSE(ready);
      CHECK(ready.error().phase == original.phase);
      CHECK(cause(ready.error(), TreeError::NODE_LIMIT));
      CHECK(toggle.node() == Node<Switch>{});
    }
  }
}

TEST_CASE("named failed Switch prerequisite passes unchanged through resource erasure") {
  ui::Ref<Switch> toggle;
  const ManagedError original{ManagedPhase::STATE, {}, TreeError::NODE_LIMIT, {}, {}};
  const std::expected<ManagedState<bool>, ManagedError> failed = std::unexpected(original);
  ui::SceneResourceOptions options;
  options.format = ui::SceneResourceFormat::CONFIGURATION;
  take(options.registry.add<Switch>("Broken", [=](ui::SceneResourceArguments) {
    return ui::toggle(toggle, "Caption", failed).settings({.readOnly = true}).framed("not-a-frame-pattern");
  }));
  auto scene = ui::scene("scene { Broken#broken { readOnly=true } }", options);
  const auto ready = scene.prepareChecked();
  REQUIRE_FALSE(ready);
  CHECK(ready.error().phase == original.phase);
  CHECK(cause(ready.error(), TreeError::NODE_LIMIT));
  CHECK(toggle.node() == Node<Switch>{});
}
