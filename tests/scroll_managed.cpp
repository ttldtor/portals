// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../src/widgets/action_state.hpp"
#include "../src/widgets/scroll_controls_state.hpp"
#include "../src/widgets/scroll_preparation_state.hpp"
#include "support/examples/replay/terminal.hpp"
#include <portals/ui.hpp>

#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <source_location>
#include <thread>

namespace portals::detail {
// White-box exhaustion/completion receipts otherwise require infeasible counter loops.
struct ManagedScrollTestAccess {
  static auto state(ManagedScene &scene) {
    return scene.scroll_;
  }
};
} // namespace portals::detail

namespace allocation_test {
thread_local void (*hook)(){};
thread_local bool failOnce{};
} // namespace allocation_test

void *operator new(std::size_t size) {
  if (std::exchange(allocation_test::failOnce, false)) {
    throw std::bad_alloc();
  }
  if (auto hook = std::exchange(allocation_test::hook, nullptr)) {
    hook();
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

namespace {
using namespace portals;
int failures{};

void check(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    ++failures;
  }
}

template <class T, class E>
T take(std::expected<T, E> value, std::source_location where = std::source_location::current()) {
  if (!value) {
    std::cerr << "fixture failure at " << where.line() << '\n';
    std::abort();
  }
  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

struct Plane : Widget, Layout {
  std::expected<CellSize, TreeError> childAvailable(CellSize) const override {
    return CellSize{10, 100};
  }

  std::expected<CellSize, TreeError> measureChildren(std::span<const MeasuredChild>) const override {
    return CellSize{10, 100};
  }

  std::expected<std::vector<CellRect>, TreeError> arrangeChildren(std::span<const MeasuredChild> children,
                                                                  CellSize) const override {
    std::vector<CellRect> result(0);
    for (std::size_t i = 0; i < children.size(); ++i) {
      result.push_back(take(CellRect::from({0, i == 0 ? 0 : 95}, {10, 1})));
    }
    return result;
  }
};

struct MotionReplayTerminal : example::ReplayTerminal {
  using ReplayTerminal::ReplayTerminal;
  bool motion{true};

  PointerCaptureCapabilities pointerCaptureCapabilities() const noexcept override {
    return motion ? PointerCaptureCapabilities{true, true, true} : PointerCaptureCapabilities{};
  }
};

struct Fixture {
  MotionReplayTerminal terminal;
  std::unique_ptr<Application> app;
  std::unique_ptr<ManagedScene> scene;
  ui::Ref<ScrollArea> area;
  ui::Ref<Button> first;
  ui::Ref<Button> last;
  std::shared_ptr<ScrollController> controller;
  Node<Row> layout;

  explicit Fixture(std::vector<example::ReplayTerminal::Input> input = {}, ScrollAreaOptions options = {})
      : terminal(std::move(input), 5000, {10, 10}) {
    app = take(Application::create({}, terminal.clock()));
    scene = take(ManagedScene::create(*app));
    options.horizontal = options.vertical = ScrollBarPolicy::NEVER;
    layout = take(ui::mount(
      *scene, ui::row(ui::scrollArea(area, ui::custom<Plane>().setup([&](ManagedScene &owner, Node<Plane> node) {
        auto a = owner.emplace<Button>(node, take(GraphemeText::from("first")));
        if (!a) {
          return std::expected<void, ManagedError>(std::unexpected(a.error()));
        }
        auto b = owner.emplace<Button>(node, take(GraphemeText::from("last")));
        if (!b) {
          return std::expected<void, ManagedError>(std::unexpected(b.error()));
        }
        firstNode = *a;
        lastNode = *b;
        take(owner.onClick(*a, [] {
        }));
        take(owner.onClick(*b, [] {
        }));
        return std::expected<void, ManagedError>{};
      }),
                                     options))));
    take(app->tree().read(area.node(), [&](const ScrollArea &widget) {
      controller = detail::ScrollWidgetAccess::state(widget).controller;
    }));
    take(app->router().setFocus(firstNode));
  }

  Node<Button> firstNode, lastNode;

  ScrollOffset position() {
    return take(controller->snapshot()).vertical.offset;
  }
};

void runtimeOrderingAndIdle() {
  Fixture fixture({{100, "a", {}}, {200, "b", {}}, {300, "c", {}}, {400, "d", {}}});
  int input{};
  take(fixture.scene->onInput([&](const InputEvent &) -> std::expected<bool, ManagedError> {
    ++input;
    if (input == 1) {
      // A real input callback invokes the existing checked router path; ordinary scroll does not reveal focus again.
      take(fixture.app->router().dispatch(MouseInputEvent{1, 1, MouseButton::WHEEL_DOWN, MouseAction::SCROLL, {}}));
    } else if (input == 2) {
      check(fixture.position().raster == 3, "wheel offset survives unchanged initial focus");
      take(fixture.area.setOffset(ScrollAxis::VERTICAL, {20, 0}));
    } else if (input == 3) {
      check(fixture.position().raster == 20, "programmatic offset survives unchanged focus");
      take(fixture.app->router().setFocus(fixture.lastNode));
    } else {
      check(fixture.position().raster == 86, "new offviewport focus reveals after prior command");
    }
    return true;
  }));
  const auto result = take(fixture.scene->run(fixture.terminal));
  check(input == 4 && result.inputs == 4 && result.frames == 4 && fixture.terminal.closed,
        "four input events produce only initial/wheel/command/focus frames, no idle redraw");
  check(fixture.position().raster == 86, "final controller values remain owning after scene cleanup");
}

void commandFocusOrder(bool commandFirst) {
  Fixture fixture({{100, "a", {}}, {200, "b", {}}});
  int input{};
  take(fixture.scene->onInput([&](const InputEvent &) -> std::expected<bool, ManagedError> {
    if (++input == 1) {
      if (commandFirst) {
        take(fixture.area.setOffset(ScrollAxis::VERTICAL, {20, 0}));
      }
      take(fixture.app->router().setFocus(fixture.lastNode));
      if (!commandFirst) {
        take(fixture.area.setOffset(ScrollAxis::VERTICAL, {20, 0}));
      }
    } else {
      check(fixture.position().raster == (commandFirst ? 86u : 20u),
            "command then new focus reveals; focus then command suppresses the old request");
    }
    return true;
  }));
  take(fixture.scene->run(fixture.terminal));
}

void fractionAndExplicit() {
  ScrollAreaOptions options;
  options.wheelStep = .5;
  Fixture fixture({{100, "a", {}}, {200, "b", {}}, {300, "c", {}}, {400, "d", {}}}, options);
  int input{};
  take(fixture.scene->onInput([&](const InputEvent &) -> std::expected<bool, ManagedError> {
    ++input;
    if (input == 1) {
      take(fixture.app->router().setFocus(fixture.lastNode));
      auto moved =
        take(fixture.app->router().dispatch(MouseInputEvent{1, 1, MouseButton::WHEEL_DOWN, MouseAction::SCROLL, {}}));
      check(moved.status == DispatchStatus::HANDLED, "fractional wheel handled as logical intent");
    } else if (input == 2) {
      check(fixture.position().raster == 0 && fixture.position().fraction == .5,
            "fraction-only intent suppresses older focus reveal");
      take(fixture.scene->ensureVisible(fixture.lastNode));
      take(fixture.area.setOffset(ScrollAxis::VERTICAL, {2, 0}));
    } else if (input == 3) {
      check(fixture.position().raster == 2, "later logical command cancels explicit pending reveal");
      take(fixture.scene->ensureVisible(fixture.lastNode));
      take(fixture.area.setOffset(ScrollAxis::VERTICAL, {2, 0}));
    } else {
      check(fixture.position().raster == 86, "no-op command does not cancel explicit reveal");
    }
    return true;
  }));
  take(fixture.scene->run(fixture.terminal));
}

void scopedFocusOwnership() {
  auto app = take(Application::create());
  auto first = take(ManagedScene::create(*app));
  auto second = take(ManagedScene::create(*app));
  const auto a = take(first->emplace<ScrollArea>(first->root()));
  auto value = take(detail::EventRouterAccess::observeFocus(app->router()));
  std::weak_ptr<const detail::FocusObservation> weak = value;
  const auto b = take(second->emplace<ScrollArea>(second->root()));
  auto same = take(detail::EventRouterAccess::observeFocus(app->router()));
  check(value == same, "two scenes on one router share scoped focus identity");
  value.reset();
  same.reset();
  take(first->remove(a));
  check(!weak.expired(), "second scene retains value after first detaches");
  take(second->remove(b));
  check(weak.expired(), "last detach releases value despite router weak slot");
  auto renewed = take(detail::EventRouterAccess::observeFocus(app->router()));
  check(!renewed->retired, "expired weak slot permits new observation");
  auto scheduler = take(Scheduler::create());
  auto tree = take(WidgetTree::create(*scheduler));
  auto router = take(EventRouter::create(*tree));
  auto retired = take(detail::EventRouterAccess::observeFocus(*router));
  router.reset();
  check(retired->retired && !retired->generation && retired->focused == Node<Widget>{},
        "router destruction retires surviving owning value before teardown");
}

struct FailingChild : Widget {
  void mount() override {
    throw std::bad_alloc();
  }
};

void mountRollbackAndRefs() {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));
  ui::Ref<ScrollArea> failed;
  auto result = ui::mount(*scene, ui::scrollArea(failed, ui::custom<FailingChild>()));
  check(!result && failed.node() == Node<ScrollArea>{}, "failed subtree never publishes Ref");
  check(!scene->requiresPointerMotion(), "failed scroll mount does not publish a native input requirement");
  auto focus = take(detail::EventRouterAccess::observeFocus(app->router()));
  check(focus.use_count() == 1, "failed mount leaves no hidden scene focus owner");
  auto controller = take(ScrollController::create());
  ui::Ref<ScrollArea> area;
  ui::Ref<ScrollBar> projection;
  take(ui::mount(*scene, ui::row(ui::scrollArea(area, ui::label("content"), {}, controller),
                                 ui::scrollBar(projection, controller))));
  check(!projection.setMetrics({100, 10}), "shared factory chooses projection instead of competing writer");
  auto secondWriter = scene->emplace<ScrollArea>(scene->root(), controller);
  check(!secondWriter, "failed second writer preserves existing winner");
  take(area.setOffset(ScrollAxis::VERTICAL, {}));
  std::thread worker([&] {
    const auto denied = area.setOffset(ScrollAxis::VERTICAL, {});
    check(!denied && std::get<TreeError>(denied.error().cause) == TreeError::WRONG_THREAD,
          "Ref rejects offthread command before mutable owner access");
  });
  worker.join();
  take(scene->remove(area.node()));
  check(!area.setOffset(ScrollAxis::VERTICAL, {}), "stale Ref retains removed identity");
}

void automaticExhaustion() {
  Fixture fixture({{100, "a", {}}, {200, "b", {}}, {300, "c", {}}});
  int input{};
  auto focus = take(detail::EventRouterAccess::observeFocus(fixture.app->router()));
  take(fixture.scene->onInput([&](const InputEvent &) -> std::expected<bool, ManagedError> {
    ++input;
    if (input == 1) {
      take(fixture.app->router().setFocus(fixture.lastNode));
      // Private value fixture models the existing permanently exhausted generation stamp.
      const_cast<detail::FocusObservation &>(*focus).generation = 0;
    } else if (input == 2) {
      check(fixture.position().raster == 0, "exhausted generation cancels automatic reveal");
      take(fixture.scene->ensureVisible(fixture.lastNode));
    } else {
      check(fixture.position().raster == 86, "explicit reveal survives automatic identity exhaustion");
    }
    return true;
  }));
  take(fixture.scene->run(fixture.terminal));
}

EventRouter *reentryRouter{};
std::shared_ptr<const detail::FocusObservation> reentryWinner;
bool failAfterWinner{};

void observeAgain() {
  reentryWinner = take(detail::EventRouterAccess::observeFocus(*reentryRouter));
  if (failAfterWinner) {
    throw std::bad_alloc();
  }
}

void focusAcquisitionReentry() {
  for (const bool fail : {false, true}) {
    auto app = take(Application::create());
    reentryRouter = &app->router();
    failAfterWinner = fail;
    allocation_test::hook = observeAgain;
    auto outer = detail::EventRouterAccess::observeFocus(app->router());
    check(fail ? (!outer && outer.error() == TreeError::NO_MEMORY) : (outer && *outer == reentryWinner),
          "lazy focus allocation preserves reentrant winner and truthful outer error");
    check(take(detail::EventRouterAccess::observeFocus(app->router())) == reentryWinner,
          "failed outer acquisition cannot remove winning weak association");
    reentryWinner.reset();
  }
  reentryRouter = nullptr;
}

void standaloneCaptureAndRemoval() {
  MotionReplayTerminal terminal({{100, "a", {}}, {200, "b", {}}}, 5000, {10, 10});
  auto app = take(Application::create({}, terminal.clock()));
  auto scene = take(ManagedScene::create(*app));
  ui::Ref<ScrollBar> bar;
  take(ui::mount(*scene, ui::scrollBar(bar)));
  take(bar.setMetrics({100, 10}));
  std::shared_ptr<ScrollController> controller;
  take(app->tree().read(bar.node(), [&](const ScrollBar &widget) {
    controller = detail::ScrollWidgetAccess::state(widget).controller;
  }));
  int input{};
  take(scene->onInput([&](const InputEvent &) -> std::expected<bool, ManagedError> {
    if (++input == 1) {
      take(app->router().dispatch(MouseInputEvent{0, 0, MouseButton::LEFT, MouseAction::PRESS, {}}));
      take(app->router().dispatch(MouseInputEvent{30, 4, MouseButton::LEFT, MouseAction::MOVE, {}}));
      // The first move invalidates LAYOUT; the second must still use the same admitted anchor.
      take(app->router().dispatch(MouseInputEvent{30, 6, MouseButton::LEFT, MouseAction::MOVE, {}}));
      take(app->router().dispatch(MouseInputEvent{30, 6, MouseButton::LEFT, MouseAction::RELEASE, {}}));
    } else {
      check(take(controller->snapshot()).vertical.offset.raster == 60,
            "managed bar capture survives repeated outside moves");
      take(scene->remove(bar.node()));
      auto released = take(app->router().dispatch(MouseInputEvent{30, 9, MouseButton::LEFT, MouseAction::RELEASE, {}}));
      check(released.status == DispatchStatus::NO_TARGET, "removed bar has no stale capture delivery");
    }
    return true;
  }));
  const auto result = take(scene->run(terminal));
  check(result.frames == 3 && terminal.closed, "standalone capture/removal runs without idle redraw");

  Fixture removed({{100, "a", {}}});
  take(removed.scene->onInput([&](const InputEvent &) -> std::expected<bool, ManagedError> {
    const auto button = take(removed.scene->emplace<Button>(removed.area.node(), take(GraphemeText::from("remove"))));
    take(removed.scene->onClick(button, [&] {
      take(removed.scene->remove(removed.area.node()));
    }));
    take(removed.app->router().invoke(button, {Action::ACTIVATE}));
    return true;
  }));
  auto removedResult = take(removed.scene->run(removed.terminal));
  check(removedResult.frames == 2 && removed.terminal.closed,
        "admitted child callback can remove owning scroll subtree");
}

Fixture *lateFixture{};
bool lateFocus{};

void lateMutation() {
  if (lateFocus) {
    take(lateFixture->app->router().setFocus(lateFixture->firstNode));
  } else {
    take(setScrollOffset(lateFixture->app->tree(), lateFixture->area.node(), ScrollAxis::VERTICAL, {20, .5}));
  }
}

LayoutSnapshot layoutSnapshot(Fixture &fixture) {
  const auto bounds = take(CellRect::from({}, {10, 10}));
  std::optional<LayoutSnapshot> snapshot;
  take(fixture.app->tree().frame([&] {
    take(layoutTree(fixture.app->tree(), fixture.app->root(), bounds, bounds));
    snapshot.emplace(
      take(LayoutSnapshot::capture(fixture.app->tree(), fixture.app->root(), LayoutSnapshotProfile::GEOMETRY)));
  }));
  return std::move(*snapshot);
}

void allocationAdmissionOrdering() {
  for (const bool focus : {false, true}) {
    Fixture fixture;
    take(fixture.scene->prepare(*fixture.app, {10, 10}));
    for (int pass = 0; pass < 2; ++pass) {
      auto snapshot = layoutSnapshot(fixture);
      take(fixture.app->tree().consumeFrameRequest());
      take(fixture.scene->prepareViews(*fixture.app, snapshot));
    }
    take(fixture.area.setOffset(ScrollAxis::VERTICAL, {20, 0}));
    take(fixture.app->router().setFocus(fixture.lastNode));
    take(fixture.scene->prepare(*fixture.app, {10, 10}));
    auto snapshot = layoutSnapshot(fixture);
    take(fixture.app->tree().consumeFrameRequest());
    lateFixture = &fixture;
    lateFocus = focus;
    allocation_test::hook = lateMutation;
    take(fixture.scene->prepareViews(*fixture.app, snapshot));
    check(fixture.position().raster == 20, "post-allocation admission discards obsolete reveal before publishing");
    if (focus) {
      snapshot = layoutSnapshot(fixture);
      take(fixture.app->tree().consumeFrameRequest());
      take(fixture.scene->prepareViews(*fixture.app, snapshot));
      check(fixture.position().raster == 0, "next ordinary pass handles new focus within existing three-pass budget");
    } else {
      check(fixture.position().fraction == .5, "refresh does not overwrite late fractional logical command");
    }
  }
  lateFixture = nullptr;
}

void failedAutomaticAdmissionRetries() {
  Fixture fixture;
  take(fixture.app->router().setFocus(fixture.lastNode));
  auto snapshot = layoutSnapshot(fixture);
  allocation_test::failOnce = true;
  const auto failed = fixture.scene->prepareViews(*fixture.app, snapshot);
  check(!failed && failed.error() == TreeError::NO_MEMORY, "automatic request allocation reports typed error");
  take(fixture.scene->prepareViews(*fixture.app, snapshot));
  check(fixture.position().raster == 86,
        "failed automatic admission does not advance observed watermark and lose retry");
}

void focusChangesDuringRequestAllocation() {
  Fixture fixture;
  take(fixture.app->router().setFocus(fixture.lastNode));
  auto snapshot = layoutSnapshot(fixture);
  lateFixture = &fixture;
  lateFocus = true;
  allocation_test::hook = lateMutation;
  take(fixture.scene->prepareViews(*fixture.app, snapshot));
  check(fixture.position().raster == 0, "request allocation cannot combine old target with newer focus stamp");
  lateFixture = nullptr;
}

void nestedExplicit() {
  take(lateFixture->scene->ensureVisible(lateFixture->firstNode));
}

void reentrantExplicitWinner(bool settled) {
  Fixture fixture;
  take(fixture.scene->prepare(*fixture.app, {10, 10}));
  auto snapshot = layoutSnapshot(fixture);
  take(fixture.scene->prepareViews(*fixture.app, snapshot));
  if (settled) {
    snapshot = layoutSnapshot(fixture);
    take(fixture.scene->prepareViews(*fixture.app, snapshot));
    take(fixture.scene->completedLayout(*fixture.app));
    check(!detail::ManagedScrollTestAccess::state(*fixture.scene)->pending,
          "nested winner also starts from settled empty pending");
  }
  lateFixture = &fixture;
  allocation_test::hook = nestedExplicit;
  const auto outer = fixture.scene->ensureVisible(fixture.lastNode);
  check(!outer && std::get<TreeError>(outer.error().cause) == TreeError::WOULD_BLOCK,
        "outer explicit allocation preserves newer nested request admission");
  snapshot = layoutSnapshot(fixture);
  take(fixture.scene->prepareViews(*fixture.app, snapshot));
  check(fixture.position().raster == 0, "winning nested explicit target remains authoritative");
  lateFixture = nullptr;
}

void multiAreaIntentIsolation() {
  Fixture fixture({{100, "a", {}}, {200, "b", {}}});
  ScrollAreaOptions options;
  options.horizontal = options.vertical = ScrollBarPolicy::NEVER;
  const auto other = take(
    fixture.scene->emplace<ScrollArea>(fixture.layout, std::shared_ptr<ScrollController>{}, WidthPolicy{}, options));
  take(fixture.scene->emplace<Plane>(other));
  int input{};
  take(fixture.scene->onInput([&](const InputEvent &) -> std::expected<bool, ManagedError> {
    if (++input == 1) {
      take(fixture.app->router().setFocus(fixture.lastNode));
      take(fixture.scene->setScrollOffset(other, ScrollAxis::VERTICAL, {20, 0}));
    } else {
      check(fixture.position().raster == 86, "unrelated area intent cannot cancel focused area's pending request");
      take(fixture.app->tree().read(other, [&](const ScrollArea &widget) {
        check(take(detail::ScrollWidgetAccess::state(widget).controller->snapshot()).vertical.offset.raster == 20,
              "other area preserves its own logical command");
      }));
    }
    return true;
  }));
  const auto stats = take(fixture.scene->run(fixture.terminal));
  check(stats.frames == 2, "multi-area runtime adds no idle redraw");
}

void readOnlyRevealAndTheme() {
  ScrollAreaOptions options;
  options.readOnly = true;
  Fixture fixture({{100, "a", {}}, {200, "b", {}}, {300, "c", CellSize{10, 8}}, {400, "d", {}}}, options);
  int input{};
  take(fixture.scene->onInput([&](const InputEvent &) -> std::expected<bool, ManagedError> {
    ++input;
    if (input == 1) {
      take(fixture.app->router().setFocus(fixture.lastNode));
    } else if (input == 2) {
      check(fixture.position().raster == 86, "readOnly permits automatic focus reveal");
      take(fixture.area.setOffset(ScrollAxis::VERTICAL, {20, 0}));
      take(fixture.scene->theme(take(parsePss("ScrollArea { foreground: #010203; }"))));
    } else if (input == 4) {
      check(fixture.position().raster == 20, "unchanged-focus theme and viewport resize do not rearm reveal");
    }
    return true;
  }));
  take(fixture.scene->run(fixture.terminal));
}

struct NestedPlane : Plane {
  int row;
  int height;
  int *arrangements;

  NestedPlane(int row, int height, int *arrangements) : row(row), height(height), arrangements(arrangements) {
  }

  std::expected<void, TreeError> arrange(CellRect) override {
    ++*arrangements;
    return {};
  }

  std::expected<std::vector<CellRect>, TreeError> arrangeChildren(std::span<const MeasuredChild> children,
                                                                  CellSize) const override {
    return std::vector<CellRect>(children.size(), take(CellRect::from({0, row}, {10, height})));
  }
};

void nestedActualRunner() {
  for (const std::size_t depth : {3u, 16u}) {
    example::ReplayTerminal terminal({}, 5000, {10, 10});
    auto app = take(Application::create({}, terminal.clock()));
    auto scene = take(ManagedScene::create(*app));
    auto parent = scene->root();
    std::vector<std::shared_ptr<ScrollController>> controllers;
    std::vector<int> arrangements(depth, 0);
    ScrollAreaOptions options;
    options.horizontal = options.vertical = ScrollBarPolicy::NEVER;
    for (std::size_t level = 0; level < depth; ++level) {
      auto controller = take(ScrollController::create());
      const auto area = take(scene->emplace<ScrollArea>(parent, controller, WidthPolicy{}, options));
      parent = take(scene->emplace<NestedPlane>(area, level + 1 == depth ? 95 : 50, level + 1 == depth ? 1 : 10,
                                                &arrangements[level]));
      controllers.push_back(std::move(controller));
    }
    const auto target = take(scene->emplace<Button>(parent, take(GraphemeText::from("target"))));
    take(scene->onClick(target, [] {
    }));
    take(app->router().setFocus(target));
    const auto receipt = detail::ManagedScrollTestAccess::state(*scene);
    const auto result = take(scene->run(terminal));
    check(result.frames == 1 && terminal.closed, "actual nested runner settles and remains idle");
    check(receipt->consumedGeneration != 0 && receipt->consumedFocus == Node<Widget>(target) && !receipt->pending,
          "only successful completed final render advances consumed focus watermark");
    for (std::size_t level = 0; level < depth; ++level) {
      check(take(controllers[level]->snapshot()).vertical.offset.raster == (level + 1 == depth ? 86u : 50u),
            "actual managed runner adopts all nested final positions");
      check(arrangements[level] == 3, "two preliminary passes plus final rendering within unchanged budget");
    }
  }
}

void failedPreparationRetries() {
  Fixture fixture;
  take(fixture.scene->ensureVisible(fixture.lastNode));
  auto snapshot = layoutSnapshot(fixture);
  allocation_test::failOnce = true;
  const auto failed = fixture.scene->prepareViews(*fixture.app, snapshot);
  check(!failed && failed.error() == TreeError::NO_MEMORY, "pending request preparation failure remains typed");
  take(fixture.scene->prepareViews(*fixture.app, snapshot));
  check(fixture.position().raster == 86, "failed preparation keeps explicit request pending for retry");
}

struct FailingPaintPlane : Plane {
  std::expected<void, TreeError> paint(PaintContext &) override {
    return std::unexpected(TreeError::NODE_LIMIT);
  }
};

void failedFinalRender() {
  example::ReplayTerminal terminal({}, 5000, {10, 10});
  auto app = take(Application::create({}, terminal.clock()));
  auto scene = take(ManagedScene::create(*app));
  auto controller = take(ScrollController::create());
  ScrollAreaOptions options;
  options.horizontal = options.vertical = ScrollBarPolicy::NEVER;
  const auto area = take(scene->emplace<ScrollArea>(scene->root(), controller, WidthPolicy{}, options));
  const auto content = take(scene->emplace<FailingPaintPlane>(area));
  const auto first = take(scene->emplace<Button>(content, take(GraphemeText::from("first"))));
  const auto target = take(scene->emplace<Button>(content, take(GraphemeText::from("target"))));
  take(scene->onClick(first, [] {
  }));
  take(scene->onClick(target, [] {
  }));
  take(app->router().setFocus(target));
  const auto receipt = detail::ManagedScrollTestAccess::state(*scene);
  const auto result = scene->run(terminal);
  check(!result && result.error().application && result.error().application->phase == ApplicationPhase::RENDER,
        "final paint failure remains a render failure after reveal adoption");
  check(terminal.output.empty() && terminal.closed && take(controller->snapshot()).vertical.offset.raster == 86,
        "failed render submits no frame, preserves published values and cleans owned resources");
  check(receipt->consumedGeneration == 0 && !receipt->pending,
        "failed render never consumes request; stop cancels pending request without callbacks");
}

void explicitRootAdmission() {
  Fixture fixture;
  auto foreign = take(Application::create());
  auto wrong = fixture.scene->ensureVisible(fixture.lastNode, foreign->root());
  check(!wrong && std::get<TreeError>(wrong.error().cause) == TreeError::WRONG_SESSION,
        "explicit root keeps exact foreign-session error");
  const auto root = take(fixture.scene->emplace<Widget>(fixture.scene->root()));
  take(fixture.scene->remove(root));
  auto stale = fixture.scene->ensureVisible(fixture.lastNode, root);
  check(!stale && std::get<TreeError>(stale.error().cause) == TreeError::STALE_NODE,
        "explicit root keeps exact retired identity error");
  take(fixture.app->tree().setVisible(fixture.layout, false));
  auto hidden = fixture.scene->ensureVisible(fixture.lastNode, fixture.area.node());
  check(!hidden && std::get<TreeError>(hidden.error().cause) == TreeError::INVALID_ARGUMENT,
        "requested root cannot bypass effective hidden ancestry above it");
}

void hideDuringPlan() {
  take(lateFixture->app->tree().setVisible(lateFixture->area.node(), false));
}

void hiddenDuringAllocationPreservesPosition() {
  Fixture fixture;
  take(fixture.scene->prepare(*fixture.app, {10, 10}));
  for (int pass = 0; pass < 2; ++pass) {
    auto snapshot = layoutSnapshot(fixture);
    take(fixture.scene->prepareViews(*fixture.app, snapshot));
  }
  take(fixture.app->router().setFocus(fixture.lastNode));
  take(fixture.scene->prepare(*fixture.app, {10, 10}));
  auto snapshot = layoutSnapshot(fixture);
  lateFixture = &fixture;
  allocation_test::hook = hideDuringPlan;
  take(fixture.scene->prepareViews(*fixture.app, snapshot));
  check(fixture.position().raster == 0,
        "area hidden during plan allocation preserves old position until fresh snapshot");
  lateFixture = nullptr;
}

struct MutationScene : ApplicationScene {
  ManagedScene &managed;
  Node<Widget> unrelated;
  bool unstable;
  int passes{};

  MutationScene(ManagedScene &managed, Node<Widget> unrelated, bool unstable)
      : managed(managed), unrelated(unrelated), unstable(unstable) {
  }

  bool usesPreparedLayout() const noexcept override {
    return true;
  }

  bool usesGeometrySnapshot() const noexcept override {
    return managed.usesGeometrySnapshot();
  }

  std::expected<void, TreeError> prepare(Application &app, CellSize size) override {
    return managed.prepare(app, size);
  }

  std::expected<void, TreeError> prepareViews(Application &app, const LayoutSnapshot &snapshot) override {
    ++passes;
    if (unstable || passes == 1) {
      take(app.tree().setEnabled(unrelated, false));
      take(app.tree().setEnabled(unrelated, true));
    }
    return managed.prepareViews(app, snapshot);
  }

  std::expected<void, TreeError> completedLayout(Application &app) override {
    return managed.completedLayout(app);
  }

  void stop(Application &app) override {
    managed.stop(app);
  }
};

void conservativeRetryBudget() {
  for (const bool unstable : {false, true}) {
    Fixture fixture;
    const auto unrelated = take(fixture.scene->emplace<Widget>(fixture.layout));
    MutationScene adapter(*fixture.scene, unrelated, unstable);
    const auto result = fixture.app->run(adapter, fixture.terminal);
    if (unstable) {
      check(!result && result.error().phase == ApplicationPhase::PREPARE &&
              std::get<TreeError>(result.error().cause) == TreeError::WOULD_BLOCK && fixture.terminal.output.empty(),
            "deliberately unstable eligibility exhausts existing three passes without output");
    } else {
      check(result && result->frames == 1, "one unrelated rapid toggle retries then settles");
    }
    check(adapter.passes == 3 && fixture.terminal.closed, "conservative retry never raises default pass budget");
  }
}

void explicitAndIntentExhaustion() {
  Fixture fixture;
  take(fixture.scene->prepare(*fixture.app, {10, 10}));
  auto snapshot = layoutSnapshot(fixture);
  take(fixture.scene->prepareViews(*fixture.app, snapshot));
  take(fixture.scene->ensureVisible(fixture.lastNode));
  const auto receipt = detail::ManagedScrollTestAccess::state(*fixture.scene);
  const auto serial = receipt->pending->serial;
  receipt->explicitSerial = std::numeric_limits<std::uint64_t>::max();
  const auto denied = fixture.scene->ensureVisible(fixture.firstNode);
  check(!denied && std::get<TreeError>(denied.error().cause) == TreeError::IDENTITY_EXHAUSTED &&
          receipt->pending->serial == serial && receipt->pending->reveal.target == Node<Widget>(fixture.lastNode),
        "explicit serial exhaustion preserves older pending request without wrap");
  take(fixture.app->tree().read(fixture.area.node(), [&](const ScrollArea &widget) {
    detail::ScrollWidgetAccess::state(widget).intent.lock()->serial = std::numeric_limits<std::uint64_t>::max();
  }));
  take(fixture.area.setOffset(ScrollAxis::VERTICAL, {}));
  const auto command = fixture.area.setOffset(ScrollAxis::VERTICAL, {1, 0});
  check(!command && std::get<TreeError>(command.error().cause) == TreeError::IDENTITY_EXHAUSTED,
        "exhausted intent serial allows no-op but rejects logical movement before publication");
}

int lateEligibilityMode{};

void mutateEligibility() {
  auto &tree = lateFixture->app->tree();
  if (lateEligibilityMode == 0) {
    take(tree.setEnabled(lateFixture->layout, false));
  }
  if (lateEligibilityMode == 1) {
    take(tree.remove(lateFixture->layout));
  }
  if (lateEligibilityMode == 2) {
    take(tree.setVisible(lateFixture->layout, false));
    take(tree.setVisible(lateFixture->layout, true));
  }
  if (lateEligibilityMode == 3) {
    take(tree.setEnabled(lateFixture->firstNode, false));
    take(tree.setEnabled(lateFixture->firstNode, true));
  }
}

void eligibilityAllocationVariants() {
  for (int mutation = 0; mutation < 4; ++mutation) {
    Fixture fixture;
    take(fixture.scene->prepare(*fixture.app, {10, 10}));
    for (int pass = 0; pass < 2; ++pass) {
      auto snapshot = layoutSnapshot(fixture);
      take(fixture.scene->prepareViews(*fixture.app, snapshot));
    }
    take(fixture.app->router().setFocus(fixture.lastNode));
    take(fixture.scene->prepare(*fixture.app, {10, 10}));
    auto snapshot = layoutSnapshot(fixture);
    lateFixture = &fixture;
    lateEligibilityMode = mutation;
    allocation_test::hook = mutateEligibility;
    take(fixture.scene->prepareViews(*fixture.app, snapshot));
    check(fixture.position().raster == 0, "eligibility/removal reentry cannot publish old visible geometry");
    snapshot = layoutSnapshot(fixture);
    take(fixture.scene->prepareViews(*fixture.app, snapshot));
    check(fixture.position().raster == (mutation >= 2 ? 86u : 0u),
          "fresh snapshot resolves disabled/removed ancestors and conservative rapid-toggle retries");
  }
  lateFixture = nullptr;

  Fixture frame;
  auto snapshot = layoutSnapshot(frame);
  take(frame.app->tree().consumeFrameRequest());
  take(frame.app->tree().frame([&] {
    const auto denied = frame.scene->prepareViews(*frame.app, snapshot);
    check(!denied && denied.error() == TreeError::WOULD_BLOCK,
          "frame reentry error is not mistaken for stale-history retry");
  }));
  check(take(frame.app->tree().consumeFrameRequest()) == Invalidation::NONE,
        "unrelated WOULD_BLOCK does not secretly request another layout pass");
}

int admissionAllocation{};
int admissionMutation{};
bool admissionMutated{};

void mutateExplicitAdmission() {
  if (--admissionAllocation != 0) {
    allocation_test::hook = mutateExplicitAdmission;
    return;
  }
  auto &tree = lateFixture->app->tree();
  if (admissionMutation == 0) {
    take(tree.setVisible(lateFixture->lastNode, false));
  }
  if (admissionMutation == 1) {
    take(tree.setVisible(lateFixture->layout, false));
  }
  if (admissionMutation == 2) {
    take(tree.remove(lateFixture->lastNode));
  }
  if (admissionMutation == 3) {
    take(tree.remove(lateFixture->layout));
  }
  admissionMutated = true;
}

void explicitFinalAdmissionAllocationBoundaries(bool settled) {
  for (int mutation = 0; mutation < 4; ++mutation) {
    int exercised{};
    for (int boundary = 1; boundary <= 64; ++boundary) {
      Fixture fixture;
      take(fixture.scene->prepare(*fixture.app, {10, 10}));
      for (int pass = 0; pass < (settled ? 2 : 1); ++pass) {
        auto snapshot = layoutSnapshot(fixture);
        take(fixture.scene->prepareViews(*fixture.app, snapshot));
      }
      if (settled) {
        take(fixture.scene->completedLayout(*fixture.app));
      } else {
        take(fixture.scene->ensureVisible(fixture.firstNode));
      }
      const auto owner = detail::ManagedScrollTestAccess::state(*fixture.scene);
      const auto serial = owner->explicitSerial;
      const auto pendingSerial = owner->pending ? owner->pending->serial : 0;
      check(static_cast<bool>(owner->pending) != settled, "admission sweep starts with the intended pending state");
      lateFixture = &fixture;
      admissionAllocation = boundary;
      admissionMutation = mutation;
      admissionMutated = false;
      allocation_test::hook = mutateExplicitAdmission;
      const auto result = fixture.scene->ensureVisible(fixture.lastNode);
      allocation_test::hook = nullptr;
      if (admissionMutated) {
        ++exercised;
        const auto expected = mutation < 2 ? TreeError::INVALID_ARGUMENT : TreeError::STALE_NODE;
        check(!result && std::get<TreeError>(result.error().cause) == expected,
              "final explicit admission rejects hidden/retired target or ancestor with exact typed error");
        const bool preserved = settled ? !owner->pending
                                       : owner->pending && owner->pending->serial == pendingSerial &&
                                           owner->pending->reveal.target == Node<Widget>(fixture.firstNode);
        check(owner->explicitSerial == serial && preserved,
              "failed final admission preserves previous pending and explicit serial");
      } else {
        check(static_cast<bool>(result), "unmutated explicit request remains admissible");
      }
      lateFixture = nullptr;
    }
    check(exercised > 0 && exercised < 64,
          "fixture covers all fallible admission allocation boundaries on this toolchain");
  }
}

void explicitAllocationFailurePreservesPending() {
  for (const bool settled : {false, true}) {
    Fixture fixture;
    take(fixture.scene->prepare(*fixture.app, {10, 10}));
    for (int pass = 0; pass < 2; ++pass) {
      auto snapshot = layoutSnapshot(fixture);
      take(fixture.scene->prepareViews(*fixture.app, snapshot));
    }
    take(fixture.scene->completedLayout(*fixture.app));
    if (!settled) {
      take(fixture.scene->ensureVisible(fixture.firstNode));
    }
    const auto owner = detail::ManagedScrollTestAccess::state(*fixture.scene);
    const auto serial = owner->explicitSerial;
    const auto *pending = owner->pending ? &*owner->pending : nullptr;

    allocation_test::failOnce = true;

    const auto result = fixture.scene->ensureVisible(fixture.lastNode);

    check(!result && std::get<TreeError>(result.error().cause) == TreeError::NO_MEMORY,
          "explicit request holder allocation reports typed NO_MEMORY");
    check(owner->explicitSerial == serial && (owner->pending ? &*owner->pending : nullptr) == pending,
          "explicit allocation failure retains empty or engaged pending and its serial");
  }
}

void preAdmissionLogicalCommand() {
  take(setScrollOffset(lateFixture->app->tree(), lateFixture->area.node(), ScrollAxis::VERTICAL, {20, .5}));
}

void explicitAdmissionLinearizationAndDisabled() {
  Fixture fixture;
  take(fixture.scene->prepare(*fixture.app, {10, 10}));
  auto snapshot = layoutSnapshot(fixture);
  take(fixture.scene->prepareViews(*fixture.app, snapshot));
  lateFixture = &fixture;
  allocation_test::hook = preAdmissionLogicalCommand;
  take(fixture.scene->ensureVisible(fixture.lastNode));
  allocation_test::hook = nullptr;
  lateFixture = nullptr;
  snapshot = layoutSnapshot(fixture);
  take(fixture.scene->prepareViews(*fixture.app, snapshot));
  check(fixture.position().raster == 86,
        "explicit request admitted after earlier nested command remains authoritative");
  take(fixture.scene->ensureVisible(fixture.lastNode));
  take(fixture.area.setOffset(ScrollAxis::VERTICAL, {20, .5}));
  snapshot = layoutSnapshot(fixture);
  take(fixture.scene->prepareViews(*fixture.app, snapshot));
  check(fixture.position().raster == 20 && fixture.position().fraction == .5,
        "logical command after explicit admission cancels the intersecting request");

  Fixture disabled;
  take(disabled.app->tree().setEnabled(disabled.layout, false));
  take(disabled.scene->ensureVisible(disabled.lastNode));
  snapshot = layoutSnapshot(disabled);
  take(disabled.scene->prepareViews(*disabled.app, snapshot));
  check(disabled.position().raster == 86, "disabled but visible ancestry still permits programmatic explicit reveal");
}

void managedInputProfileAndNativeDrag() {
  auto app = take(Application::create());
  auto scene = take(ManagedScene::create(*app));

  allocation_test::failOnce = true;

  const auto ordinary = scene->requiresPointerMotion();
  const auto noAllocation = allocation_test::failOnce;

  allocation_test::failOnce = false;
  check(!ordinary && noAllocation, "ordinary scene input-profile query allocates nothing and stays false");

  ScrollAreaOptions readOnly;
  readOnly.readOnly = true;

  const auto area =
    take(scene->emplace<ScrollArea>(scene->root(), std::shared_ptr<ScrollController>{}, WidthPolicy{}, readOnly));

  take(app->tree().setVisible(area, false));
  take(app->tree().setEnabled(area, false));
  check(scene->requiresPointerMotion(), "mounted hidden/disabled/read-only scroll subsystem requests input profile");
  take(scene->remove(area));
  check(!scene->requiresPointerMotion(), "last scroll removal retires its input requirement");

  for (const bool motion : {false, true}) {
    MotionReplayTerminal terminal({{100, "\x1b[<0;1;1M", {}},
                                   {200, "\x1b[<32;31;5M", {}},
                                   {300, "\x1b[<32;31;7M", {}},
                                   {400, "\x1b[<0;31;7m", {}},
                                   {500, "check", {}}},
                                  1000, {10, 10});
    terminal.motion = motion;

    auto runner = take(Application::create({}, terminal.clock()));
    auto managed = take(ManagedScene::create(*runner));
    ui::Ref<ScrollBar> bar;

    take(ui::mount(*managed, ui::scrollBar(bar)));
    take(bar.setMetrics({100, 10}));
    check(managed->requiresPointerMotion(), "standalone bar also requests the session input profile");

    std::shared_ptr<ScrollController> controller;

    take(runner->tree().read(bar.node(), [&](const ScrollBar &widget) {
      controller = detail::ScrollWidgetAccess::state(widget).controller;
    }));
    int interceptedMoves{};

    take(managed->onInput([&](const InputEvent &event) -> std::expected<bool, ManagedError> {
      if (const auto *mouse = std::get_if<MouseInputEvent>(&event)) {
        if (mouse->action == MouseAction::PRESS) {
          return false;
        }
        if (mouse->action == MouseAction::MOVE) {
          ++interceptedMoves;
        }
      }

      return true;
    }));

    const auto stats = take(managed->run(terminal));

    check(take(controller->snapshot()).vertical.offset.raster == (motion ? 60u : 0u),
          "ordinary runner routes repeated outside drag using declared host capability");
    check(interceptedMoves == (motion ? 0 : 2) && stats.frames <= 5 && terminal.closed,
          "captured continuation bypasses interceptor without adding idle frames");
  }
}

void lateMountKeepsOpenedProfile() {
  MotionReplayTerminal terminal({{100, "mount", {}},
                                 {200, "\x1b[<0;1;1M", {}},
                                 {300, "\x1b[<32;31;7M", {}},
                                 {400, "\x1b[<0;31;7m", {}},
                                 {500, "\x1b[<65;1;1M", {}}},
                                1000, {10, 10});
  terminal.motion = false;

  auto app = take(Application::create({}, terminal.clock()));
  auto scene = take(ManagedScene::create(*app));
  ui::Ref<ScrollBar> bar;
  std::shared_ptr<ScrollController> controller;

  check(!scene->requiresPointerMotion(), "scene has no scroll requirement before late mount");
  take(scene->onInput([&](const InputEvent &event) -> std::expected<bool, ManagedError> {
    if (bar.node() == Node<ScrollBar>{}) {
      take(ui::mount(*scene, ui::scrollBar(bar)));
      take(bar.setMetrics({100, 10}));
      take(app->tree().read(bar.node(), [&](const ScrollBar &widget) {
        controller = detail::ScrollWidgetAccess::state(widget).controller;
      }));
      check(scene->requiresPointerMotion(), "late mount records its requirement without changing the transport");
    }
    if (const auto *mouse = std::get_if<MouseInputEvent>(&event)) {
      return mouse->action != MouseAction::PRESS && mouse->action != MouseAction::SCROLL;
    }

    return true;
  }));
  take(scene->run(terminal));
  check(controller && take(controller->snapshot()).vertical.offset.raster == 3,
        "late-mounted bar preserves wheel input while unavailable motion cannot start a drag");
}

void liveTextAndScrollSamePrepare() {
  for (const bool explicitRequest : {false, true}) {
    MotionReplayTerminal terminal({{100, "a", {}}, {300, "b", {}}}, 1000, {40, 10});
    auto app = take(Application::create({}, terminal.clock()));
    auto scene = take(ManagedScene::create(*app));
    auto text = take(scene->state(std::string("one two")));
    ui::Ref<ScrollArea> area;
    ui::Ref<LineEdit> input;
    ui::Ref<VirtualList<TextSegment>> list;
    Node<Button> first, last;
    ScrollAreaOptions options;
    options.horizontal = options.vertical = ScrollBarPolicy::NEVER;

    take(ui::mount(
      *scene,
      ui::row(
        ui::scrollArea(
          area,
          ui::custom<Plane>().setup([&](ManagedScene &owner, Node<Plane> plane) -> std::expected<void, ManagedError> {
            first = take(owner.emplace<Button>(plane, take(GraphemeText::from("first"))));
            last = take(owner.emplace<Button>(plane, take(GraphemeText::from("last"))));
            take(owner.onClick(first, [] {
            }));
            take(owner.onClick(last, [] {
            }));

            return {};
          }),
          options),
        ui::column(ui::lineEdit(input, text), ui::virtualList(ui::words(input.draftText())).reference(list).height(3))
          .width(10))));
    take(app->router().setFocus(first));

    std::shared_ptr<ScrollController> controller;

    take(app->tree().read(area.node(), [&](const ScrollArea &widget) {
      controller = detail::ScrollWidgetAccess::state(widget).controller;
    }));

    auto live = take(scene->liveText(input.draftText()));
    int custom{}, expectedCustom{}, callbacks{}, inputs{};
    bool armed{};
    std::uint64_t latestGeneration{}, admittedSerial{};

    take(scene->onPrepare(input.node(), [&](LineEdit &, Node<Widget>) {
      ++custom;

      return std::expected<void, TreeError>{};
    }));
    take(scene->observe(live, [&](const GraphemeText &value) {
      if (!armed) {
        return;
      }
      armed = false;
      ++callbacks;
      check(custom == expectedCustom && value.bytes() == "three four five",
            "live text capture follows this prepare's single custom sync");
      take(app->router().setFocus(last));

      const auto receipt = detail::ManagedScrollTestAccess::state(*scene);

      latestGeneration = receipt->focus->generation;
      if (explicitRequest) {
        take(scene->ensureVisible(last));
        admittedSerial = receipt->explicitSerial;
      }
    }));
    take(scene->onInput([&](const InputEvent &) -> std::expected<bool, ManagedError> {
      if (++inputs == 1) {
        expectedCustom = custom + 1;
        armed = true;
        take(text.set("three four five"));
        check(armed, "live capture waits for presentation sync rather than observing the model setter early");
      } else if (inputs == 2) {
        check(callbacks == 1 && custom == expectedCustom,
              "P15 recovery does not repeat arbitrary custom sync while admitting P18 reveal");

        const auto receipt = detail::ManagedScrollTestAccess::state(*scene);

        check(latestGeneration && receipt->observedGeneration == latestGeneration && receipt->observedFocus == last,
              "scroll preparation adopts the latest focus generation produced by live-text recovery");
        check(explicitRequest ? admittedSerial && receipt->consumedExplicit == admittedSerial
                              : receipt->consumedGeneration == latestGeneration && receipt->consumedFocus == last,
              "automatic and explicit reveal consume only the admitted latest request");
        check(take(controller->snapshot()).vertical.offset.raster == 86,
              "same-frame live text recovery reveals the newly focused off-viewport child");
        take(app->tree().read(list.node(), [&](const VirtualList<TextSegment> &view) {
          check(view.visibleRange().count == 3 && view.firstRow() == 0,
                "live-text collection reaches the list without an imposed outer scroll offset");
        }));
      }

      return true;
    }));

    const auto stats = take(scene->run(terminal));

    check(callbacks == 1 && custom == 2 && stats.frames == 2 && terminal.closed,
          "combined adapter/reveal flow settles with one updated frame and no idle redraw");
  }
}
} // namespace

int main() {
  runtimeOrderingAndIdle();
  commandFocusOrder(true);
  commandFocusOrder(false);
  fractionAndExplicit();
  scopedFocusOwnership();
  mountRollbackAndRefs();
  automaticExhaustion();
  focusAcquisitionReentry();
  standaloneCaptureAndRemoval();
  allocationAdmissionOrdering();
  failedAutomaticAdmissionRetries();
  focusChangesDuringRequestAllocation();
  reentrantExplicitWinner(false);
  reentrantExplicitWinner(true);
  multiAreaIntentIsolation();
  readOnlyRevealAndTheme();
  nestedActualRunner();
  failedPreparationRetries();
  failedFinalRender();
  explicitRootAdmission();
  hiddenDuringAllocationPreservesPosition();
  conservativeRetryBudget();
  explicitAndIntentExhaustion();
  eligibilityAllocationVariants();
  explicitFinalAdmissionAllocationBoundaries(false);
  explicitFinalAdmissionAllocationBoundaries(true);
  explicitAllocationFailurePreservesPending();
  explicitAdmissionLinearizationAndDisabled();
  managedInputProfileAndNativeDrag();
  lateMountKeepsOpenedProfile();
  liveTextAndScrollSamePrepare();
  return failures ? 1 : 0;
}
