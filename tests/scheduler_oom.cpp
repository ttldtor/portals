// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/scheduler.hpp>

#include <array>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <new>

namespace {
std::ptrdiff_t allocationsBeforeFailure{-1};
std::size_t allocationAttempts{};

void oomTerminate() {
  allocationsBeforeFailure = -1;
  std::abort();
}

struct AllocationFailure {
  std::terminate_handler previous{std::set_terminate(oomTerminate)};

  explicit AllocationFailure(std::ptrdiff_t budget) {
    allocationAttempts = 0;
    allocationsBeforeFailure = budget;
  }

  ~AllocationFailure() {
    allocationsBeforeFailure = -1;
    std::set_terminate(previous);
  }
};
} // namespace

// Fault injection is process-local and armed only around the checked public boundary.
void *operator new(std::size_t size) {
  if (allocationsBeforeFailure >= 0) {
    ++allocationAttempts;

    if (!allocationsBeforeFailure) {
      throw std::bad_alloc();
    }

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
  return ::operator new(size, std::nothrow);
}

void operator delete(void *memory, const std::nothrow_t &) noexcept {
  ::operator delete(memory);
}

void operator delete[](void *memory, const std::nothrow_t &) noexcept {
  ::operator delete(memory);
}

namespace {
using namespace portals;
int failures{};

void check(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << '\n';
    ++failures;
  }
}

void createAndUnwind() {
  constexpr std::ptrdiff_t MAX_BUDGET = 24;
  std::size_t rejected{}, created{};

  for (std::ptrdiff_t budget = 0; budget <= MAX_BUDGET; ++budget) {
    std::expected<std::unique_ptr<Scheduler>, SchedulerError> result;

    {
      AllocationFailure fault(budget);

      result = Scheduler::create({.data_capacity = 1, .control_capacity = 1, .subscription_capacity = 1});
    }

    if (result) {
      ++created;
    } else {
      ++rejected;
      check(result.error() == SchedulerError::NO_MEMORY, "create preserves typed NO_MEMORY during unwind");
    }
  }

  check(rejected >= 3 && created > 0, "allocation budgets exercise partial create and successful ownership");
}

struct Reentry {
  Scheduler &scheduler;
  Publisher<int> &publisher;
  std::shared_ptr<const int> probe;
  std::array<int, 3> order{};
  std::size_t released{};
  bool wholeStateEmpty{true}, nestedStopSucceeded{true}, publicationClosed{true};

  void observe(int kind) {
    if (released < order.size()) {
      order[released] = kind;
    }

    ++released;

    const auto metrics = scheduler.metrics();

    wholeStateEmpty &= metrics.control.pending == 0 && metrics.data.pending == 0 && metrics.control.cancelled == 1 &&
                       metrics.data.cancelled == 1;
    nestedStopSucceeded &= scheduler.stop().has_value();

    const auto publication = publisher.publish(probe);

    publicationClosed &= !publication && publication.error() == SchedulerError::CLOSED;
  }
};

struct Capture {
  Reentry &reentry;

  ~Capture() {
    reentry.observe(3);
  }
};

void stopRetiresWholeState() {
  auto scheduler = Scheduler::create({.data_capacity = 1, .control_capacity = 1, .subscription_capacity = 2});

  check(scheduler.has_value(), "scheduler created for retirement test");

  if (!scheduler) {
    return;
  }

  auto scope = (*scheduler)->makeScope();
  Publisher<int> endpoint;
  Reentry reentry{**scheduler, endpoint, std::make_shared<const int>(0)};
  auto capture = std::make_shared<Capture>(reentry);
  auto data = (*scheduler)->connect<int>(*scope, [capture](auto) {
  });
  auto control = (*scheduler)
                   ->connect<int>(
                     *scope,
                     [](auto) {
                     },
                     SchedulerLane::CONTROL);

  check(data && control, "data and control registered");

  if (!data || !control) {
    return;
  }

  endpoint = data->publisher;
  capture.reset();

  auto dataSnapshot = std::shared_ptr<const int>(new int(1), [&](const int *value) {
    reentry.observe(2);
    delete value;
  });
  auto controlSnapshot = std::shared_ptr<const int>(new int(2), [&](const int *value) {
    reentry.observe(1);
    delete value;
  });

  check(data->publisher.publish(std::move(dataSnapshot)).has_value(), "data snapshot queued");
  check(control->publisher.publish(std::move(controlSnapshot)).has_value(), "control snapshot queued");

  std::expected<void, SchedulerError> stopped;
  std::size_t attempts{};

  {
    AllocationFailure fault(0);

    stopped = (*scheduler)->stop();
    attempts = allocationAttempts;
  }

  check(stopped.has_value() && attempts == 0, "stop and reentrant deleters allocate no storage");
  check(reentry.released == 3 && reentry.order == std::array{1, 2, 3},
        "off-lock retirement order is control/data/captures");
  check(reentry.wholeStateEmpty && reentry.nestedStopSucceeded && reentry.publicationClosed,
        "all reentrant releases observe complete cancellation and CLOSED publication");
  check((*scheduler)->stop().has_value(), "repeated stop succeeds without repeated releases");
  check(reentry.released == 3, "each user owner is retired exactly once");
}

void destructionDoesNotAllocate() {
  auto scheduler = Scheduler::create();

  check(scheduler.has_value(), "scheduler created for destructor test");

  if (!scheduler) {
    return;
  }

  std::size_t attempts{};

  {
    AllocationFailure fault(0);

    scheduler->reset();
    attempts = allocationAttempts;
  }

  check(attempts == 0, "scheduler destructor never allocates retirement proxies");
}
} // namespace

int main() {
  createAndUnwind();
  stopRetiresWholeState();
  destructionDoesNotAllocate();

  return failures ? 1 : 0;
}
