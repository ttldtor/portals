// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "../examples/replay/terminal.hpp"
#include <portals/managed.hpp>

#include <array>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>

static_assert(std::is_aggregate_v<portals::ManagedError>);
static_assert(std::is_nothrow_copy_constructible_v<portals::ManagedError>);

TEST_CASE("managed errors preserve aggregate positional and designated initialization") {
  const portals::ManagedError positional{portals::ManagedPhase::TEXT, {}, portals::TreeError::CLOSED, {}, {}};
  const portals::ManagedError designated{.phase = portals::ManagedPhase::TEXT, .cause = portals::TreeError::CLOSED};
  CHECK(positional.phase == designated.phase);
  CHECK(std::get<portals::TreeError>(positional.cause) == std::get<portals::TreeError>(designated.cause));
  CHECK_FALSE(positional.operation);
  CHECK_FALSE(designated.operationOrigin);
  CHECK_FALSE(designated.setupStopped);
}

namespace {
std::atomic<bool> rejectAllocations{};
std::atomic<std::ptrdiff_t> allocationsBeforeFailure{-1};
} // namespace

// Deterministic process-local allocation failure, enabled only around the tested API call.
void *operator new(std::size_t size) {
  if (rejectAllocations.load() || allocationsBeforeFailure.load() == 0) {
    throw std::bad_alloc();
  }

  if (allocationsBeforeFailure.load() > 0) {
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
  try {
    return ::operator new[](size);
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void *memory, const std::nothrow_t &) noexcept {
  ::operator delete(memory);
}

void operator delete[](void *memory, const std::nothrow_t &) noexcept {
  ::operator delete[](memory);
}

namespace {
using namespace portals;
constexpr CellSize VIEWPORT{20, 3};
static_assert(std::variant_size_v<decltype(ManagedError{}.cause)> == 3);

template <class T, class E> T take(std::expected<T, E> result) {
  if (!result) {
    throw std::runtime_error("fixture setup failed");
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

ManagedError failure(TreeError cause = TreeError::NODE_LIMIT) {
  return {ManagedPhase::STATE, {}, cause, {}, {}};
}

struct Fixture {
  example::ReplayTerminal terminal{{}, 200, VIEWPORT};
  std::unique_ptr<Application> app;
  std::unique_ptr<ManagedScene> scene;

  explicit Fixture(ManagedDiagnosticOptions diagnostics = {}) {
    ManagedOptions options;

    options.diagnostics = diagnostics;
    app = take(Application::create({}, terminal.clock()));
    scene = take(ManagedScene::create(*app, options));
  }
};

struct Lifetime {
  int &destroyed;

  ~Lifetime() {
    ++destroyed;
  }
};
} // namespace

TEST_CASE("collector keeps every original typed cause and the report origin") {
  Fixture fixture;
  const auto origin = std::source_location::current();
  const TextError textCause{TextErrorKind::INVALID_UTF8, 17};
  const StyleError styleCause{TreeError::INVALID_ARGUMENT, 4, 9, "static style cause"};
  const ApplicationError appCause{ApplicationPhase::FLUSH, TerminalError{TerminalErrorKind::SYSTEM, 71}, {}};

  take(fixture.scene->reportError(failure(), {}, origin));
  take(fixture.scene->reportError({ManagedPhase::STYLE, {}, styleCause, {}, {}}));
  take(fixture.scene->reportError({ManagedPhase::RUN, {}, appCause, appCause, TreeError::CLOSED}));
  take(fixture.scene->reportError({ManagedPhase::STATE, {}, TreeError::INVALID_ARGUMENT, {}, {}, textCause}));
  take(fixture.scene->reportError(
    {ManagedPhase::STATE, {}, TreeError::INVALID_ARGUMENT, {}, {}, {}, TextResourceError::MISSING_RESOURCE}));

  const auto batch = take(fixture.scene->diagnostics());

  REQUIRE(batch.errors.size() == 5);
  CHECK(std::get<TreeError>(batch.errors[0].error.cause) == TreeError::NODE_LIMIT);
  CHECK(batch.errors[0].origin.line() == origin.line());
  CHECK(std::string_view(batch.errors[0].origin.file_name()) == origin.file_name());
  CHECK(std::get<StyleError>(batch.errors[1].error.cause).column == 9);
  CHECK(std::get<StyleError>(batch.errors[1].error.cause).message == "static style cause");
  CHECK(std::get<ApplicationError>(batch.errors[2].error.cause).phase == ApplicationPhase::FLUSH);
  CHECK(batch.errors[2].error.application.has_value());
  CHECK(batch.errors[2].error.cleanup == TreeError::CLOSED);
  CHECK(batch.errors[3].error.text == textCause);
  CHECK(batch.errors[4].error.resource == TextResourceError::MISSING_RESOURCE);
}

TEST_CASE("causal labels own temporary input and preserve deferred source locations") {
  Fixture fixture;
  const auto lookupOrigin = std::source_location::current();
  const auto styleOrigin = std::source_location::current();

  {
    std::array contexts{ManagedDiagnosticContext{"lookup", "message", "form", "LineEdit", lookupOrigin},
                        ManagedDiagnosticContext{"style", "message", "form", "Control", styleOrigin}};

    take(fixture.scene->reportError(failure(), contexts));
    contexts[0].name = "changed";
  }

  const auto batch = take(fixture.scene->drainErrors());

  take(fixture.scene->close());
  fixture.scene.reset();
  REQUIRE(batch.errors.size() == 1);
  REQUIRE(batch.errors[0].contexts.size() == 2);
  CHECK(batch.errors[0].contexts[0].name == "message");
  CHECK(batch.errors[0].contexts[0].expectedType == "LineEdit");
  CHECK(batch.errors[0].contexts[0].origin.line() == lookupOrigin.line());
  CHECK(batch.errors[0].contexts[1].origin.line() == styleOrigin.line());
}

TEST_CASE("context count and combined label byte budgets truncate predictably") {
  Fixture fixture({2, 1, 5});
  const std::array contexts{ManagedDiagnosticContext{"append", "name", "scope", "type", {}},
                            ManagedDiagnosticContext{"later", {}, {}, {}, {}}};

  take(fixture.scene->reportError(failure(), contexts));

  const auto batch = take(fixture.scene->diagnostics());
  const auto &diagnostic = batch.errors.front();

  REQUIRE(diagnostic.contexts.size() == 1);
  CHECK(diagnostic.contexts[0].operation == "appen");
  CHECK(diagnostic.contexts[0].name.empty());
  CHECK(diagnostic.contextsTruncated);
  CHECK_FALSE(diagnostic.contextUnavailable);
}

TEST_CASE("overflow and reporting itself preserve causes without allocation") {
  Fixture fixture({1, 2, 128});
  int notifications{};
  TreeError observed{};
  bool overflow{};
  bool preservedOverflowBorrow{};

  take(fixture.scene->onError([&](const ManagedDiagnostic &diagnostic) {
    ++notifications;
    observed = std::get<TreeError>(diagnostic.error.cause);
    overflow = diagnostic.storageOverflow;

    if (overflow) {
      take(fixture.scene->reportError(failure(TreeError::CLOSED)));
      preservedOverflowBorrow = std::get<TreeError>(diagnostic.error.cause) == TreeError::DEPTH_LIMIT;
    }
  }));
  rejectAllocations = true;

  const auto first = fixture.scene->reportError(failure(TreeError::NODE_LIMIT));
  const auto second = fixture.scene->reportError(failure(TreeError::DEPTH_LIMIT));

  rejectAllocations = false;
  REQUIRE(first);
  REQUIRE(second);

  const auto batch = take(fixture.scene->diagnostics());

  REQUIRE(batch.errors.size() == 1);
  CHECK(std::get<TreeError>(batch.errors[0].error.cause) == TreeError::NODE_LIMIT);
  CHECK(batch.droppedErrors == 2);
  CHECK(batch.suppressedNotifications == 1);
  CHECK(notifications == 2);
  CHECK(observed == TreeError::DEPTH_LIMIT);
  CHECK(overflow);
  CHECK(preservedOverflowBorrow);
}

TEST_CASE("every allocation failure during deep snapshot or drain preserves the original history") {
  Fixture fixture;
  const std::array contexts{ManagedDiagnosticContext{std::string(80, 'x'), std::string(80, 'n'), {}, {}, {}}};
  constexpr std::ptrdiff_t MAX_ALLOCATION_BUDGET = 128;
  bool drain{};

  SUBCASE("snapshot") {
    drain = false;
  }

  SUBCASE("drain") {
    drain = true;
  }

  take(fixture.scene->reportError(failure(), contexts));

  bool completed{};
  std::size_t failures{};

  for (std::ptrdiff_t budget = 0; budget < MAX_ALLOCATION_BUDGET && !completed; ++budget) {
    allocationsBeforeFailure = budget;

    // Avoid a conditional-expression temporary: MSVC 2022 may move the expected here.
    // Its Debug vector move allocates a proxy outside the tested error boundary.
    const auto result = [&] {
      if (drain) {
        return fixture.scene->drainErrors();
      }

      return fixture.scene->diagnostics();
    }();

    allocationsBeforeFailure = -1;

    if (result) {
      completed = true;
      REQUIRE(result->errors.size() == 1);
      CHECK(result->errors[0].contexts[0].operation == contexts[0].operation);
      CHECK(result->errors[0].contexts[0].name == contexts[0].name);
    } else {
      ++failures;
      CHECK(result.error() == TreeError::NO_MEMORY);

      const auto retained = take(fixture.scene->diagnostics());

      REQUIRE(retained.errors.size() == 1);
      REQUIRE(retained.errors[0].contexts.size() == 1);
      CHECK(retained.errors[0].contexts[0].operation == contexts[0].operation);
      CHECK(retained.errors[0].contexts[0].name == contexts[0].name);
    }
  }

  CHECK(completed);
  CHECK(failures > 0);
  CHECK(take(fixture.scene->diagnostics()).errors.size() == (drain ? 0 : 1));
}

TEST_CASE("context OOM retains error and origin then notifies without allocating") {
  Fixture fixture;
  const std::array contexts{ManagedDiagnosticContext{std::string(80, 'x'), {}, {}, {}, {}}};
  const auto origin = std::source_location::current();
  bool unavailable{};

  take(fixture.scene->onError([&](const ManagedDiagnostic &diagnostic) {
    unavailable = diagnostic.contextUnavailable;
  }));
  rejectAllocations = true;

  const auto result = fixture.scene->reportError(failure(TreeError::NO_MEMORY), contexts, origin);

  rejectAllocations = false;
  REQUIRE(result);
  CHECK(unavailable);

  const auto batch = take(fixture.scene->diagnostics());

  REQUIRE(batch.errors.size() == 1);
  CHECK(std::get<TreeError>(batch.errors[0].error.cause) == TreeError::NO_MEMORY);
  CHECK(batch.errors[0].origin.line() == origin.line());
  CHECK(batch.errors[0].contexts.empty());
  CHECK(batch.errors[0].contextUnavailable);
  CHECK(batch.contextFailures == 1);
}

TEST_CASE("failed snapshot drain and registration leave history and observer unchanged") {
  Fixture fixture;
  int calls{};

  take(fixture.scene->onError([&](const ManagedDiagnostic &) {
    ++calls;
  }));
  take(fixture.scene->reportError(failure()));
  rejectAllocations = true;

  const auto snapshot = fixture.scene->diagnostics();
  const auto drained = fixture.scene->drainErrors();
  const auto registration = fixture.scene->onError([](const ManagedDiagnostic &) {
  });

  rejectAllocations = false;
  REQUIRE_FALSE(snapshot);
  REQUIRE_FALSE(drained);
  REQUIRE_FALSE(registration);
  CHECK(snapshot.error() == TreeError::NO_MEMORY);
  CHECK(drained.error() == TreeError::NO_MEMORY);
  CHECK(std::get<TreeError>(registration.error().cause) == TreeError::NO_MEMORY);
  take(fixture.scene->reportError(failure()));
  CHECK(calls == 2);
  CHECK(take(fixture.scene->drainErrors()).errors.size() == 2);
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
  take(fixture.scene->reportError(failure()));
  CHECK(calls == 3);
}

TEST_CASE("throwing and reentrant observer cannot erase original errors or recurse") {
  Fixture fixture;
  int calls{};
  bool rejectedClear{}, rejectedDrain{}, rejectedClose{}, rejectedRegistration{}, inspectionWorked{};
  bool rejectedPrepare{}, rejectedInput{};

  take(fixture.scene->onError([&](const ManagedDiagnostic &) {
    ++calls;

    const auto clear = fixture.scene->clearErrors();
    const auto drain = fixture.scene->drainErrors();
    const auto close = fixture.scene->close();
    const auto registration = fixture.scene->onError({});
    const auto prepare = fixture.scene->prepare(*fixture.app, VIEWPORT);
    const auto input = fixture.scene->input(*fixture.app, TextInputEvent{U'x'});

    rejectedClear = !clear && clear.error() == TreeError::WOULD_BLOCK;
    rejectedDrain = !drain && drain.error() == TreeError::WOULD_BLOCK;
    rejectedClose = !close && std::get<TreeError>(close.error().cause) == TreeError::WOULD_BLOCK;
    rejectedRegistration = !registration && std::get<TreeError>(registration.error().cause) == TreeError::WOULD_BLOCK;
    rejectedPrepare = !prepare && prepare.error() == TreeError::WOULD_BLOCK;
    rejectedInput = !input && input.error() == TreeError::WOULD_BLOCK;
    inspectionWorked = take(fixture.scene->diagnostics()).errors.size() == 1;
    take(fixture.scene->reportError(failure(TreeError::DEPTH_LIMIT)));

    throw std::runtime_error("observer failed");
  }));
  take(fixture.scene->reportError(failure()));

  const auto batch = take(fixture.scene->diagnostics());

  CHECK(calls == 1);
  CHECK(rejectedClear);
  CHECK(rejectedDrain);
  CHECK(rejectedClose);
  CHECK(rejectedRegistration);
  CHECK(rejectedPrepare);
  CHECK(rejectedInput);
  CHECK(inspectionWorked);
  REQUIRE(batch.errors.size() == 2);
  CHECK(std::get<TreeError>(batch.errors[0].error.cause) == TreeError::NODE_LIMIT);
  CHECK(batch.errors[0].observerFailed);
  CHECK(batch.errors[1].notificationSuppressed);
  CHECK(batch.observerFailures == 1);
  CHECK(batch.suppressedNotifications == 1);
  take(fixture.scene->clearErrors());
  CHECK(take(fixture.scene->diagnostics()).observerFailures == 0);
  take(fixture.scene->onError({}));
  CHECK(take(fixture.scene->run(fixture.terminal)).frames == 1);
}

TEST_CASE("observer captures survive rejected replacement and end on replacement or close") {
  Fixture fixture;
  int destroyed{};
  auto lifetime = std::make_shared<Lifetime>(destroyed);
  std::weak_ptr<Lifetime> observer = lifetime;
  bool retainedInside{};

  take(fixture.scene->onError([&, lifetime](const ManagedDiagnostic &) {
    const auto replacement = fixture.scene->onError({});

    retainedInside = !replacement && !observer.expired();
  }));
  lifetime.reset();
  take(fixture.scene->reportError(failure()));
  CHECK(retainedInside);
  CHECK_FALSE(observer.expired());
  take(fixture.scene->onError({}));
  CHECK(observer.expired());
  CHECK(destroyed == 1);

  lifetime = std::make_shared<Lifetime>(destroyed);
  observer = lifetime;
  take(fixture.scene->onError([lifetime](const ManagedDiagnostic &) {
  }));
  lifetime.reset();
  take(fixture.scene->close());
  CHECK(observer.expired());
  CHECK(destroyed == 2);
  CHECK(take(fixture.scene->diagnostics()).closed);
  CHECK(take(fixture.scene->drainErrors()).errors.size() == 1);
  take(fixture.scene->clearErrors());

  const auto report = fixture.scene->reportError(failure());
  const auto registration = fixture.scene->onError({});

  REQUIRE_FALSE(report);
  REQUIRE_FALSE(registration);
  CHECK(report.error() == TreeError::CLOSED);
  CHECK(std::get<TreeError>(registration.error().cause) == TreeError::CLOSED);
}

TEST_CASE("foreign collector operations reject before changing state or invoking UI observer") {
  Fixture fixture;
  int calls{};
  bool rejectedReport{}, rejectedSnapshot{}, rejectedClear{}, rejectedDrain{}, rejectedObserver{};

  take(fixture.scene->onError([&](const ManagedDiagnostic &) {
    ++calls;
  }));

  std::thread worker([&] {
    const auto report = fixture.scene->reportError(failure());
    const auto snapshot = fixture.scene->diagnostics();
    const auto clear = fixture.scene->clearErrors();
    const auto drain = fixture.scene->drainErrors();
    const auto registration = fixture.scene->onError({});

    rejectedReport = !report && report.error() == TreeError::WRONG_THREAD;
    rejectedSnapshot = !snapshot && snapshot.error() == TreeError::WRONG_THREAD;
    rejectedClear = !clear && clear.error() == TreeError::WRONG_THREAD;
    rejectedDrain = !drain && drain.error() == TreeError::WRONG_THREAD;
    rejectedObserver = !registration && std::get<TreeError>(registration.error().cause) == TreeError::WRONG_THREAD;
  });

  worker.join();
  CHECK(rejectedReport);
  CHECK(rejectedSnapshot);
  CHECK(rejectedClear);
  CHECK(rejectedDrain);
  CHECK(rejectedObserver);
  CHECK(calls == 0);
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}

TEST_CASE("successful operations checked setup errors and idle redraw do not fill collector") {
  Fixture fixture;
  const auto invalid = fixture.scene->onClick({}, {});

  REQUIRE_FALSE(invalid);
  take(fixture.scene->state(1));
  take(fixture.scene->prepare(*fixture.app, VIEWPORT));
  take(fixture.app->tree().consumeFrameRequest());
  take(fixture.scene->reportError(failure()));
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  take(fixture.scene->clearErrors());

  const auto result = take(fixture.scene->run(fixture.terminal));

  CHECK(result.frames == 1);
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}

TEST_CASE("runtime first error survives observer throw and gains restoration detail after close") {
  class RestoreFailure final : public example::ReplayTerminal {
  public:
    RestoreFailure() : ReplayTerminal({{20, "\r", {}}}, 200, VIEWPORT) {
    }

    std::expected<void, TerminalError> close() noexcept override {
      (void)ReplayTerminal::close();

      return std::unexpected(TerminalError{TerminalErrorKind::SYSTEM, 71});
    }
  } terminal;

  auto app = take(Application::create({}, terminal.clock()));
  auto scene = take(ManagedScene::create(*app));
  const auto root = scene->root();
  const auto button = take(scene->emplace<Button>(root, take(GraphemeText::from("Fail"))));
  int calls{};

  take(scene->onError([&](const ManagedDiagnostic &diagnostic) {
    ++calls;
    CHECK(diagnostic.error.node == button.id());
    CHECK_FALSE(terminal.closed);

    throw std::bad_alloc();
  }));
  take(scene->onClick(button, []() -> std::expected<void, TreeError> {
    return std::unexpected(TreeError::DEPTH_LIMIT);
  }));
  take(app->router().setFocus(button));

  const auto result = scene->run(terminal);
  const auto batch = take(scene->diagnostics());

  REQUIRE_FALSE(result);
  CHECK(std::get<TreeError>(result.error().cause) == TreeError::DEPTH_LIMIT);
  REQUIRE(result.error().application);
  REQUIRE(result.error().application->restoration);
  CHECK(result.error().application->restoration->native_code == 71);
  CHECK(terminal.closed);
  CHECK_FALSE(app->tree().inspect(root));
  CHECK(calls == 1);
  REQUIRE(batch.errors.size() == 1);
  CHECK(std::get<TreeError>(batch.errors[0].error.cause) == TreeError::DEPTH_LIMIT);
  REQUIRE(batch.errors[0].error.application);
  REQUIRE(batch.errors[0].error.application->restoration);
  CHECK(batch.errors[0].error.application->restoration->native_code == 71);
  CHECK(batch.observerFailures == 1);
  CHECK(batch.closed);
}

TEST_CASE("invalid diagnostic budgets and creation OOM do not mount a partial subtree") {
  Fixture fixture;

  for (const auto diagnostics :
       {ManagedDiagnosticOptions{0, 1, 1}, ManagedDiagnosticOptions{1, 0, 1}, ManagedDiagnosticOptions{1, 1, 0},
        ManagedDiagnosticOptions{std::numeric_limits<std::size_t>::max(), 1, 1}}) {
    ManagedOptions options;

    options.diagnostics = diagnostics;

    const auto result = ManagedScene::create(*fixture.app, options);

    REQUIRE_FALSE(result);
    CHECK(std::get<TreeError>(result.error().cause) == TreeError::INVALID_ARGUMENT);
  }

  rejectAllocations = true;

  const auto allocation = ManagedScene::create(*fixture.app);

  rejectAllocations = false;
  REQUIRE_FALSE(allocation);
  CHECK(std::get<TreeError>(allocation.error().cause) == TreeError::NO_MEMORY);
  CHECK(take(fixture.app->tree().inspect(fixture.app->root())).children.size() == 1);
}

TEST_CASE("clearing or draining during runtime never rewrites an unrelated diagnostic at finish") {
  struct ResetHistory {
    bool drain{};
    bool reset{};
  } reset;

  struct ResetAtUnmount final : Widget {
    ManagedScene &scene;
    ResetHistory &state;

    ResetAtUnmount(ManagedScene &owner, ResetHistory &reset) : scene(owner), state(reset) {
    }

    void unmount() override {
      if (state.drain) {
        (void)take(scene.drainErrors());
      } else {
        take(scene.clearErrors());
      }

      state.reset = true;
      take(scene.reportError(failure(TreeError::NODE_LIMIT)));
    }
  };

  SUBCASE("clear") {
    reset.drain = false;
  }

  SUBCASE("drain") {
    reset.drain = true;
  }

  example::ReplayTerminal terminal({{20, "\r", {}}}, 200, VIEWPORT);
  auto app = take(Application::create({}, terminal.clock()));
  auto scene = take(ManagedScene::create(*app));
  const auto column = take(scene->emplace<Column>(scene->root()));
  const auto button = take(scene->emplace<Button>(column, take(GraphemeText::from("Fail"))));
  int clicks{};

  take(scene->emplace<ResetAtUnmount>(column, *scene, reset));
  take(scene->onClick(button, [&]() -> std::expected<void, TreeError> {
    ++clicks;

    return std::unexpected(TreeError::DEPTH_LIMIT);
  }));
  take(app->router().setFocus(button));

  const auto result = scene->run(terminal);
  const auto batch = take(scene->diagnostics());

  REQUIRE_FALSE(result);
  CHECK(clicks == 1);
  CHECK(reset.reset);
  REQUIRE(std::holds_alternative<TreeError>(result.error().cause));
  CHECK(std::get<TreeError>(result.error().cause) == TreeError::DEPTH_LIMIT);
  REQUIRE(batch.errors.size() == 2);
  CHECK(std::get<TreeError>(batch.errors[0].error.cause) == TreeError::NODE_LIMIT);
  REQUIRE(std::holds_alternative<TreeError>(batch.errors[1].error.cause));
  CHECK(std::get<TreeError>(batch.errors[1].error.cause) == TreeError::DEPTH_LIMIT);
  CHECK(batch.errors[1].error.node == button.id());
  CHECK_FALSE(batch.errors[0].error.application);
  CHECK_FALSE(batch.errors[1].error.application);
}
