// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "support/examples/replay/terminal.hpp"
#include <portals/ui.hpp>

#include <cstdlib>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <thread>

namespace allocationTest {
thread_local std::ptrdiff_t remaining = -1;
thread_local std::size_t count{};
} // namespace allocationTest

void *operator new(std::size_t size) {
  if (allocationTest::remaining == 0) {
    throw std::bad_alloc();
  }
  if (allocationTest::remaining > 0) {
    --allocationTest::remaining;
  }
  ++allocationTest::count;
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
  try {
    return ::operator new[](size);
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void *value, const std::nothrow_t &) noexcept {
  ::operator delete(value);
}

void operator delete[](void *value, const std::nothrow_t &) noexcept {
  ::operator delete(value);
}

namespace {
using namespace portals;
constexpr CellSize VIEWPORT{30, 4};

void check(bool condition, std::string_view message, std::source_location where = std::source_location::current()) {
  if (!condition) {
    throw std::runtime_error(std::string(message) + " at line " + std::to_string(where.line()));
  }
}

template <class T, class E>
T take(std::expected<T, E> result, std::source_location where = std::source_location::current()) {
  if (!result) {
    if constexpr (std::same_as<E, ManagedError>) {
      if (const auto *cause = std::get_if<TreeError>(&result.error().cause)) {
        std::cerr << "managed phase=" << static_cast<int>(result.error().phase)
                  << " tree cause=" << static_cast<int>(*cause) << '\n';
      }
    }
  }
  check(result.has_value(), "unexpected API failure", where);
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

bool treeCause(const ManagedError &failure, TreeError cause) {
  const auto *value = std::get_if<TreeError>(&failure.cause);
  return value && *value == cause;
}

struct Fixture {
  example::ReplayTerminal terminal{{}, 5000, VIEWPORT};
  std::unique_ptr<Application> app;
  std::unique_ptr<ManagedScene> scene;

  explicit Fixture(ManagedOptions managed = {}, std::size_t capacity = 8) {
    ApplicationOptions options;
    options.scheduler.data_capacity = capacity;
    app = take(Application::create(options, terminal.clock()));
    scene = take(ManagedScene::create(*app, managed));
  }

  void prepare() {
    take(scene->prepare(*app, VIEWPORT));
  }

  void dispatch() {
    take(app->scheduler().dispatch());
  }

  Node<Widget> node() {
    return take(scene->emplace<Widget>(scene->root()));
  }
};

class UnenteredTerminal final : public ApplicationReadyTerminal {
public:
  mutable int calls{};

  TerminalRead poll(std::stop_token) override {
    ++calls;
    return {TerminalStatus::END_OF_INPUT};
  }

  std::expected<CellSize, TerminalError> size() const override {
    ++calls;
    return VIEWPORT;
  }

  std::expected<void, TerminalError> submit(std::string) override {
    ++calls;
    return {};
  }

  TerminalWrite flushReady(std::stop_token) override {
    ++calls;
    return {};
  }

  std::expected<void, TerminalError> close() noexcept override {
    ++calls;
    return {};
  }
};

void initialAndQueuedDelivery() {
  Fixture fixture;
  auto state = take(fixture.scene->state(1));
  auto first = fixture.node();
  auto second = fixture.node();
  int firstValue{}, secondValue{}, firstCalls{}, secondCalls{};
  take(fixture.scene->observe(first, state, [&](const int &value) {
    firstValue = value;
    ++firstCalls;
  }));
  take(fixture.scene->observe(second, state, [&](const int &value) {
    secondValue = value;
    ++secondCalls;
  }));
  check(firstCalls == 0 && secondCalls == 0, "registration defers initial delivery");
  take(state.set(2));
  fixture.dispatch();
  check(firstCalls == 0, "queued delivery before initial preparation is suppressed");
  fixture.prepare();
  check(firstValue == 2 && secondValue == 2 && firstCalls == 1 && secondCalls == 1,
        "both receivers initialize from latest snapshot");
  take(state.set(2));
  fixture.dispatch();
  fixture.prepare();
  check(firstCalls == 1 && secondCalls == 1, "equal set and clean prepare do not redeliver");
  take(state.set(3));
  take(state.set(4));
  fixture.dispatch();
  fixture.prepare();
  check(firstValue == 4 && secondValue == 4 && firstCalls == 2 && secondCalls == 2,
        "superseded notifications coalesce without rolling values back");
  take(fixture.app->tree().consumeFrameRequest());
  fixture.dispatch();
  fixture.prepare();
  check(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE,
        "settled observers do not self-invalidate");
}

void declarativeMount() {
  for (const bool observedFirst : {true, false}) {
    Fixture fixture;
    auto state = take(fixture.scene->state(7));
    ui::Ref<Label> output;
    int calls{};
    auto observed = ui::label("observer").observe(state, [&, output](const int &value) mutable {
      check(output.node() != Node<Label>{} && value == 7, "all sibling references publish before initial callback");
      ++calls;
    });
    auto target = ui::label("output").reference(output);
    if (observedFirst) {
      take(ui::mount(*fixture.scene, ui::column(std::move(observed), std::move(target))));
    } else {
      take(ui::mount(*fixture.scene, ui::column(std::move(target), std::move(observed))));
    }
    check(calls == 0, "description setup does not invoke callback");
    fixture.prepare();
    check(calls == 1, "description initial callback delivered once");
  }

  Fixture fixture;
  auto state = take(fixture.scene->state(0));
  int calls{};
  UnenteredTerminal ready;
  auto outer = ui::label("outer")
                 .observe(state,
                          [&](const int &) {
                            ++calls;
                          })
                 .setup([&](ManagedScene &scene, Node<Label>) -> std::expected<void, ManagedError> {
                   auto premature = scene.prepare(*fixture.app, VIEWPORT);
                   check(!premature && premature.error() == TreeError::WOULD_BLOCK, "prepare rejects partial mount");
                   auto run = scene.run(fixture.terminal);
                   auto start = scene.start(ready);
                   check(!run && treeCause(run.error(), TreeError::WOULD_BLOCK) && !start &&
                           treeCause(start.error(), TreeError::WOULD_BLOCK) && ready.calls == 0,
                         "run and session start reject partial mount before touching transport");
                   take(ui::mount(scene, ui::label("nested").observe(state, [&](const int &) {
                     ++calls;
                   })));
                   take(state.set(1));
                   fixture.dispatch();
                   premature = scene.prepare(*fixture.app, VIEWPORT);
                   check(!premature && premature.error() == TreeError::WOULD_BLOCK && calls == 0,
                         "nested mount cannot release outer mount guard");
                   return {};
                 });
  take(ui::mount(*fixture.scene, std::move(outer)));
  fixture.prepare();
  check(calls == 2, "outermost publication releases both initial callbacks");

  Fixture plain;
  take(ui::mount(*plain.scene,
                 ui::label("plain").setup([&](ManagedScene &scene, Node<Label>) -> std::expected<void, ManagedError> {
                   take(scene.prepare(*plain.app, VIEWPORT));
                   return {};
                 })));
}

void rejectedAndFixedPoint() {
  Fixture fixture({}, 1);
  auto early = take(fixture.scene->state(0));
  auto late = take(fixture.scene->state(0));
  int seenEarly{}, earlyCalls{}, lateCalls{};
  take(fixture.scene->observe(early, [&](const int &value) {
    seenEarly = value;
    ++earlyCalls;
  }));
  take(fixture.scene->observe(late, [&](const int &value) {
    ++lateCalls;
    if (value) {
      const auto changed = take(early.set(value * 2));
      check(changed.rejected == 1, "later observer changes earlier while its notification is rejected");
    }
  }));
  fixture.prepare();
  take(early.set(1));
  const auto update = take(late.set(3));
  check(update.rejected == 1 && update.changed, "set commits despite full notification queue");
  take(fixture.app->tree().consumeFrameRequest());
  fixture.prepare();
  check(seenEarly == 6 && earlyCalls == 3 && lateCalls == 2,
        "recovery reaches current snapshots before runner consumes dirty");
  fixture.dispatch();
  fixture.prepare();
  check(earlyCalls == 3 && lateCalls == 2, "stale queued notification does not repeat recovered revisions");
}

void boundedRecovery() {
  for (const int target : {15, 16}) {
    Fixture fixture({}, 1);
    auto state = take(fixture.scene->state(0));
    int calls{};
    take(fixture.scene->observe(state, [&](const int &value) {
      ++calls;
      if (value < target) {
        take(state.set(value + 1));
        fixture.dispatch();
      }
      auto blocked = fixture.scene->observe(state, [](const int &) {
      });
      check(!blocked && treeCause(blocked.error(), TreeError::WOULD_BLOCK), "recovery rejects registration");
      auto close = fixture.scene->close();
      check(!close && treeCause(close.error(), TreeError::WOULD_BLOCK), "recovery rejects close");
      auto prepare = fixture.scene->prepare(*fixture.app, VIEWPORT);
      check(!prepare && prepare.error() == TreeError::WOULD_BLOCK, "recovery rejects nested prepare");
    }));
    auto result = fixture.scene->prepare(*fixture.app, VIEWPORT);
    check(calls == 16, "recursive dispatch never reinvokes the active record");
    check(target == 15 ? result.has_value() : (!result && result.error() == TreeError::WOULD_BLOCK),
          "last permitted recovery pass succeeds exactly when converged");
    take(fixture.app->tree().consumeFrameRequest());
    fixture.dispatch();
    check(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE,
          "exhaustion does not install idle retry");
  }
}

void removalAndErrors() {
  for (const int phase : {0, 1, 2}) {
    Fixture fixture({}, 1);
    auto state = take(fixture.scene->state(0));
    auto parent = fixture.node();
    auto node = take(fixture.scene->emplace<Widget>(parent));
    int calls{};
    int destroyed{};

    struct Capture {
      int &destroyed;

      ~Capture() {
        ++destroyed;
      }
    };

    auto capture = std::make_shared<Capture>(destroyed);
    take(fixture.scene->observe(node, state, [&, capture](const int &value) -> std::expected<void, TreeError> {
      ++calls;
      if ((phase == 0 && value == 0) || value == 2) {
        take(fixture.scene->remove(parent));
        check(destroyed == 0, "admitted callable survives ancestor retirement");
        return std::unexpected(TreeError::INVALID_GEOMETRY);
      }
      return {};
    }));
    capture.reset();
    if (phase == 0) {
      auto prepared = fixture.scene->prepare(*fixture.app, VIEWPORT);
      check(!prepared && prepared.error() == TreeError::INVALID_GEOMETRY, "initial failure preserves exact error");
    } else {
      fixture.prepare();
      take(state.set(1));
      take(state.set(2));
      if (phase == 1) {
        fixture.dispatch();
      } else {
        auto prepared = fixture.scene->prepare(*fixture.app, VIEWPORT);
        check(!prepared && prepared.error() == TreeError::INVALID_GEOMETRY, "recovery error survives removal");
      }
    }
    check(destroyed == 1, "callable retires after the last admitted invocation returns");
    const auto errors = take(fixture.scene->diagnostics()).errors;
    check(!errors.empty() && errors.back().error.node == node.id() &&
            treeCause(errors.back().error, TreeError::INVALID_GEOMETRY) &&
            errors.back().error.phase == (phase == 1 ? ManagedPhase::INPUT : ManagedPhase::PREPARE),
          "exact original receiver and phase survive retirement");
    take(state.set(3));
    fixture.dispatch();
    check(calls == (phase == 0 ? 1 : 2), "retired callback never delivers again");
  }
}

void admissionAndLifetime() {
  ManagedOptions options;
  options.maxObservers = 2;
  options.maxObserversPerNode = 1;
  Fixture fixture(options);
  Fixture other;
  auto state = take(fixture.scene->state(0));
  auto node = fixture.node();
  auto alien = take(other.scene->state(0));
  auto rejected = fixture.scene->observe(node, alien, [](const int &) {
  });
  check(!rejected && treeCause(rejected.error(), TreeError::INVALID_ARGUMENT), "foreign source is rejected");
  take(fixture.scene->observe(node, state, [](const int &) {
  }));
  rejected = fixture.scene->observe(node, state, [](const int &) {
  });
  check(!rejected && treeCause(rejected.error(), TreeError::NODE_LIMIT), "per receiver budget is independent");
  take(fixture.scene->observe(state, [](const int &) {
  }));
  rejected = fixture.scene->observe(fixture.node(), state, [](const int &) {
  });
  check(!rejected && treeCause(rejected.error(), TreeError::NODE_LIMIT), "scene observer budget is bounded");
  take(fixture.app->tree().remove(node));
  take(fixture.scene->observe(fixture.node(), state, [](const int &) {
  }));
  std::thread worker([&] {
    auto wrong = fixture.scene->observe(state, [](const int &) {
    });
    check(!wrong && treeCause(wrong.error(), TreeError::WRONG_THREAD), "registration checks UI affinity");
  });
  worker.join();
  fixture.prepare();
  take(state.set(1));
  fixture.scene.reset();
  fixture.dispatch();
  check(!state.snapshot() && state.snapshot().error() == TreeError::CLOSED, "weak source expires with scene");

  Fixture bounded;
  auto source = take(bounded.scene->state(0, Invalidation::PAINT, PropertyOptions{1}));
  auto receiver = bounded.node();
  take(bounded.scene->observe(receiver, source, [](const int &) {
  }));
  auto capacity = bounded.scene->observe(source, [](const int &) {
  });
  check(!capacity && treeCause(capacity.error(), TreeError::NODE_LIMIT),
        "existing Property endpoint capacity remains independent from managed budgets");
  take(bounded.scene->remove(receiver));
  take(bounded.scene->observe(source, [](const int &) {
  }));
}

void registrationConstructionReentry() {
  Fixture fixture;
  auto state = take(fixture.scene->state(1));
  auto node = fixture.node();

  struct Reentrant {
    std::function<void()> onCopy;

    explicit Reentrant(std::function<void()> action) : onCopy(std::move(action)) {
    }

    Reentrant(const Reentrant &other) : onCopy(other.onCopy) {
      onCopy();
    }

    void operator()(const int &) {
    }
  } callback([&] {
    take(fixture.scene->remove(node));
  });

  const auto result = fixture.scene->observe(node, state, callback);
  check(!result && treeCause(result.error(), TreeError::STALE_NODE),
        "capture reentry rechecks receiver before publication");
  take(state.set(2));
  fixture.dispatch();
  fixture.prepare();
}

void constructionAndCallbackFailures() {
  for (const auto cause : {TreeError::NO_MEMORY, TreeError::CALLBACK_FAILED}) {
    Fixture fixture;
    auto state = take(fixture.scene->state(0));

    struct ThrowingCapture {
      TreeError cause;

      explicit ThrowingCapture(TreeError error) : cause(error) {
      }

      ThrowingCapture(const ThrowingCapture &other) : cause(other.cause) {
        if (cause == TreeError::NO_MEMORY) {
          throw std::bad_alloc();
        }
        throw std::runtime_error("capture construction");
      }

      void operator()(const int &) {
      }
    } callback(cause);

    auto rejected = fixture.scene->observe(state, callback);
    check(!rejected && treeCause(rejected.error(), cause), "capture construction preserves typed exception mapping");
    int calls{};
    take(fixture.scene->observe(state, [&](const int &) -> std::expected<void, TreeError> {
      if (++calls == 1) {
        return std::unexpected(cause);
      }
      return {};
    }));
    auto initial = fixture.scene->prepare(*fixture.app, VIEWPORT);
    check(!initial && initial.error() == cause, "initial callback failure belongs to PREPARE");
    fixture.prepare();
    fixture.prepare();
    check(calls == 2, "explicit later prepare retries failed callback but successful revision is not repeated");
  }

  ManagedOptions options;
  options.maxObservers = 1;
  Fixture fixture(options);
  auto state = take(fixture.scene->state(0));
  bool reentered{};

  struct FillCapacity {
    Fixture &fixture;
    ManagedState<int> state;
    bool &reentered;

    FillCapacity(Fixture &owner, ManagedState<int> source, bool &entered)
        : fixture(owner), state(source), reentered(entered) {
    }

    FillCapacity(const FillCapacity &other) : fixture(other.fixture), state(other.state), reentered(other.reentered) {
      if (!std::exchange(reentered, true)) {
        take(fixture.scene->observe(state, [](const int &) {
        }));
      }
    }

    void operator()(const int &) {
    }
  } callback(fixture, state, reentered);

  auto result = fixture.scene->observe(state, callback);
  check(!result && treeCause(result.error(), TreeError::NODE_LIMIT),
        "reentrant construction rechecks finite registry capacity");
  fixture.prepare();
}

void hiddenDisabledAndSnapshotLifetime() {
  Fixture fixture;
  auto state = take(fixture.scene->state(0));
  auto first = fixture.node();
  auto second = fixture.node();
  int calls{}, secondCalls{};
  take(fixture.scene->observe(first, state, [&](const int &value) {
    const int enteredValue = value;
    ++calls;
    if (value == 1) {
      take(state.set(2));
      auto nested = fixture.app->scheduler().dispatch();
      check(!nested && nested.error() == SchedulerError::REENTRANT_DISPATCH,
            "ordinary delivery preserves scheduler reentry rejection");
      check(value == enteredValue, "immutable entered snapshot survives replacing the source");
      take(fixture.scene->remove(second));
    }
  }));
  take(fixture.scene->observe(second, state, [&](const int &) {
    ++secondCalls;
  }));
  fixture.prepare();
  take(fixture.app->tree().setVisible(first, false));
  take(fixture.app->tree().setEnabled(first, false));
  take(state.set(1));
  fixture.dispatch();
  fixture.prepare();
  check(calls == 3, "hidden and disabled receivers still synchronize and suppressed nested updates recover");
  const auto before = secondCalls;
  take(state.set(3));
  fixture.dispatch();
  fixture.prepare();
  check(secondCalls == before, "removed receiver is suppressed in every delivery path");
  auto replacement = fixture.node();
  take(fixture.scene->observe(replacement, state, [&](const int &) {
  }));
  take(state.set(4));
  fixture.dispatch();
  fixture.prepare();
  check(replacement != second && secondCalls == before, "stale identity cannot retarget a reused tree slot");
}

void shortMutationFailure() {
  Fixture fixture;
  auto state = take(fixture.scene->state(0));
  ui::Ref<Label> output;
  auto node = take(ui::mount(*fixture.scene, ui::label("output").reference(output)));
  take(fixture.scene->observe(state, [&, output](const int &) mutable {
    take(fixture.scene->remove(node));
    output.text() = "retired";
  }));
  auto prepared = fixture.scene->prepare(*fixture.app, VIEWPORT);
  check(!prepared && prepared.error() == TreeError::STALE_NODE, "void observer cannot hide a fluent mutation failure");
  const auto errors = take(fixture.scene->diagnostics()).errors;
  check(!errors.empty() && errors.back().error.node == node.id() && errors.back().error.execution &&
          std::string_view(errors.back().error.execution) == "managed.observe",
        "existing collector preserves failed Ref identity and entered execution");
}

void allocationFailures() {
  std::size_t allocations{};
  {
    Fixture fixture;
    auto state = take(fixture.scene->state(1));
    const auto before = allocationTest::count;
    take(fixture.scene->observe(state, [](const int &) {
    }));
    allocations = allocationTest::count - before;
  }
  check(allocations > 0, "registration allocation calibration is nonempty");
  for (std::size_t failAt = 0; failAt < allocations; ++failAt) {
    Fixture fixture;
    auto state = take(fixture.scene->state(1));
    int calls{};
    allocationTest::remaining = static_cast<std::ptrdiff_t>(failAt);
    auto result = fixture.scene->observe(state, [&](const int &) {
      ++calls;
    });
    allocationTest::remaining = -1;
    check(!result && treeCause(result.error(), TreeError::NO_MEMORY),
          "each calibrated allocation fails with NO_MEMORY");
    take(state.set(2));
    fixture.dispatch();
    fixture.prepare();
    check(calls == 0, "failed admission leaves no subscription or attachment");
    take(fixture.scene->observe(state, [&](const int &) {
      ++calls;
    }));
    fixture.prepare();
    check(calls == 1, "retry after failed admission remains possible");
  }

  std::size_t snapshotAllocations{};
  {
    Fixture fixture;
    auto state = take(fixture.scene->state(0));
    std::size_t before{};
    take(fixture.scene->observe(state, [&](const int &) {
      snapshotAllocations = allocationTest::count - before;
    }));
    before = allocationTest::count;
    fixture.prepare();
  }
  check(snapshotAllocations > 0, "owning snapshot allocation calibration is nonempty");
  for (std::size_t failAt = 0; failAt < snapshotAllocations; ++failAt) {
    Fixture fixture;
    auto state = take(fixture.scene->state(0));
    int calls{};
    take(fixture.scene->observe(state, [&](const int &) {
      ++calls;
    }));
    allocationTest::remaining = static_cast<std::ptrdiff_t>(failAt);
    auto failed = fixture.scene->prepare(*fixture.app, VIEWPORT);
    allocationTest::remaining = -1;
    check(!failed && failed.error() == TreeError::NO_MEMORY && calls == 0,
          "owning registry snapshot OOM preserves initial watermark");
    fixture.prepare();
    check(calls == 1, "explicit prepare retries initial synchronization after allocation failure");
  }
  std::cout << "ordinary-new registration allocation points: " << allocations << '\n';
  std::cout << "ordinary-new observer snapshot allocation points: " << snapshotAllocations << '\n';
}

void idleRuntime() {
  Fixture fixture;
  auto state = take(fixture.scene->state(1));
  int calls{};
  take(fixture.scene->observe(state, [&](const int &) {
    ++calls;
  }));
  auto result = take(fixture.scene->run(fixture.terminal));
  check(calls == 1 && result.frames == 1, "managed observers produce no idle redraw");
}

void nestedRecoveryFailure() {
  Fixture fixture;
  auto early = take(fixture.scene->state(0));
  auto late = take(fixture.scene->state(0));
  int failureCalls{};
  take(fixture.scene->observe(early, [&](const int &value) {
    if (value == 1) {
      take(late.set(1));
      fixture.dispatch();
    }
  }));
  take(fixture.scene->observe(late, [&](const int &value) -> std::expected<void, TreeError> {
    if (value == 1 && ++failureCalls == 1) {
      return std::unexpected(TreeError::INVALID_GEOMETRY);
    }
    return {};
  }));
  fixture.prepare();
  take(early.set(1));
  auto result = fixture.scene->prepare(*fixture.app, VIEWPORT);
  std::cout << "nested recovery result=" << result.has_value() << " failureCalls=" << failureCalls << '\n';
  check(!result && result.error() == TreeError::INVALID_GEOMETRY && failureCalls == 1,
        "nested dispatch failure is preserved without automatic retry in the same prepare");
  fixture.prepare();
  check(failureCalls == 2, "only explicit later prepare retries the failed record");
}

void currentFacadeDuringObserver() {
  for (const int phase : {0, 1, 2}) {
    example::ReplayTerminal terminal({}, 5000, VIEWPORT);
    ui::SceneOptions options;
    options.clock = terminal.clock();
    bool currentAvailable{true};
    int calls{};
    ui::Ref<Label> output;
    ManagedState<int> state;
    Application *application{};
    auto owner = ui::scene(
      [&](ManagedScene &scene, Application &app) {
        application = &app;
        state = take(scene.state(0));
        return ui::label("output").reference(output).observe(state, [&, output](const int &value) mutable {
          ++calls;
          currentAvailable = currentAvailable && !ui::detail::CurrentSceneScope::snapshot().expired();
          output.text() = "updated";
          if (phase == 0 || value == 1) {
            ui::quit();
          }
        });
      },
      options);
    if (phase) {
      take(owner.prepareChecked());
      auto &managed = take(owner.managed()).get();
      take(managed.prepare(*application, VIEWPORT));
      take(state.set(1));
      if (phase == 1) {
        take(application->scheduler().dispatch());
      } else {
        take(managed.prepare(*application, VIEWPORT));
      }
    }
    take(owner.run(terminal));
    std::cout << "observer current facade=" << currentAvailable << " calls=" << calls << '\n';
    check(currentAvailable && calls == (phase ? 2 : 1),
          "all observer phases restore current facade for short commands and Ref operations");
  }
}

void shortFailureReportedOnce() {
  for (const bool throwAfter : {false, true}) {
    Fixture fixture;
    auto state = take(fixture.scene->state(0));
    ui::Ref<Label> output;
    auto node = take(ui::mount(*fixture.scene, ui::label("output").reference(output)));
    int notified{};
    take(fixture.scene->onError([&](const ManagedDiagnostic &) {
      ++notified;
    }));
    take(fixture.scene->observe(state, [&, output](const int &) mutable {
      take(fixture.scene->remove(node));
      output.text() = "retired";
      if (throwAfter) {
        throw std::runtime_error("after short failure");
      }
    }));
    auto result = fixture.scene->run(fixture.terminal);
    const auto errors = take(fixture.scene->diagnostics()).errors;
    std::cout << "short failure retained=" << errors.size() << " notified=" << notified << '\n';
    check(!result && treeCause(result.error(), TreeError::STALE_NODE) && errors.size() == 1 && notified == 1,
          "first short observer failure is retained and notified once across runtime finish");
  }
}
} // namespace

int main() {
  try {
    initialAndQueuedDelivery();
    declarativeMount();
    rejectedAndFixedPoint();
    boundedRecovery();
    removalAndErrors();
    admissionAndLifetime();
    registrationConstructionReentry();
    constructionAndCallbackFailures();
    hiddenDisabledAndSnapshotLifetime();
    shortMutationFailure();
    allocationFailures();
    idleRuntime();
    shortFailureReportedOnce();
    currentFacadeDuringObserver();
    nestedRecoveryFailure();
    std::cout << "managed observe contracts passed\n";
  } catch (const std::exception &failure) {
    allocationTest::remaining = -1;
    std::cerr << failure.what() << '\n';
    return 1;
  }
}
