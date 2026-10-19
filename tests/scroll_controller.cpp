// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "../src/views/scroll_controller_state.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <thread>

namespace {
thread_local int allocationFailureAfter{-1};
}

void *operator new(std::size_t size) {
  if (allocationFailureAfter == 0) {
    allocationFailureAfter = -1;

    throw std::bad_alloc();
  }

  if (allocationFailureAfter > 0) {
    --allocationFailureAfter;
  }

  if (auto *storage = std::malloc(size == 0 ? 1 : size)) {
    return storage;
  }

  throw std::bad_alloc();
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete(void *storage) noexcept {
  std::free(storage);
}

void operator delete[](void *storage) noexcept {
  std::free(storage);
}

void operator delete(void *storage, std::size_t) noexcept {
  std::free(storage);
}

void operator delete[](void *storage, std::size_t) noexcept {
  std::free(storage);
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
using namespace portals::detail;
constexpr auto MAXIMUM = std::numeric_limits<std::uint64_t>::max();
int failures{};

void check(bool value, const char *message) {
  if (!value) {
    std::cerr << message << '\n';
    ++failures;
  }
}

template <class T> T take(std::expected<T, TreeError> result) {
  if (!result) {
    std::cerr << "unexpected fixture error " << static_cast<int>(result.error()) << '\n';
    std::abort();
  }

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

void positions() {
  auto controller = take(ScrollController::create({{{100, 10}, {}}, {{MAXIMUM, 1}, {(1ULL << 53) + 1, 0.5}}}));
  auto lease = take(ScrollControllerAccess::acquire(controller, 1, true));
  const auto original = take(controller->snapshot());
  auto plan = take(ScrollControllerAccess::stage(lease));

  take(ScrollControllerAccess::move(plan, ScrollAxis::HORIZONTAL, 0.5));

  const auto first = take(ScrollControllerAccess::commit(lease, std::move(plan)));
  const auto fractional = take(controller->snapshot());

  check(first.logicalChanged && !first.rasterChanged && !first.metricsChanged &&
          fractional.horizontal.offset.raster == 0 && fractional.horizontal.offset.fraction == 0.5,
        "fractional movement is logical, not raster movement");
  check(original.horizontal.offset.fraction == 0 && original.revision == 1,
        "retained snapshot owns the original value");

  plan = take(ScrollControllerAccess::stage(lease));
  take(ScrollControllerAccess::move(plan, ScrollAxis::HORIZONTAL, 0.5));

  const auto second = take(ScrollControllerAccess::commit(lease, std::move(plan)));

  check(second.logicalChanged && second.rasterChanged && take(controller->snapshot()).horizontal.offset.raster == 1,
        "two fractional steps reach one raster cell");

  plan = take(ScrollControllerAccess::stage(lease));
  take(ScrollControllerAccess::step(plan, ScrollAxis::VERTICAL, 1, true));
  take(ScrollControllerAccess::commit(lease, std::move(plan)));

  const auto exact = take(controller->snapshot());

  check(exact.vertical.offset.raster == (1ULL << 53) + 2 && exact.vertical.offset.fraction == 0.5,
        "integer step preserves an exact index and fraction above 2^53");

  plan = take(ScrollControllerAccess::stage(lease));
  take(ScrollControllerAccess::step(plan, ScrollAxis::VERTICAL, MAXIMUM, true));
  take(ScrollControllerAccess::commit(lease, std::move(plan)));

  check(take(controller->snapshot()).vertical.offset.raster == MAXIMUM - 1 &&
          take(controller->snapshot()).vertical.offset.fraction == 0,
        "forward step saturates without overflow");

  plan = take(ScrollControllerAccess::stage(lease));
  take(ScrollControllerAccess::step(plan, ScrollAxis::VERTICAL, MAXIMUM, false));
  take(ScrollControllerAccess::commit(lease, std::move(plan)));

  check(take(controller->snapshot()).vertical.offset.raster == 0, "backward step saturates without underflow");

  plan = take(ScrollControllerAccess::stage(lease));
  take(ScrollControllerAccess::offset(plan, ScrollAxis::HORIZONTAL, {90, 0.75}));
  take(ScrollControllerAccess::commit(lease, std::move(plan)));

  check(take(controller->snapshot()).horizontal.offset.fraction == 0, "viewport endpoint discards overscroll fraction");

  const auto revision = take(controller->snapshot()).revision;

  plan = take(ScrollControllerAccess::stage(lease));
  take(ScrollControllerAccess::move(plan, ScrollAxis::HORIZONTAL, 0.5));

  const auto boundary = take(ScrollControllerAccess::commit(lease, std::move(plan)));

  check(!boundary.logicalChanged && !boundary.rasterChanged && take(controller->snapshot()).revision == revision,
        "exact boundary is a no-op without a revision");

  plan = take(ScrollControllerAccess::stage(lease));
  take(ScrollControllerAccess::metrics(lease, plan, {100, 0}, {0, MAXIMUM}));

  const auto resized = take(ScrollControllerAccess::commit(lease, std::move(plan)));

  check(resized.metricsChanged && resized.rasterChanged && take(controller->snapshot()).horizontal.offset.raster == 0,
        "visible zero viewport and empty extent clamp to zero");
}

void admission() {
  auto controller = take(ScrollController::create({{{100, 10}, {20, 0}}, {}}));
  auto writer = take(ScrollControllerAccess::acquire(controller, 10, true));
  auto projection = take(ScrollControllerAccess::acquire(controller, 10, false));
  auto duplicate = ScrollControllerAccess::acquire(controller, 10, true);
  auto foreign = ScrollControllerAccess::acquire(controller, 11, false);
  auto plan = take(ScrollControllerAccess::stage(projection));

  check(!duplicate && duplicate.error() == TreeError::INVALID_ARGUMENT, "a second geometry writer is rejected");
  check(!foreign && foreign.error() == TreeError::WRONG_SESSION, "projections cannot span tree sessions");
  check(!ScrollControllerAccess::metrics(projection, plan, {10, 10}, {}), "projection cannot write metrics");

  auto stale = take(ScrollControllerAccess::stage(writer));

  take(ScrollControllerAccess::offset(plan, ScrollAxis::HORIZONTAL, {30, 0}));
  take(ScrollControllerAccess::commit(projection, std::move(plan)));

  auto rejected = ScrollControllerAccess::commit(writer, std::move(stale));

  check(!rejected && rejected.error() == TreeError::WOULD_BLOCK &&
          take(controller->snapshot()).horizontal.offset.raster == 30,
        "stale plan cannot undo a newer commit");

  plan = take(ScrollControllerAccess::stage(writer));

  const auto before = plan.horizontal.rasterOffset();

  check(!ScrollControllerAccess::offset(plan, ScrollAxis::HORIZONTAL, {99, std::numeric_limits<double>::quiet_NaN()}),
        "invalid fractional offset rejected");
  check(plan.horizontal.rasterOffset() == before, "invalid set leaves staged state unchanged");
  check(!ScrollControllerAccess::move(plan, ScrollAxis::HORIZONTAL, std::numeric_limits<double>::infinity()),
        "nonfinite wheel movement rejected");
  check(plan.horizontal.rasterOffset() == before, "invalid movement leaves staged state unchanged");
  check(!ScrollControllerAccess::step(plan, static_cast<ScrollAxis>(99), 1, true), "unknown axis rejected");

  bool affinityChecks{};

  std::thread worker([&] {
    const auto observed = controller->snapshot();
    const auto acquired = ScrollControllerAccess::acquire(controller, 10, false);
    const auto staged = ScrollControllerAccess::stage(writer);
    const auto moved = ScrollControllerAccess::step(plan, ScrollAxis::HORIZONTAL, 1, true);

    affinityChecks = !observed && observed.error() == TreeError::WRONG_THREAD && !acquired &&
                     acquired.error() == TreeError::WRONG_THREAD && !staged &&
                     staged.error() == TreeError::WRONG_THREAD && !moved && moved.error() == TreeError::WRONG_THREAD;
  });

  worker.join();

  check(affinityChecks && plan.horizontal.rasterOffset() == before, "wrong-thread boundaries reject before mutation");
}

void lifetime() {
  auto controller = take(ScrollController::create());
  std::optional<ScrollControllerLease> writer(take(ScrollControllerAccess::acquire(controller, 50, true)));
  std::optional<ScrollControllerLease> projection(take(ScrollControllerAccess::acquire(controller, 50, false)));
  auto oldPlan = take(ScrollControllerAccess::stage(*writer));
  auto retiredMetrics = oldPlan;

  take(ScrollControllerAccess::metrics(*writer, retiredMetrics, {100, 10}, {}));

  writer.reset();

  auto replacement = take(ScrollControllerAccess::acquire(controller, 50, true));
  auto retiredWriter = ScrollControllerAccess::commit(replacement, std::move(retiredMetrics));

  check(!retiredWriter && retiredWriter.error() == TreeError::INVALID_ARGUMENT,
        "staged metrics do not keep or transfer a retired writer capability");

  check(!ScrollControllerAccess::acquire(controller, 51, false), "live projection retains its session association");

  replacement = take(ScrollControllerAccess::acquire(take(ScrollController::create()), 99, true));
  projection.reset();

  auto newSession = take(ScrollControllerAccess::acquire(controller, 51, true));
  auto wrongAssociation = ScrollControllerAccess::commit(newSession, std::move(oldPlan));

  check(!wrongAssociation && wrongAssociation.error() == TreeError::WRONG_SESSION,
        "old plan grants neither an expired session nor a writer lease");

  const auto retained = take(controller->snapshot());

  controller.reset();

  check(retained.revision == 1 && take(newSession.owner->snapshot()).revision == 1,
        "projection and owning snapshot survive the caller handle");
}

void allocations() {
  for (int failure = 0; failure < 3; ++failure) {
    allocationFailureAfter = failure;

    const auto result = ScrollController::create();

    allocationFailureAfter = -1;

    check(!result && result.error() == TreeError::NO_MEMORY, "controller creation translates each allocation failure");
  }

  for (int failure = 0; failure < 2; ++failure) {
    auto controller = take(ScrollController::create());

    allocationFailureAfter = failure;

    const auto rejected = ScrollControllerAccess::acquire(controller, 1, true);

    allocationFailureAfter = -1;

    check(!rejected && rejected.error() == TreeError::NO_MEMORY, "writer acquisition translates allocation failure");

    auto different = ScrollControllerAccess::acquire(controller, 2, true);

    check(different.has_value(), "failed acquisition publishes no session or writer");
  }

  for (const auto fraction :
       {-1.0, 1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
    const auto result = ScrollController::create({{{1, 1}, {0, fraction}}, {}});

    check(!result && result.error() == TreeError::INVALID_ARGUMENT, "invalid initial fraction rejected");
  }
}
} // namespace

int main() {
  positions();
  admission();
  lifetime();
  allocations();

  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
