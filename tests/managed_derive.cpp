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

struct Count {
  int value;
  Count() = delete;

  explicit Count(int input) : value(input) {
  }

  bool operator==(const Count &) const = default;
};

void initialAndReadonly() {
  Fixture fixture;
  auto input = take(fixture.scene->state(2));
  int calls{};
  auto derived = take(fixture.scene->derive(input, [&](const int &value) {
    ++calls;
    return Count(value * 3);
  }));
  check(take(derived.snapshot())->value.value == 6 && calls == 1, "initial output is immediate and nondefault");
  fixture.prepare();
  check(calls == 1, "unchanged first prepare does not recompute");
  take(input.set(3));
  fixture.prepare();
  check(take(derived.snapshot())->value.value == 9 && calls == 2, "changed input derives latest output");
}

template <class T>
concept Writable = requires(T value) { value.set(1); };
template <class F>
concept Derivable =
  requires(ManagedScene &scene, ManagedState<int> input, F function) { scene.derive(input, function); };
static_assert(!Writable<ManagedSource<int>>);
static_assert(!std::convertible_to<ManagedSource<int>, ManagedState<int>>);
static_assert(!Derivable<decltype([](const int &) {
  return std::string_view("borrowed");
})>);
static_assert(!Derivable<decltype([](const int &) {
  return std::span<const int>{};
})>);
static_assert(!Derivable<decltype([](const int &) -> const int & {
  static int value{};
  return value;
})>);
static_assert(!Derivable<decltype([](const int &) {
  return std::expected<int, std::string>(1);
})>);

ManagedOptions boundedOptions() {
  ManagedOptions options;
  options.maxStates = 2;
  options.maxObservers = options.maxObserversPerNode = 1;
  return options;
}

void initialErrorsDoNotPoisonRuntime() {
  for (int mode = 0; mode < 4; ++mode) {
    Fixture fixture(boundedOptions());
    auto input = take(fixture.scene->state(3));
    auto result = fixture.scene->derive(input, [mode](const int &) -> std::expected<int, TreeError> {
      if (mode == 0) {
        return std::unexpected(TreeError::INVALID_GEOMETRY);
      }
      if (mode == 1) {
        throw std::bad_alloc();
      }
      if (mode == 2) {
        throw std::runtime_error("initial computation");
      }
      throw std::length_error("client length error");
    });
    const auto cause = mode == 0   ? TreeError::INVALID_GEOMETRY
                       : mode == 1 ? TreeError::NO_MEMORY
                                   : TreeError::CALLBACK_FAILED;
    check(!result && treeCause(result.error(), cause) && result.error().phase == ManagedPhase::STATE,
          "initial failure retains exact error without an output");
    auto valid = take(fixture.scene->derive(input, [](const int &value) {
      return value * 2;
    }));
    check(take(valid.snapshot())->value == 6, "failed creation released both finite budgets");
    check(take(fixture.scene->diagnostics()).errors.empty(), "handled initial error is not a runtime diagnostic");
    take(fixture.scene->run(fixture.terminal));
    check(take(valid.snapshot())->value == 6, "handled initial failure allows a valid run and final snapshot");
  }
}

void initialShortErrorAndReentry() {
  Fixture fixture(boundedOptions());
  auto input = take(fixture.scene->state(0));
  ui::Ref<Label> output;
  auto node = take(ui::mount(*fixture.scene, ui::label(output, "old")));
  take(fixture.scene->remove(node));
  auto failed = fixture.scene->derive(input, [output](const int &) mutable {
    output.text() = "retired";
    return 1;
  });
  check(!failed && treeCause(failed.error(), TreeError::STALE_NODE),
        "initial ignored short error prevents publication");
  auto valid = take(fixture.scene->derive(input, [](const int &value) {
    return value + 1;
  }));
  take(fixture.scene->run(fixture.terminal));
  check(take(valid.snapshot())->value == 1 && take(fixture.scene->diagnostics()).errors.size() == 1,
        "initial short error is retained once without poisoning runtime");

  Fixture changed(boundedOptions());
  auto source = take(changed.scene->state(1));
  auto inconsistent = changed.scene->derive(source, [source](const int &value) {
    take(source.set(value + 1));
    return value;
  });
  check(!inconsistent && treeCause(inconsistent.error(), TreeError::WOULD_BLOCK) && take(source.snapshot())->value == 2,
        "initial declared input mutation rejects stale output without claiming client rollback");
  take(changed.scene->derive(source, [&](const int &value) {
    auto prepare = changed.scene->prepare(*changed.app, VIEWPORT);
    auto close = changed.scene->close();
    auto run = changed.scene->run(changed.terminal);
    check(!prepare && prepare.error() == TreeError::WOULD_BLOCK && !close &&
            treeCause(close.error(), TreeError::WOULD_BLOCK) && !run && treeCause(run.error(), TreeError::WOULD_BLOCK),
          "initial callback guard rejects lifecycle reentry");
    return value;
  }));
  take(changed.scene->run(changed.terminal));
}

void chainsCaptionsAndRejections() {
  Fixture fixture({}, 1);
  auto input = take(fixture.scene->state(1));
  int firstCalls{}, secondCalls{}, lastCalls{}, observed{};
  auto first = take(fixture.scene->derive(input, [&](const int &value) {
    ++firstCalls;
    return value * 2;
  }));
  auto second = take(fixture.scene->derive(first, [&](const int &value) {
    ++secondCalls;
    return value + 1;
  }));
  auto last = take(fixture.scene->derive(second, [&](const int &value) {
    ++lastCalls;
    return std::to_string(value);
  }));
  ui::Ref<Label> label;
  ui::Ref<Button> button;
  take(ui::mount(*fixture.scene, ui::vbox(ui::label(last).reference(label).observe(last,
                                                                                   [&](const std::string &) {
                                                                                     ++observed;
                                                                                   }),
                                          ui::button(last, [] {
                                          }).reference(button))));
  fixture.prepare();
  check(firstCalls == 1 && secondCalls == 1 && lastCalls == 1 && observed == 1,
        "seeded derivation input revisions avoid duplicate initial computation");
  take(input.set(4));
  fixture.prepare();
  check(take(label.captionText().snapshot()).bytes() == "9" && take(button.captionText().snapshot()).bytes() == "9",
        "chain settles both existing captions before the same dirty frame");
  check(take(last.snapshot())->value == "9" && observed == 2 && firstCalls == 2 && secondCalls == 2 && lastCalls == 2,
        "rejected committed output notifications recover without retrying transform");
  fixture.dispatch();
  fixture.prepare();
  check(firstCalls == 2 && observed == 2, "queued superseded notifications cannot duplicate recovered values");
  take(fixture.app->tree().consumeFrameRequest());
  fixture.dispatch();
  fixture.prepare();
  check(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE, "settled chain installs no idle redraw");
}

void equalOutputAndLastGood() {
  Fixture fixture;
  auto input = take(fixture.scene->state(0));
  int computes{}, presentations{};
  auto parity = take(fixture.scene->derive(input, [&](const int &value) {
    ++computes;
    return value % 2;
  }));
  take(fixture.scene->observe(parity, [&](const int &) {
    ++presentations;
  }));
  fixture.prepare();
  take(input.set(2));
  fixture.prepare();
  fixture.prepare();
  check(computes == 2 && presentations == 1 && take(parity.snapshot())->revision == 0,
        "equal output advances only the input watermark");
  take(input.set(3));
  fixture.prepare();
  check(computes == 3 && presentations == 2 && take(parity.snapshot())->revision == 1, "changed output publishes once");

  Fixture failure;
  auto source = take(failure.scene->state(2));
  auto output = take(failure.scene->derive(source, [](const int &value) -> std::expected<int, TreeError> {
    return value == 20 ? std::unexpected(TreeError::INVALID_GEOMETRY) : std::expected<int, TreeError>(value * 2);
  }));
  take(source.set(20));
  auto result = failure.scene->prepare(*failure.app, VIEWPORT);
  check(!result && result.error() == TreeError::INVALID_GEOMETRY && take(source.snapshot())->value == 20 &&
          take(output.snapshot())->value == 4 && take(output.snapshot())->revision == 0,
        "runtime error preserves committed source and exact last good output");
  const auto errors = take(failure.scene->diagnostics()).errors;
  check(errors.size() == 1 && errors[0].error.phase == ManagedPhase::PREPARE && errors[0].error.registrationOrigin &&
          errors[0].error.operation && std::string_view(errors[0].error.operation) == "derive.compute",
        "runtime error has one typed derivation diagnostic with registration origin");
  take(source.set(3));
  failure.prepare();
  check(take(output.snapshot())->value == 6 && take(output.snapshot())->revision == 1,
        "explicit later prepare retries");
}

void sourceLifetimeAndRemoval() {
  Fixture fixture;
  auto input = take(fixture.scene->state(std::string("temporary source")));
  auto output = take(fixture.scene->derive(input, [](const std::string &value) {
    return std::vector<std::string>{value.substr(0, 9), value.substr(10)};
  }));
  auto pinned = take(output.snapshot());
  auto receiver = fixture.node();
  int calls{};
  take(fixture.scene->observe(receiver, output, [&](const std::vector<std::string> &) {
    ++calls;
    take(fixture.scene->remove(receiver));
  }));
  fixture.prepare();
  take(input.set(std::string("different text")));
  fixture.prepare();
  check(calls == 1 && pinned->value[0] == "temporary", "owning output snapshot and self removal remain independent");
  std::thread worker([&] {
    auto wrong = output.snapshot();
    check(!wrong && wrong.error() == TreeError::WRONG_THREAD, "source enforces UI affinity");
  });
  worker.join();
  take(fixture.scene->close());
  check(take(output.snapshot())->value[0] == "different", "output remains readable after close");
  fixture.scene.reset();
  check(!output.snapshot() && output.snapshot().error() == TreeError::CLOSED && pinned->value[1] == "source",
        "weak output expires while independent owning snapshots survive");
}

void atomicAllocationFailures() {
  std::size_t allocations{};
  {
    Fixture fixture(boundedOptions());
    auto source = take(fixture.scene->state(2, Invalidation::PAINT, PropertyOptions{1}));
    const auto before = allocationTest::count;
    take(fixture.scene->derive(source, [](const int &value) {
      return value * 2;
    }));
    allocations = allocationTest::count - before;
  }
  for (std::size_t failAt = 0; failAt < allocations; ++failAt) {
    Fixture fixture(boundedOptions());
    auto source = take(fixture.scene->state(2, Invalidation::PAINT, PropertyOptions{1}));
    allocationTest::remaining = static_cast<std::ptrdiff_t>(failAt);
    auto failed = fixture.scene->derive(source, [](const int &value) {
      return value * 2;
    });
    allocationTest::remaining = -1;
    check(!failed && treeCause(failed.error(), TreeError::NO_MEMORY), "calibrated derive allocation fails atomically");
    auto valid = take(fixture.scene->derive(source, [](const int &value) {
      return value * 2;
    }));
    fixture.prepare();
    take(source.set(3));
    fixture.prepare();
    check(take(valid.snapshot())->value == 6 && take(fixture.scene->diagnostics()).errors.empty(),
          "failed admission released state, observer, and input endpoint budgets");
  }
  std::cout << "ordinary-new derive allocation points: " << allocations << '\n';
}

void stringAllocationFailures() {
  std::size_t allocations{};
  const auto transform = [](const int &value) {
    return std::to_string(value);
  };
  {
    Fixture fixture(boundedOptions());
    auto input = take(fixture.scene->state(1, Invalidation::PAINT, PropertyOptions{1}));
    const auto before = allocationTest::count;
    take(fixture.scene->derive(input, transform));
    allocations = allocationTest::count - before;
  }
  for (std::size_t failAt = 0; failAt < allocations; ++failAt) {
    Fixture fixture(boundedOptions());
    auto input = take(fixture.scene->state(1, Invalidation::PAINT, PropertyOptions{1}));
    allocationTest::remaining = static_cast<std::ptrdiff_t>(failAt);
    auto failed = fixture.scene->derive(input, transform);
    allocationTest::remaining = -1;
    check(!failed && treeCause(failed.error(), TreeError::NO_MEMORY), "string output allocation remains checked");
    auto valid = take(fixture.scene->derive(input, transform));
    take(input.set(2));
    fixture.prepare();
    check(take(valid.snapshot())->value == "2", "string OOM rollback preserves all budgets and later computation");
  }
  std::cout << "ordinary-new string derive allocation points: " << allocations << '\n';
}

struct OutputValue {
  int value;
  static inline int copies{}, moves{};
  static inline bool failMove{}, failEquality{};

  explicit OutputValue(int input) : value(input) {
  }

  OutputValue(const OutputValue &other) : value(other.value) {
    ++copies;
  }

  OutputValue(OutputValue &&other) : value(other.value) {
    if (failMove) {
      throw std::bad_alloc();
    }
    ++moves;
  }

  bool operator==(const OutputValue &other) const {
    if (failEquality) {
      throw std::runtime_error("output comparison");
    }
    return value == other.value;
  }
};

void outputOwnershipAndCommitFailures() {
  for (bool failMove : {false, true}) {
    Fixture fixture;
    auto input = take(fixture.scene->state(1));
    OutputValue::copies = OutputValue::moves = 0;
    auto output = take(fixture.scene->derive(input, [](const int &value) {
      return OutputValue(value);
    }));
    check(OutputValue::copies == 0 && OutputValue::moves == 1, "initial output moves directly to its owning snapshot");
    take(input.set(2));
    OutputValue::failMove = failMove;
    OutputValue::failEquality = !failMove;
    auto result = fixture.scene->prepare(*fixture.app, VIEWPORT);
    OutputValue::failMove = OutputValue::failEquality = false;
    const auto cause = failMove ? TreeError::NO_MEMORY : TreeError::CALLBACK_FAILED;
    check(!result && result.error() == cause && take(output.snapshot())->value.value == 1 &&
            take(output.snapshot())->revision == 0 && take(input.snapshot())->value == 2,
          "output construction/comparison failure leaves last good output and committed input");
    fixture.prepare();
    check(take(output.snapshot())->value.value == 2 && OutputValue::copies == 0 && OutputValue::moves == 2,
          "explicit retry transfers exactly one new output without copying payloads");
  }
}

void initialCaptureReentryAndLifetime() {
  Fixture fixture;
  auto input = take(fixture.scene->state(1));

  struct Reenter {
    Fixture &fixture;

    explicit Reenter(Fixture &owner) : fixture(owner) {
    }

    Reenter(const Reenter &other) : fixture(other.fixture) {
      take(fixture.scene->close());
    }

    int operator()(const int &value) {
      return value;
    }
  } transform(fixture);

  auto failed = fixture.scene->derive(input, transform);
  check(!failed && treeCause(failed.error(), TreeError::CLOSED), "capture construction reentry rechecks closed scene");

  Fixture lifetime;
  auto source = take(lifetime.scene->state(1));
  int destroyed{};
  auto capture = std::shared_ptr<int>(new int(1), [&](int *value) {
    delete value;
    ++destroyed;
  });
  auto output = take(lifetime.scene->derive(source, [capture](const int &value) {
    return value + *capture;
  }));
  capture.reset();
  check(destroyed == 0, "scene retains transform capture after derive returns");
  take(lifetime.scene->close());
  check(destroyed == 1 && take(output.snapshot())->value == 2,
        "close releases transform while retaining last output until destruction");
}

void outputAllocationAndInitialRetirement() {
  Fixture fixture;
  auto input = take(fixture.scene->state(0));
  bool arm{true};
  auto output = take(fixture.scene->derive(input, [&](const int &value) {
    if (arm && value == 1) {
      allocationTest::remaining = 0;
    }
    return value;
  }));
  take(input.set(1));
  auto failed = fixture.scene->prepare(*fixture.app, VIEWPORT);
  allocationTest::remaining = -1;
  check(!failed && failed.error() == TreeError::NO_MEMORY && take(output.snapshot())->value == 0 &&
          take(output.snapshot())->revision == 0,
        "runtime output snapshot allocation failure preserves last good output");
  arm = false;
  fixture.prepare();
  check(take(output.snapshot())->value == 1, "explicit prepare retries failed output allocation");

  Fixture retired;
  auto source = take(retired.scene->state(1));
  auto removed = retired.scene->derive(source, [&](const int &value) {
    take(retired.app->tree().remove(retired.scene->root()));
    return value;
  });
  check(!removed && treeCause(removed.error(), TreeError::STALE_NODE),
        "low-level root retirement during initial computation prevents attachment publication");
}
} // namespace

int main() {
  try {
    initialAndReadonly();
    initialErrorsDoNotPoisonRuntime();
    initialShortErrorAndReentry();
    chainsCaptionsAndRejections();
    equalOutputAndLastGood();
    sourceLifetimeAndRemoval();
    atomicAllocationFailures();
    stringAllocationFailures();
    outputOwnershipAndCommitFailures();
    initialCaptureReentryAndLifetime();
    outputAllocationAndInitialRetirement();
    std::cout << "managed derive contracts passed\n";
  } catch (const std::exception &error) {
    allocationTest::remaining = -1;
    std::cerr << error.what() << '\n';
    return 1;
  }
}
