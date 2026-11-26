// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/ui_scene_resource.hpp>
#include <portals/ui_style.hpp>

#include <cstdlib>
#include <new>
#include <stdexcept>
#include <thread>

namespace allocationTest {
thread_local std::ptrdiff_t remaining = -1;
thread_local void (*hook)(void *){};
thread_local void *context{};
} // namespace allocationTest

void *operator new(std::size_t size) {
  if (auto callback = std::exchange(allocationTest::hook, nullptr)) {
    callback(allocationTest::context);
  }
  if (allocationTest::remaining == 0) {
    throw std::bad_alloc();
  }
  if (allocationTest::remaining > 0) {
    --allocationTest::remaining;
  }
  if (auto *result = std::malloc(size ? size : 1)) {
    return result;
  }
  throw std::bad_alloc();
}

void operator delete(void *value) noexcept {
  std::free(value);
}

void operator delete(void *value, std::size_t) noexcept {
  std::free(value);
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete[](void *value) noexcept {
  ::operator delete(value);
}

void operator delete[](void *value, std::size_t) noexcept {
  ::operator delete(value);
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

void operator delete(void *value, const std::nothrow_t &) noexcept {
  ::operator delete(value);
}

void operator delete[](void *value, const std::nothrow_t &) noexcept {
  ::operator delete(value);
}

namespace {
using namespace portals;
static_assert(!std::is_copy_constructible_v<Spinner>);

template <class T, class E> T take(std::expected<T, E> result) {
  if (!result) {
    throw std::runtime_error("checked spinner fixture operation failed");
  }
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

struct Fixture {
  SchedulerTime now{};
  std::unique_ptr<Application> app = take(Application::create({}, {[](void *context) noexcept {
                                                                     return *static_cast<SchedulerTime *>(context);
                                                                   },
                                                                   &now}));
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));

  void prepare(CellSize size = {30, 5}) {
    take(scene->prepare(*app, size));
    const auto bounds = take(CellRect::from({}, size));
    take(app->tree().consumeFrameRequest());
    std::optional<LayoutSnapshot> snapshot;
    take(app->tree().frame([&] {
      take(layoutTree(app->tree(), app->root(), bounds, bounds));
      snapshot.emplace(take(LayoutSnapshot::capture(app->tree(), app->root())));
    }));
    take(scene->prepareViews(*app, *snapshot));
    take(scene->completedLayout(*app));
    take(app->tree().consumeFrameRequest());
  }

  SchedulerDispatch advance(SchedulerTime time) {
    now = time;
    auto dispatched = take(app->scheduler().dispatch());
    take(scene->afterDispatch(*app));
    return dispatched;
  }

  std::size_t frame(Node<Spinner> node) {
    std::size_t value{};
    take(app->tree().read(node, [&](const Spinner &spinner) {
      value = spinner.frameIndex();
    }));
    return value;
  }

  SpinnerOptions options(Node<Spinner> node) {
    std::optional<SpinnerOptions> value;
    take(app->tree().read(node, [&](const Spinner &spinner) {
      value.emplace(spinner.options());
    }));
    return *value;
  }
};
} // namespace

TEST_CASE("spinner uses one existing timer and resumes without catch-up") {
  Fixture f;
  auto node = take(ui::mount(*f.scene, ui::spinner()));
  CHECK(f.scene->usesPreparedLayout());
  f.prepare();
  CHECK(f.advance(99).timers == 0);
  CHECK(f.advance(100).timers == 1);
  CHECK(f.frame(node) == 1);
  CHECK(take(f.app->tree().consumeFrameRequest()) == Invalidation::PAINT);
  f.prepare({15, 3});
  f.prepare({20, 4});
  CHECK(f.advance(200).timers == 1);

  take(f.app->tree().setVisible(node, false));
  f.prepare();
  CHECK(f.advance(10000).timers == 0);
  CHECK(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  take(f.app->tree().setVisible(node, true));
  f.prepare();
  CHECK(f.advance(10099).timers == 0);
  CHECK(f.advance(10100).timers == 1);
  CHECK(f.frame(node) == 0);
  CHECK(f.advance(50000).timers == 1);
  CHECK(f.frame(node) == 1);
  CHECK(f.advance(50000).timers == 0);

  take(f.app->tree().setEnabled(node, false));
  f.prepare();
  CHECK(f.advance(60000).timers == 0);
  take(f.app->tree().setEnabled(node, true));
  f.prepare();
  CHECK(f.advance(60100).timers == 1);
  take(f.scene->remove(node));
  take(f.app->tree().consumeFrameRequest());
  CHECK(f.advance(90000).timers == 0);
  CHECK(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE);
}

TEST_CASE("fully clipped indicator pauses until ordinary layout reveals it") {
  Fixture f;
  ui::Ref<Spinner> spinner;
  take(ui::mount(*f.scene, ui::vbox(ui::label("above").height(3), ui::spinner(spinner).height(1))));
  f.prepare({10, 1});
  CHECK(f.advance(1000).timers == 0);
  CHECK(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  f.prepare({10, 5});
  CHECK(f.advance(1099).timers == 0);
  CHECK(f.advance(1100).timers == 1);
  CHECK(f.frame(spinner.node()) == 1);
}

TEST_CASE("inactive reduced-motion and single-frame indicators remain idle") {
  for (const auto mode : {0, 1, 2}) {
    Fixture f;
    SpinnerOptions options;
    options.active = mode != 0;
    options.reducedMotion = mode == 1;
    if (mode == 2) {
      options.frames = "static";
    }
    auto node = take(ui::mount(*f.scene, ui::spinner(options)));
    f.prepare();
    CHECK(f.advance(100000).timers == 0);
    CHECK(f.frame(node) == 0);
    CHECK(take(f.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  }
}

TEST_CASE("runtime rate and direction replacement cancels previous registration atomically") {
  Fixture f;
  auto node = take(ui::mount(*f.scene, ui::spinner()));
  f.prepare();
  auto config = f.options(node);
  config.interval = 250;
  config.direction = SpinnerDirection::REVERSE;
  take(setSpinner(f.app->tree(), node, config));
  f.prepare();
  CHECK(f.advance(100).timers == 0);
  CHECK(f.advance(250).timers == 1);
  CHECK(f.frame(node) == 2);
  auto invalid = config;
  invalid.frames = "first\n";
  CHECK(setSpinner(f.app->tree(), node, invalid).error() == TreeError::INVALID_ARGUMENT);
  CHECK(f.advance(500).timers == 1);
  CHECK(f.frame(node) == 1);

  std::optional<TreeError> foreign;
  std::thread worker([&] {
    foreign = setSpinner(f.app->tree(), node, config).error();
  });
  worker.join();
  CHECK(foreign == TreeError::WRONG_THREAD);
}

TEST_CASE("finite owning frames validate UTF-8 geometry and complete budgets") {
  for (std::size_t i = 0; i < SPINNER_PRESET_NAMES.size(); ++i) {
    SpinnerOptions options;
    options.preset = static_cast<SpinnerPreset>(i);
    CHECK(Spinner::validate({}, options));
    WidthPolicy wide;
    wide.ambiguous = AmbiguousWidth::WIDE;
    CHECK(Spinner::validate(wide, options));
    Fixture animated;
    auto node = take(ui::mount(*animated.scene, ui::spinner(options)));
    animated.prepare();
    CHECK(animated.advance(100).timers == 1);
    CHECK(animated.frame(node) == 1);
  }
  WidthPolicy wide;
  wide.ambiguous = AmbiguousWidth::WIDE;
  auto circle = take(GraphemeText::from("\u25d0", wide));
  REQUIRE(circle.measure() == 2);
  SpinnerOptions fallback;
  fallback.preset = SpinnerPreset::CIRCLE;
  fallback.maxFrameBytes = 7;
  fallback.maxFrames = 4;
  fallback.maxFrameCells = 1;
  CHECK(Spinner::validate(wide, fallback));
  fallback.frames = "\u25d0";
  CHECK_FALSE(Spinner::validate(wide, fallback));
  fallback.frames.clear();
  auto invalidWidth = wide;
  invalidWidth.ambiguous = static_cast<AmbiguousWidth>(-1);
  CHECK(Spinner::validate(invalidWidth, fallback).error() == TreeError::INVALID_ARGUMENT);

  SpinnerOptions custom;
  custom.frames = "x\nwide";
  Fixture f;
  auto recipe = ui::spinner(custom).label("Loading").alignment(LayoutAlign::END);
  custom.frames = "changed";
  auto node = take(ui::mount(*f.scene, std::move(recipe)));
  CHECK(f.options(node).frames == "x\nwide");
  CellSize intrinsic;
  take(f.app->tree().edit(node, [&](Spinner &spinner) {
    intrinsic = take(spinner.measure({}));
  }));
  CHECK(intrinsic == CellSize{12, 1});
  for (const auto bad : {"x\n", "\nx", "x\n\ny", "\x1b", "\xff"}) {
    custom.frames = bad;
    CHECK(Spinner::validate({}, custom).error() == TreeError::INVALID_ARGUMENT);
  }
  custom.frames = "x\ny";
  custom.maxFrames = 1;
  CHECK(Spinner::validate({}, custom).error() == TreeError::NODE_LIMIT);
  custom.maxFrames = 2;
  custom.maxFrameBytes = 2;
  CHECK(Spinner::validate({}, custom).error() == TreeError::NODE_LIMIT);
  custom.maxFrameBytes = 10;
  custom.frames = "wide";
  custom.maxFrameCells = 3;
  CHECK(Spinner::validate({}, custom).error() == TreeError::NODE_LIMIT);
  custom.interval = 0;
  CHECK(Spinner::validate({}, custom).error() == TreeError::INVALID_ARGUMENT);
}

TEST_CASE("resource method pipe and defaults share the indicator schema") {
  Fixture f;
  ui::SceneResourceOptions resource;
  resource.format = ui::SceneResourceFormat::CONFIGURATION;
  auto owner = ui::scene(R"(scene { Spinner#loading {
    preset=LINE frames="a\nbb" interval=250 direction=REVERSE alignment=CENTER
    active=false reducedMotion=true label="Wait" maxFrames=8 maxFrameBytes=32 maxFrameCells=4 maxPaintCells=100
  } })",
                         resource);
  std::optional<SpinnerOptions> observed;
  auto indicator = owner.get<Spinner>("loading");
  owner.configure([&](ManagedScene &, Application &app) {
    take(app.tree().read(indicator.node(), [&](const Spinner &spinner) {
      observed.emplace(spinner.options());
    }));
  });
  take(owner.prepareChecked());
  auto config = *observed;
  CHECK(config.preset == SpinnerPreset::LINE);
  CHECK(config.frames == "a\nbb");
  CHECK(config.interval == 250);
  CHECK(config.direction == SpinnerDirection::REVERSE);
  CHECK(config.alignment == LayoutAlign::CENTER);
  CHECK_FALSE(config.active);
  CHECK(config.reducedMotion);
  CHECK(config.maxFrames == 8);
  CHECK(config.maxFrameBytes == 32);
  CHECK(config.maxFrameCells == 4);
  CHECK(config.maxPaintCells == 100);

  Fixture fluent;
  auto node = take(ui::mount(
    *fluent.scene, ui::spinner() | ui::preset(SpinnerPreset::LINE) | ui::frames("a\nbb") | ui::interval(250) |
                     ui::direction(SpinnerDirection::REVERSE) | ui::alignment(LayoutAlign::CENTER) | ui::active(false) |
                     ui::reducedMotion(true) | ui::spinnerLabel(TextValue("Wait")) | ui::maxFrames(8) |
                     ui::maxFrameBytes(32) | ui::maxFrameCells(4) | ui::maxPaintCells(100)));
  CHECK(fluent.options(node).frames == config.frames);
  CHECK(fluent.options(node).interval == config.interval);
  CHECK(fluent.options(node).alignment == config.alignment);

  Fixture defaults;
  ui::ControlContext controls;
  controls.defaults("Spinner", {.spinnerTick = 333, .spinnerActive = false});
  auto inherited = take(ui::mount(*defaults.scene, ui::spinner(), controls));
  CHECK(defaults.options(inherited).interval == 333);
  CHECK_FALSE(defaults.options(inherited).active);
}

TEST_CASE("spinner preset names and owning initializer frames preserve checked errors") {
  ui::Ref<Spinner> reference;
  auto oldDefault = ui::spinner({});
  auto oldRefDefault = ui::spinner(reference, {});
  auto oldPreset = ui::spinner().preset({});
  auto emptyFrames = ui::spinner().frames({});
  (void)oldDefault;
  (void)oldRefDefault;
  (void)oldPreset;
  Fixture empty;
  auto reset = take(ui::mount(*empty.scene, std::move(emptyFrames)));
  CHECK(empty.options(reset).frames.empty());

  Fixture named;
  auto node = take(ui::mount(*named.scene, ui::spinner("arc").frames({"[   ]", "[=  ]"}).interval(120)));
  CHECK(named.options(node).preset == SpinnerPreset::ARC);
  CHECK(named.options(node).frames == "[   ]\n[=  ]");
  CHECK(named.options(node).interval == 120);
  for (const auto bad : {std::string_view{}, std::string_view{"a\nb"}, std::string_view{"a\rb"}}) {
    Fixture invalid;
    CHECK_FALSE(ui::mount(*invalid.scene, ui::spinner().frames({bad})));
  }
  Fixture unknown;
  CHECK_FALSE(ui::mount(*unknown.scene, ui::spinner("unknown")));
  CHECK(spinnerPreset("bar-bounce") == SpinnerPreset::BAR_BOUNCE);
  for (auto name : SPINNER_PRESET_NAMES) {
    ui::SceneResourceOptions resource;
    resource.format = ui::SceneResourceFormat::CONFIGURATION;
    auto owner = ui::scene("scene{Spinner{preset=" + std::string(name) + "}}", resource);
    CHECK(owner.prepareChecked());
  }
  ui::SceneResourceOptions resource;
  resource.format = ui::SceneResourceFormat::CONFIGURATION;
  auto owner = ui::scene("scene{Spinner{preset=bar-bounce}}", resource);
  CHECK(owner.prepareChecked());
}

TEST_CASE("resource invalid leaves report source paths before setup") {
  for (const auto property : {"interval=0", "alignment=STRETCH", "active=other", "frames=\"x\\n\"", "maxFrames=0",
                              "preset=OTHER", "unknown=1", "interval=1 interval=2"}) {
    Fixture f;
    ui::SceneResourceOptions resource;
    resource.format = ui::SceneResourceFormat::CONFIGURATION;
    auto owner = ui::scene("scene{Spinner{" + std::string(property) + "}}", resource);
    auto mounted = owner.prepareChecked();
    REQUIRE_FALSE(mounted);
    REQUIRE(mounted.error().sceneResource);
    CHECK(static_cast<bool>(mounted.error().sceneResource->propertyPath));
  }
}

TEST_CASE("replacement owning allocations fail without changing current animation") {
  Fixture f;
  auto node = take(ui::mount(*f.scene, ui::spinner()));
  f.prepare();
  SpinnerOptions replacement;
  replacement.frames = "custom frame one\ncustom frame two\ncustom frame three";
  replacement.label = TextValue("custom owning label longer than small string storage");
  bool success{};
  for (std::ptrdiff_t fail = 0; fail < 256 && !success; ++fail) {
    allocationTest::remaining = fail;
    auto changed = setSpinner(f.app->tree(), node, replacement);
    allocationTest::remaining = -1;
    if (!changed) {
      CHECK(changed.error() == TreeError::NO_MEMORY);
      CHECK(f.options(node).frames.empty());
    } else {
      success = true;
      CHECK(f.options(node).frames == replacement.frames);
    }
  }
  CHECK(success);
}

TEST_CASE("mount allocation failures retire newly owned sequence and registrations") {
  bool success{};
  std::size_t failures{};
  for (std::ptrdiff_t fail = 0; fail < 512 && !success; ++fail) {
    Fixture f;
    auto recipe = ui::spinner().frames("custom first\ncustom second").label("Owning loading label");
    allocationTest::remaining = fail;
    auto mounted = ui::mount(*f.scene, std::move(recipe));
    allocationTest::remaining = -1;
    if (!mounted) {
      ++failures;
      REQUIRE(std::get_if<TreeError>(&mounted.error().cause));
      CHECK(std::get<TreeError>(mounted.error().cause) == TreeError::NO_MEMORY);
      CHECK(take(f.app->tree().inspect(f.scene->root())).children.empty());
      CHECK(f.advance(10000).timers == 0);
    } else {
      success = true;
    }
  }
  CHECK(success);
  CHECK(failures > 0);
}

TEST_CASE("localization removal preserves stale identity and cancels animation") {
  Fixture f;
  auto node = take(ui::mount(*f.scene, ui::spinner()));
  f.prepare();
  SpinnerOptions config;
  config.label = i18n("loading");
  auto changed =
    setSpinner(f.app->tree(), node, config, [&](std::string_view) -> std::expected<std::string, TextResourceError> {
      take(f.app->tree().remove(node));
      return "Loading";
    });
  CHECK_FALSE(changed);
  CHECK(changed.error() == TreeError::STALE_NODE);
  CHECK(f.advance(10000).timers == 0);

  Fixture missing;
  auto failed = ui::mount(*missing.scene, ui::spinner().label(i18n("missing")));
  REQUIRE_FALSE(failed);
  CHECK(failed.error().resource == TextResourceError::RESOLVER_REQUIRED);
}

TEST_CASE("timer registration allocation may retire the scene without retaining a dangling callback") {
  Fixture f;
  const auto node = take(ui::mount(*f.scene, ui::spinner()));
  f.prepare();
  allocationTest::context = &f;
  allocationTest::hook = [](void *context) {
    static_cast<Fixture *>(context)->scene.reset();
  };
  f.now = 100;
  auto dispatched = f.app->scheduler().dispatch();
  allocationTest::hook = nullptr;
  allocationTest::context = nullptr;
  REQUIRE(dispatched);
  CHECK(dispatched->timers == 1);
  CHECK_FALSE(f.scene);
  CHECK(f.app->tree()
          .read(node,
                [](const Spinner &) {
                })
          .error() == TreeError::STALE_NODE);
  f.now = 10000;
  CHECK(take(f.app->scheduler().dispatch()).timers == 0);
}

TEST_CASE("checked Spinner reference starts stops and stages labels with existing error collection") {
  Fixture f;
  ui::Ref<Spinner> spinner;
  auto node = take(ui::mount(*f.scene, ui::spinner(spinner)));
  f.prepare();
  CHECK(f.advance(100).timers == 1);
  take(spinner.stop());
  CHECK_FALSE(f.options(node).active);
  f.prepare();
  CHECK(f.advance(10000).timers == 0);
  take(spinner.start());
  f.prepare();
  CHECK(f.advance(10099).timers == 0);
  CHECK(f.advance(10100).timers == 1);
  CHECK(f.frame(node) == 2);
  take(spinner.label("Done"));
  CHECK(f.frame(node) == 0);
  CHECK(take(f.options(node).label.resolve({})).bytes() == "Done");

  const auto longLabel = "This owning label must allocate outside small-string storage";
  allocationTest::remaining = 0;
  auto noMemory = spinner.label(longLabel);
  allocationTest::remaining = -1;
  REQUIRE_FALSE(noMemory);
  CHECK(std::get<TreeError>(noMemory.error().cause) == TreeError::NO_MEMORY);
  CHECK(take(f.options(node).label.resolve({})).bytes() == "Done");

  auto localized =
    spinner.label(i18n("loading"), [](std::string_view key) -> std::expected<std::string, TextResourceError> {
      CHECK(key == "loading");
      return "Localized";
    });
  REQUIRE(localized);
  CHECK(take(f.options(node).label.resolve({})).bytes() == "Localized");

  std::optional<ManagedError> foreign;
  std::thread thread([&] {
    allocationTest::remaining = 0;
    auto failed = spinner.label(longLabel);
    allocationTest::remaining = -1;
    foreign = failed.error();
  });
  thread.join();
  REQUIRE(foreign);
  CHECK(std::get<TreeError>(foreign->cause) == TreeError::WRONG_THREAD);

  take(f.scene->remove(node));
  auto stale = spinner.stop();
  REQUIRE_FALSE(stale);
  CHECK(std::get<TreeError>(stale.error().cause) == TreeError::STALE_NODE);
  CHECK(stale.error().node == node.id());
  const auto diagnostics = take(f.scene->diagnostics());
  REQUIRE_FALSE(diagnostics.errors.empty());
  CHECK(diagnostics.errors.back().error.operation == std::string_view("spinner.stop"));
}

TEST_CASE("managed and DSL spinner use natural style identity and respect explicit overrides") {
  constexpr auto SHEET = "Spinner { foreground: #123456; } Spinner:disabled { foreground: #654321; } "
                         "Override { foreground: #abcdef; }";

  struct Sink : PaintContext {
    std::optional<CellStyle> frameStyle;

    std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
      for (const auto &command : commands) {
        if (const auto *run = std::get_if<TextCommand>(&command); run && !frameStyle) {
          frameStyle = run->style;
        }
      }
      return {};
    }
  };

  Fixture managed;
  auto node = take(managed.scene->spinner(managed.scene->root()));
  take(managed.scene->theme(take(parsePss(SHEET))));
  managed.prepare();
  take(managed.app->tree().read(node, [&](const Spinner &spinner) {
    REQUIRE(spinner.computedStyle());
    CHECK(spinner.computedStyle()->normal.foreground == RgbColor{0x12, 0x34, 0x56});
    CHECK(spinner.computedStyle()->disabled.foreground == RgbColor{0x65, 0x43, 0x21});
  }));
  Sink active;
  take(managed.app->tree().frame([&] {
    take(managed.app->tree().paint(node, active));
  }));
  REQUIRE(active.frameStyle);
  CHECK(active.frameStyle->foreground == RgbColor{0x12, 0x34, 0x56});
  take(setSpinnerActive(managed.app->tree(), node, false));
  managed.prepare();
  Sink stopped;
  take(managed.app->tree().frame([&] {
    take(managed.app->tree().paint(node, stopped));
  }));
  REQUIRE(stopped.frameStyle);
  CHECK(stopped.frameStyle->foreground == RgbColor{0x65, 0x43, 0x21});
  take(managed.scene->style(node, StyleTarget{.type = "Override"}));
  managed.prepare();
  take(managed.app->tree().read(node, [&](const Spinner &spinner) {
    REQUIRE(spinner.computedStyle());
    CHECK(spinner.computedStyle()->normal.foreground == RgbColor{0xab, 0xcd, 0xef});
  }));

  ui::Ref<Spinner> reference;
  auto owner = ui::scene(ui::spinner(reference).active(false)) | ui::style(SHEET);
  bool inspected{};
  owner.configure([&](ManagedScene &, Application &application) {
    take(application.tree().read(reference.node(), [&](const Spinner &spinner) {
      REQUIRE(spinner.computedStyle());
      CHECK(spinner.computedStyle()->normal.foreground == RgbColor{0x12, 0x34, 0x56});
      CHECK(spinner.computedStyle()->disabled.foreground == RgbColor{0x65, 0x43, 0x21});
      inspected = true;
    }));
  });
  take(owner.prepareChecked());
  CHECK(inspected);
  take(owner.close());
}

TEST_CASE("alignment manual active and dim appearances and paint budget use retained text") {
  struct Sink : PaintContext {
    struct Run {
      std::string text;
      CellPoint origin;
      CellStyle style;
    };

    std::vector<Run> runs;

    std::expected<void, TreeError> submit(std::span<const RenderCommand> commands) override {
      for (const auto &command : commands) {
        if (const auto *run = std::get_if<TextCommand>(&command)) {
          runs.push_back({std::string(run->text.bytes()), run->origin, run->style});
        }
      }
      return {};
    }
  };

  Fixture f;
  SpinnerOptions options;
  options.frames = "x\nwide";
  options.label = TextValue("OK");
  options.alignment = LayoutAlign::CENTER;
  ControlStyle style;
  style.normal.foreground = RgbColor{1, 2, 3};
  style.disabled.foreground = RgbColor{4, 5, 6};
  auto node = take(ui::mount(*f.scene, ui::spinner(options, style).width(20).height(1)));
  f.prepare({20, 1});
  Sink normal;
  take(f.app->tree().frame([&] {
    take(f.app->tree().paint(node, normal));
  }));
  REQUIRE(normal.runs.size() == 2);
  CHECK(normal.runs[0].text == "x");
  CHECK(normal.runs[0].origin == CellPoint{6, 0});
  CHECK(normal.runs[1].origin == CellPoint{11, 0});
  CHECK(normal.runs[0].style.foreground == style.normal.foreground);
  options.reducedMotion = true;
  take(setSpinner(f.app->tree(), node, options));
  f.prepare({20, 1});
  Sink reduced;
  take(f.app->tree().frame([&] {
    take(f.app->tree().paint(node, reduced));
  }));
  REQUIRE(reduced.runs.size() == 2);
  CHECK(reduced.runs[0].style.foreground == style.disabled.foreground);
  CHECK(reduced.runs[1].style.foreground == style.normal.foreground);
  options.maxPaintCells = 19;
  take(setSpinner(f.app->tree(), node, options));
  f.prepare({20, 1});
  Sink capped;
  take(f.app->tree().frame([&] {
    auto painted = f.app->tree().paint(node, capped);
    CHECK_FALSE(painted);
    CHECK(painted.error() == TreeError::WOULD_BLOCK);
  }));
  CHECK(capped.runs.empty());
}

TEST_CASE("resource label localization resolves once before retained publication") {
  ui::SceneResourceOptions options;
  options.format = ui::SceneResourceFormat::CONFIGURATION;
  std::size_t resolved{};
  options.resolver = [&](std::string_view key) -> std::expected<std::string, TextResourceError> {
    CHECK(key == "loading");
    ++resolved;
    return "Localized";
  };
  auto scene = ui::scene("scene{Spinner#loading{label=i18n(\"loading\")}}", options);
  const auto spinner = scene.get<Spinner>("loading");
  scene.configure([&](ManagedScene &, Application &app) {
    take(app.tree().read(spinner.node(), [&](const Spinner &value) {
      CHECK(take(value.options().label.resolve({})).bytes() == "Localized");
    }));
  });
  take(scene.prepareChecked());
  take(scene.prepareChecked());
  CHECK(resolved == 1);
}

TEST_CASE("Ref label resolver may remove its scene before staged publication") {
  Fixture f;
  ui::Ref<Spinner> spinner;
  take(ui::mount(*f.scene, ui::spinner(spinner)));
  f.prepare();
  auto changed = spinner.label(i18n("loading"), [&](std::string_view) -> std::expected<std::string, TextResourceError> {
    f.scene.reset();
    return "Too late";
  });
  REQUIRE_FALSE(changed);
  CHECK(std::get<TreeError>(changed.error().cause) == TreeError::STALE_NODE);
  CHECK_FALSE(f.scene);
  f.now = 10000;
  CHECK(take(f.app->scheduler().dispatch()).timers == 0);
}
