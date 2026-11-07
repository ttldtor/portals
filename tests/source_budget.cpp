// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <atomic>
#include <cstdlib>
#include <doctest.h>
#include <new>
#include <portals/scheduler.hpp>
#include <portals/source_budget.hpp>
#include <thread>

namespace allocation_test {
thread_local bool reject{};
}

void *operator new(std::size_t bytes) {
  if (allocation_test::reject) {
    throw std::bad_alloc();
  }
  if (auto *memory = std::malloc(bytes ? bytes : 1)) {
    return memory;
  }
  throw std::bad_alloc();
}

void *operator new[](std::size_t bytes) {
  return ::operator new(bytes);
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

template <class T, class E> T take(std::expected<T, E> value) {
  REQUIRE(value);
  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

struct Fixture {
  std::shared_ptr<SourceBudget> budget = take(SourceBudget::create({.maxWork = 2, .maxConsumers = 2}));
  std::unique_ptr<Scheduler> scheduler = take(Scheduler::create());
  UiScope scope = take(scheduler->makeScope());
  unsigned callbacks{};
  SchedulerConnection<int> connection = take(scheduler->connect<int>(scope, [&](auto) {
    ++callbacks;
  }));

  SourceCapacityObserver observe() {
    return take(budget->observeCapacity(connection.publisher));
  }
};

constexpr SourceBudgetUsage JOB{8192, 0, 1, 0};
constexpr SourceBudgetUsage PEAK{16384, 1, 2, 0};
} // namespace

TEST_CASE("foreign credit release grants capacity without publishing UI payload or allocating") {
  Fixture f;
  auto observer = f.observe();
  auto foreign = take(f.budget->reserve({0, 0, 1, 0}));
  CHECK(observer.reserve(JOB, PEAK).error() == SourceError::WOULD_BLOCK);
  take(observer.arm(PEAK));
  CHECK_FALSE(observer.takeReady());
  const auto epoch = f.budget->capacityEpoch();
  allocation_test::reject = true;
  foreign = {};
  allocation_test::reject = false;
  CHECK(f.budget->capacityEpoch() != epoch);
  CHECK(observer.takeReady());
  CHECK_FALSE(observer.takeReady());
  auto job = take(observer.reserve(JOB, PEAK));
  CHECK(f.budget->usage().work == 1);
  take(f.scheduler->dispatch());
  CHECK(f.callbacks == 0);
}

TEST_CASE("a release before arm is not lost") {
  Fixture f;
  auto observer = f.observe();
  auto foreign = take(f.budget->reserve({0, 0, 2, 0}));
  CHECK(observer.reserve(JOB, PEAK).error() == SourceError::WOULD_BLOCK);
  foreign = {};
  take(observer.arm(PEAK));
  CHECK(observer.takeReady());
  CHECK(observer.reserve(JOB, PEAK));
}

TEST_CASE("one-writer priority prevents two viewers occupying both source work slots") {
  Fixture f;
  auto first = f.observe();
  auto second = f.observe();
  auto foreign = take(f.budget->reserve({0, 0, 2, 0}));
  take(first.arm(PEAK));
  take(second.arm(PEAK));
  foreign = {};
  const bool firstReady = first.takeReady();
  const bool secondReady = second.takeReady();
  CHECK(firstReady != secondReady);
  auto &winner = firstReady ? first : second;
  auto &waiting = firstReady ? second : first;
  CHECK(waiting.reserve(JOB, PEAK).error() == SourceError::WOULD_BLOCK);
  auto job = take(winner.reserve(JOB, PEAK));
  auto sourceRead = take(f.budget->reserve({0, 0, 1, 0}));
  sourceRead = {};
  CHECK_FALSE(waiting.takeReady());
  job = {};
  CHECK(waiting.takeReady());
  CHECK(waiting.reserve(JOB, PEAK));
}

TEST_CASE("impossible peaks fail permanently instead of waiting") {
  Fixture f;
  auto observer = f.observe();
  const auto impossible = SourceBudgetUsage{16384, 1, 3, 0};
  CHECK(observer.arm(impossible).error() == SourceError::PAGE_TOO_LARGE);
  CHECK(observer.reserve(JOB, impossible).error() == SourceError::PAGE_TOO_LARGE);
  CHECK_FALSE(observer.takeReady());
  CHECK(f.budget->usage().work == 0);
}

TEST_CASE("observer registration is bounded and unregister removes every charged entry") {
  Fixture f;
  const auto baseline = f.budget->usage().bytes;
  auto first = f.observe();
  auto second = f.observe();
  CHECK(f.budget->observeCapacity(f.connection.publisher).error() == SourceError::WOULD_BLOCK);
  first = {};
  second = {};
  CHECK(f.budget->usage().bytes == baseline);
  for (unsigned repeat = 0; repeat < 100; ++repeat) {
    auto observer = f.observe();
    take(observer.arm(PEAK));
  }
  CHECK(f.budget->usage().bytes == baseline);
}

TEST_CASE("unregister races safely with a worker releasing credit") {
  Fixture f;
  const auto baseline = f.budget->usage().bytes;
  for (unsigned repeat = 0; repeat < 100; ++repeat) {
    auto observer = f.observe();
    auto foreign = take(f.budget->reserve({0, 0, 2, 0}));
    take(observer.arm(PEAK));
    std::thread release([credit = std::move(foreign)]() mutable {
      credit = {};
    });
    observer = {};
    release.join();
    CHECK(f.budget->usage().work == 0);
    CHECK(f.budget->usage().bytes == baseline);
  }
}
