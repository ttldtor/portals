// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <portals/ui_app.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <new>
#include <utility>

namespace {
std::atomic<std::ptrdiff_t> allocationsBeforeFailure{-1};
std::atomic<std::ptrdiff_t> activeBudget{-1};
constexpr std::ptrdiff_t MAX_ALLOCATION_BUDGET = 4096;

void oomTerminate() {
  allocationsBeforeFailure = -1;
  std::fprintf(stderr, "snapshot OOM terminate at budget=%td\n", activeBudget.load());
  std::abort();
}

struct AllocationBudget {
  explicit AllocationBudget(std::ptrdiff_t budget) noexcept : previous(std::set_terminate(oomTerminate)) {
    activeBudget = budget;
    allocationsBeforeFailure = budget;
  }

  ~AllocationBudget() {
    allocationsBeforeFailure = -1;
    activeBudget = -1;
    std::set_terminate(previous);
  }

  AllocationBudget(const AllocationBudget &) = delete;
  AllocationBudget &operator=(const AllocationBudget &) = delete;
  std::terminate_handler previous;
};

template <class Capture> auto withBudget(std::ptrdiff_t budget, Capture &&capture) {
  AllocationBudget fault(budget);

  return std::forward<Capture>(capture)();
}

void checkUnpublishedContext() {
  const auto arguments = portals::ui::app::args();
  const auto environment = portals::ui::app::environment();

  REQUIRE_FALSE(arguments);
  REQUIRE_FALSE(environment);
  CHECK(arguments.error() == portals::ui::app::ProcessContextError::UNINITIALIZED);
  CHECK(environment.error() == portals::ui::app::ProcessContextError::UNINITIALIZED);
}
} // namespace

// The fault injector is process-local; all diagnostics/assertions run after its scope has ended.
void *operator new(std::size_t size) {
  if (allocationsBeforeFailure.load() == 0) {
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

TEST_CASE("argument snapshot allocation budgets preserve typed failures") {
  using namespace portals::ui::app;

  const char *values[]{"synthetic-snapshot", "", "\xc3\x84"};
  bool completed{};

  checkUnpublishedContext();

  for (std::ptrdiff_t budget = 0; budget <= MAX_ALLOCATION_BUDGET; ++budget) {
    const auto captured = withBudget(budget, [&] {
      return Arguments::capture(3, values);
    });

    if (!captured) {
      CHECK(captured.error() == ProcessContextError::NO_MEMORY);
      checkUnpublishedContext();

      continue;
    }

    CHECK(captured->argc() == 3);
    CHECK((*captured)[0] == "synthetic-snapshot");
    CHECK((*captured)[1].empty());
    CHECK((*captured)[2] == "\xc3\x84");
    CHECK(captured->argv()[3] == nullptr);
    checkUnpublishedContext();
    std::fprintf(stderr, "Arguments::capture first_success_budget=%td\n", budget);
    completed = true;

    break;
  }

  CHECK(completed);
}

TEST_CASE("synthetic environment allocation budgets preserve typed failures") {
  using namespace portals::ui::app;

  const std::array entries{Environment::Entry{"SYNTHETIC_\xc3\x84", ""},
                           Environment::Entry{"SYNTHETIC_VALUE", "\xe2\x82\xac"}};
  bool completed{};

  checkUnpublishedContext();

  for (std::ptrdiff_t budget = 0; budget <= MAX_ALLOCATION_BUDGET; ++budget) {
    const auto captured = withBudget(budget, [&] {
      return Environment::fromEntries(entries);
    });

    if (!captured) {
      CHECK(captured.error() == ProcessContextError::NO_MEMORY);
      checkUnpublishedContext();

      continue;
    }

    CHECK(captured->size() == 2);

    const auto empty = captured->get("SYNTHETIC_\xc3\x84");
    const auto unicode = captured->get("SYNTHETIC_VALUE");

    REQUIRE(empty);
    REQUIRE(*empty);
    CHECK((**empty).empty());
    REQUIRE(unicode);
    REQUIRE(*unicode);
    CHECK(**unicode == "\xe2\x82\xac");
    checkUnpublishedContext();
    std::fprintf(stderr, "Environment::fromEntries first_success_budget=%td\n", budget);
    completed = true;

    break;
  }

  CHECK(completed);
}

TEST_CASE("native process capture allocation budgets publish only complete snapshots") {
  using namespace portals::ui::app;

  const char *values[]{"synthetic-native-capture", ""};
  bool completed{};

  checkUnpublishedContext();

  for (std::ptrdiff_t budget = 0; budget <= MAX_ALLOCATION_BUDGET; ++budget) {
    const auto initialized = withBudget(budget, [&] {
      return initialize(2, values);
    });

    if (!initialized) {
      CHECK(initialized.error() == ProcessContextError::NO_MEMORY);
      checkUnpublishedContext();

      continue;
    }

    const auto arguments = args();
    const auto capturedEnvironment = environment();

    REQUIRE(arguments);
    REQUIRE(capturedEnvironment);
    CHECK(arguments->argc() == 2);
    CHECK((*arguments)[0] == "synthetic-native-capture");
    CHECK((*arguments)[1].empty());
    std::fprintf(stderr, "app::initialize native first_success_budget=%td\n", budget);
    completed = true;

    break;
  }

  CHECK(completed);
}
