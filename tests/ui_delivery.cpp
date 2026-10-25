// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <portals/ui.hpp>
#include <portals/ui_delivery.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <latch>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
std::atomic<std::ptrdiff_t> allocationsBeforeFailure{-1};
std::atomic<std::size_t> retainedAllocationBytes{};

struct alignas(std::max_align_t) AllocationHeader {
  std::size_t size;
};

void releaseAllocation(void *memory) noexcept {
  if (memory) {
    auto allocation = static_cast<AllocationHeader *>(memory) - 1;
    retainedAllocationBytes -= allocation->size;
    std::free(allocation);
  }
}
} // namespace

// Injection is armed only around the public enqueue boundary, after immutable payload creation.
void *operator new(std::size_t size) {
  if (allocationsBeforeFailure == 0) {
    throw std::bad_alloc();
  }
  if (allocationsBeforeFailure > 0) {
    --allocationsBeforeFailure;
  }

  const auto bytes = size ? size : 1;
  if (bytes > std::numeric_limits<std::size_t>::max() - sizeof(AllocationHeader)) {
    throw std::bad_alloc();
  }
  if (auto memory = static_cast<AllocationHeader *>(std::malloc(bytes + sizeof(AllocationHeader)))) {
    memory->size = bytes;
    retainedAllocationBytes += bytes;
    return memory + 1;
  }

  throw std::bad_alloc();
}

void operator delete(void *memory) noexcept {
  releaseAllocation(memory);
}

void operator delete(void *memory, std::size_t) noexcept {
  releaseAllocation(memory);
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void operator delete[](void *memory) noexcept {
  releaseAllocation(memory);
}

void operator delete[](void *memory, std::size_t) noexcept {
  releaseAllocation(memory);
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
  releaseAllocation(memory);
}

void operator delete[](void *memory, const std::nothrow_t &) noexcept {
  releaseAllocation(memory);
}

namespace {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> result) {
  REQUIRE(result);
  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

bool hasCause(const ManagedError &failure, TreeError cause) {
  const auto original = std::get_if<TreeError>(&failure.cause);
  return original && *original == cause;
}

struct Fixture {
  std::unique_ptr<Application> app = take(Application::create());
  std::unique_ptr<ManagedScene> scene = take(ManagedScene::create(*app));
  ui::Ref<Label> label;

  Fixture() {
    take(ui::mount(*scene, ui::label("ready").reference(label)));
  }
};
} // namespace

TEST_CASE("UI delivery is deferred CONTROL FIFO and worker publication owns no callback") {
  Fixture fixture;
  const auto owner = std::this_thread::get_id();
  std::vector<int> values;
  auto endpoint = take(fixture.label.delivery<int>([&](auto value) {
    CHECK(std::this_thread::get_id() == owner);
    values.push_back(*value);
  }));
  std::optional<UiAcknowledgement> receipt;
  std::thread worker([endpoint, &receipt] {
    receipt = take(endpoint.postAcknowledged(std::make_shared<const int>(1)));
    take(endpoint.post(std::make_shared<const int>(2)));
  });
  worker.join();
  CHECK(values.empty());
  REQUIRE(receipt);
  CHECK(receipt->poll().state == UiDeliveryState::PENDING);
  CHECK(fixture.app->scheduler().metrics().control.pending == 2);
  take(fixture.app->scheduler().dispatch());
  CHECK(values == std::vector<int>{1, 2});
  CHECK(receipt->poll().state == UiDeliveryState::APPLIED);
}

TEST_CASE("UI delivery cancellation closes node and scene without retaining UI captures on workers") {
  Fixture fixture;
  const auto owner = std::this_thread::get_id();
  std::thread::id destroyed;

  struct Capture {
    std::thread::id &destroyed;

    ~Capture() {
      destroyed = std::this_thread::get_id();
    }
  };

  auto capture = std::make_shared<Capture>(destroyed);
  std::weak_ptr<Capture> observed = capture;
  auto endpoint = take(fixture.label.delivery<int>([capture](auto) {
  }));
  capture.reset();
  auto receipt = take(endpoint.postAcknowledged(std::make_shared<const int>(1)));
  take(fixture.scene->remove(fixture.label.node()));
  CHECK(receipt.poll().state == UiDeliveryState::CANCELLED);
  CHECK(observed.expired());
  CHECK(destroyed == owner);
  CHECK(endpoint.post(std::make_shared<const int>(2)).error() == SchedulerError::CLOSED);
  fixture.scene.reset();
  fixture.app.reset();
  std::thread worker([endpoint = std::move(endpoint)] {
    CHECK(endpoint.post(std::make_shared<const int>(3)).error() == SchedulerError::CLOSED);
  });
  worker.join();
}

TEST_CASE("typed delivery failures preserve cause and recoverable collector with acknowledgement") {
  Fixture fixture;
  const auto origin = std::source_location::current();
  auto endpoint = take(fixture.label.delivery<int>(
    [](auto) -> std::expected<void, TreeError> {
      return std::unexpected(TreeError::INVALID_ARGUMENT);
    },
    origin));
  auto receipt = take(endpoint.postAcknowledged(std::make_shared<const int>(1)));
  auto dispatch = take(fixture.app->scheduler().dispatch());
  CHECK(dispatch.callback_failures == 0);
  auto outcome = receipt.poll();
  CHECK(outcome.state == UiDeliveryState::FAILED);
  REQUIRE(outcome.error);
  CHECK(*outcome.error == TreeError::INVALID_ARGUMENT);
  auto diagnostics = take(fixture.scene->diagnostics());
  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(hasCause(diagnostics.errors.front().error, TreeError::INVALID_ARGUMENT));
  CHECK(diagnostics.errors.front().origin.line() == origin.line());
  CHECK(std::string_view(diagnostics.errors.front().origin.file_name()) == origin.file_name());
  take(endpoint.post(std::make_shared<const int>(2)));
  take(fixture.app->scheduler().dispatch());
}

TEST_CASE("throwing UI delivery acknowledges failure and disconnects through scheduler") {
  Fixture fixture;
  auto endpoint = take(fixture.label.delivery<int>([](auto) {
    throw std::runtime_error("delivery");
  }));
  auto receipt = take(endpoint.postAcknowledged(std::make_shared<const int>(1)));
  auto dispatch = take(fixture.app->scheduler().dispatch());
  CHECK(dispatch.callback_failures == 1);
  CHECK(receipt.poll().state == UiDeliveryState::FAILED);
  CHECK(receipt.poll().error == TreeError::CALLBACK_FAILED);
  CHECK(endpoint.post(std::make_shared<const int>(2)).error() == SchedulerError::CLOSED);
  CHECK(take(fixture.scene->diagnostics()).errors.size() == 1);
}

TEST_CASE("self removal keeps admitted callback alive and acknowledges completion") {
  Fixture fixture;
  bool returned{};
  auto endpoint = take(fixture.label.delivery<int>([&](auto) {
    take(fixture.scene->remove(fixture.label.node()));
    returned = true;
  }));
  auto receipt = take(endpoint.postAcknowledged(std::make_shared<const int>(1)));
  auto cancelled = take(endpoint.postAcknowledged(std::make_shared<const int>(2)));
  take(fixture.app->scheduler().dispatch());
  CHECK(returned);
  CHECK(receipt.poll().state == UiDeliveryState::APPLIED);
  CHECK(cancelled.poll().state == UiDeliveryState::CANCELLED);
}

TEST_CASE("full CONTROL queue rejects without altering accepted delivery") {
  Fixture fixture;
  int calls{};
  auto endpoint = take(fixture.label.delivery<int>([&](auto) {
    ++calls;
  }));
  std::vector<UiAcknowledgement> receipts;
  for (int index = 0; index < 32; ++index) {
    receipts.push_back(take(endpoint.postAcknowledged(std::make_shared<const int>(index))));
  }
  CHECK(endpoint.postAcknowledged(std::make_shared<const int>(99)).error() == SchedulerError::WOULD_BLOCK);
  CHECK(calls == 0);
  take(fixture.app->scheduler().dispatch());
  CHECK(calls == 32);
  for (const auto &receipt : receipts) {
    CHECK(receipt.poll().state == UiDeliveryState::APPLIED);
  }
}

TEST_CASE("button activation follows checked router semantics") {
  Fixture fixture;
  ui::Ref<Button> button;
  int calls{};
  take(ui::mount(*fixture.scene, ui::button(button, "run").onClick([&] {
    ++calls;
  })));
  take(button.activate().result());
  CHECK(calls == 1);
  take(fixture.scene->remove(button.node()));
  auto stale = button.activate();
  REQUIRE_FALSE(stale);
  CHECK(stale.result().error() == TreeError::STALE_NODE);
}

TEST_CASE("allocation failure and invalid payload do not alter accepted commands") {
  Fixture fixture;
  int calls{};
  auto endpoint = take(fixture.label.delivery<int>([&](auto) {
    ++calls;
  }));
  auto payload = std::make_shared<const int>(1);
  auto accepted = take(endpoint.postAcknowledged(payload));
  for (std::ptrdiff_t budget : {0, 1}) {
    allocationsBeforeFailure = budget;
    auto failed = endpoint.postAcknowledged(payload);
    allocationsBeforeFailure = -1;
    REQUIRE_FALSE(failed);
    CHECK(failed.error() == SchedulerError::NO_MEMORY);
    CHECK(fixture.app->scheduler().metrics().control.pending == 1);
  }
  CHECK(endpoint.post({}).error() == SchedulerError::INVALID_ARGUMENT);
  CHECK(accepted.poll().state == UiDeliveryState::PENDING);
  take(fixture.app->scheduler().dispatch());
  CHECK(calls == 1);
  CHECK(accepted.poll().state == UiDeliveryState::APPLIED);
}

TEST_CASE("scene close and scheduler stop cancel pending delivery") {
  Fixture fixture;
  auto endpoint = take(fixture.label.delivery<int>([](auto) {
    FAIL("closed delivery invoked");
  }));
  auto receipt = take(endpoint.postAcknowledged(std::make_shared<const int>(1)));
  fixture.scene.reset();
  CHECK(receipt.poll().state == UiDeliveryState::CANCELLED);
  CHECK(endpoint.post(std::make_shared<const int>(2)).error() == SchedulerError::CLOSED);

  Fixture stopped;
  auto sender = take(stopped.label.delivery<int>([](auto) {
    FAIL("stopped delivery invoked");
  }));
  auto cancelled = take(sender.postAcknowledged(std::make_shared<const int>(1)));
  take(stopped.app->scheduler().stop());
  CHECK(cancelled.poll().state == UiDeliveryState::CANCELLED);
  CHECK(sender.post(std::make_shared<const int>(2)).error() == SchedulerError::CLOSED);
}

TEST_CASE("foreign-thread registration and activation reject before scene reads") {
  Fixture fixture;
  ui::Ref<Button> button;
  take(ui::mount(*fixture.scene, ui::button(button, "run").onClick([] {
  })));
  std::optional<ManagedError> registration;
  std::optional<TreeError> activation;
  std::thread worker([&] {
    registration = fixture.label
                     .delivery<int>([](auto) {
                     })
                     .error();
    activation = button.activate().result().error();
  });
  worker.join();
  REQUIRE(registration);
  REQUIRE(activation);
  CHECK(hasCause(*registration, TreeError::WRONG_THREAD));
  CHECK(*activation == TreeError::WRONG_THREAD);
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
}

TEST_CASE("returned activation errors preserve a single collector receipt") {
  Fixture fixture;
  ui::Ref<Button> retired;
  ui::Ref<Button> caller;
  take(ui::mount(*fixture.scene, ui::button(retired, "retired").onClick([] {
  })));
  take(fixture.scene->remove(retired.node()));
  take(ui::mount(*fixture.scene, ui::button(caller, "caller").onClick([retired] {
    return retired.activate();
  })));
  take(fixture.app->router().invoke(caller.node(), {Action::ACTIVATE}));
  auto diagnostics = take(fixture.scene->diagnostics());
  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(hasCause(diagnostics.errors.front().error, TreeError::WOULD_BLOCK));
  CHECK(diagnostics.errors.front().error.operation == std::string_view("button.activate"));
}

TEST_CASE("self scene destruction retains the admitted delivery until return") {
  Fixture fixture;
  bool returned{};
  auto endpoint = take(fixture.label.delivery<int>([&](auto) {
    fixture.scene.reset();
    returned = true;
  }));
  auto receipt = take(endpoint.postAcknowledged(std::make_shared<const int>(1)));
  take(fixture.app->scheduler().dispatch());
  CHECK(returned);
  CHECK(receipt.poll().state == UiDeliveryState::APPLIED);
  CHECK(endpoint.post(std::make_shared<const int>(2)).error() == SchedulerError::CLOSED);
}

TEST_CASE("acknowledgement describes callback return rather than ignored nested command failures") {
  Fixture fixture;
  ui::Ref<Gauge> gauge;
  take(ui::mount(*fixture.scene, ui::gauge(gauge, .25)));
  auto endpoint = take(fixture.label.delivery<int>([gauge](auto) {
    (void)gauge.value(2);
  }));
  auto receipt = take(endpoint.postAcknowledged(std::make_shared<const int>(1)));
  take(fixture.app->scheduler().dispatch());
  CHECK(receipt.poll().state == UiDeliveryState::APPLIED);
  auto diagnostics = take(fixture.scene->diagnostics());
  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(hasCause(diagnostics.errors.front().error, TreeError::INVALID_ARGUMENT));
}

TEST_CASE("disconnected delivery bookkeeping remains bounded across repeated throwing registrations") {
  Fixture fixture;
  const auto payload = std::make_shared<const int>(1);
  auto replace = [&] {
    auto endpoint = take(fixture.label.delivery<int>([](auto) {
      throw 1;
    }));
    take(endpoint.post(payload));
    CHECK(take(fixture.app->scheduler().dispatch()).callback_failures == 1);
  };
  constexpr int WARMUP_REGISTRATIONS = 256;
  constexpr int REPLACEMENT_REGISTRATIONS = 2048;
  for (int index = 0; index < WARMUP_REGISTRATIONS; ++index) {
    replace();
  }
  const auto warmBytes = retainedAllocationBytes.load();
  for (int index = 0; index < REPLACEMENT_REGISTRATIONS; ++index) {
    replace();
  }
  const auto replacedBytes = retainedAllocationBytes.load();
  MESSAGE("retained allocation bytes: warm=", warmBytes, ", after replacements=", replacedBytes);
  CHECK(replacedBytes <= warmBytes);
  CHECK(fixture.app->scheduler().metrics().callback_failures == WARMUP_REGISTRATIONS + REPLACEMENT_REGISTRATIONS);
}

TEST_CASE("worker polls acknowledgement concurrently with UI completion and observes the exact failure") {
  Fixture fixture;
  auto endpoint = take(fixture.label.delivery<int>([](auto) -> std::expected<void, TreeError> {
    return std::unexpected(TreeError::INVALID_GEOMETRY);
  }));
  auto receipt = take(endpoint.postAcknowledged(std::make_shared<const int>(1)));
  std::latch polling{1};
  UiDeliveryOutcome observed;
  bool observedPending{};
  std::jthread worker([receipt, &polling, &observed, &observedPending] {
    constexpr auto WORKER_POLL_TIMEOUT = std::chrono::seconds(2);
    const auto deadline = std::chrono::steady_clock::now() + WORKER_POLL_TIMEOUT;
    observed = receipt.poll();
    observedPending = observed.state == UiDeliveryState::PENDING;
    polling.count_down();
    while (observed.state == UiDeliveryState::PENDING && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
      observed = receipt.poll();
    }
  });
  // The test host rendezvous precedes dispatch; UI never polls or waits on the execution receipt.
  polling.wait();
  take(fixture.app->scheduler().dispatch());
  worker.join();
  CHECK(observedPending);
  CHECK(observed.state == UiDeliveryState::FAILED);
  REQUIRE(observed.error);
  CHECK(*observed.error == TreeError::INVALID_GEOMETRY);
  auto diagnostics = take(fixture.scene->diagnostics());
  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(hasCause(diagnostics.errors.front().error, TreeError::INVALID_GEOMETRY));
}
