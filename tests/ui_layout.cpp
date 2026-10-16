// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "support/examples/replay/terminal.hpp"
#include <portals/ui.hpp>

#include <memory>
#include <new>
#include <stdexcept>
#include <thread>

using namespace portals;

namespace {
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
} // namespace

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
