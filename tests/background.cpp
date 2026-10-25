// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/application.hpp>
#include <portals/background.hpp>

#include "support/examples/replay/terminal.hpp"

#include <future>
#include <latch>

namespace {
using namespace portals;
using namespace std::chrono_literals;

template <class T, class E> T take(std::expected<T, E> value) {
  REQUIRE(value);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

struct Probe {
  std::promise<void> entered;
  std::latch release{1};
  std::atomic<unsigned> runs{}, publications{};
  std::atomic<bool> available{true}, finished{};
  std::atomic<const void *> prepared{};
  std::thread::id worker;
};

struct Release {
  std::shared_ptr<Probe> probe;
  bool released{};

  void now() {
    if (!released) {
      probe->release.count_down();
      released = true;
    }
  }

  ~Release() {
    now();
  }
};

class ProbeJob final : public BackgroundJob {
public:
  explicit ProbeJob(std::shared_ptr<Probe> probe) : probe_(std::move(probe)) {
  }

  void run(std::stop_token) noexcept override {
    ++probe_->runs;
    probe_->worker = std::this_thread::get_id();
    probe_->entered.set_value();
    probe_->release.wait();
  }

  std::expected<void, AsyncError> publish() override {
    ++probe_->publications;

    if (!probe_->available) {
      return std::unexpected(AsyncError::WOULD_BLOCK);
    }

    probe_->finished = true;

    return {};
  }

private:
  std::shared_ptr<Probe> probe_;
};

std::unique_ptr<BackgroundJob> job(std::shared_ptr<Probe> probe) {
  return std::make_unique<ProbeJob>(std::move(probe));
}

template <class Predicate> bool eventually(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;

  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }

  return predicate();
}

TEST_CASE("background cancellation and stop do not wait for running user work") {
  auto pool = take(BackgroundPool::create({1, 1}));
  auto probe = std::make_shared<Probe>();
  auto entered = probe->entered.get_future();
  auto pending = job(probe);
  auto ticket = take(pool->submit(pending));
  Release release{probe};

  REQUIRE(entered.wait_for(5s) == std::future_status::ready);
  CHECK(probe->worker != std::this_thread::get_id());

  ticket.cancel();
  pool->requestStop();

  CHECK(probe->publications == 0);
  CHECK_FALSE(probe->finished);
  CHECK_FALSE(pending);
}

TEST_CASE("background full admission preserves input and latest replaces only the pending intent") {
  auto pool = take(BackgroundPool::create({1, 1}));
  auto first = std::make_shared<Probe>();
  auto entered = first->entered.get_future();
  auto running = job(first);
  auto firstTicket = take(pool->submit(running));
  Release release{first};

  REQUIRE(entered.wait_for(5s) == std::future_status::ready);

  auto second = std::make_shared<Probe>();
  auto queued = job(second);
  auto secondTicket = take(pool->submit(queued));
  Release releaseSecond{second};
  auto rejected = job(std::make_shared<Probe>());
  const auto original = rejected.get();
  auto full = pool->submit(rejected);

  REQUIRE_FALSE(full);
  CHECK(full.error() == BackgroundError::WOULD_BLOCK);
  CHECK(rejected.get() == original);

  BackgroundLatest latest(*pool);
  auto stale = std::make_shared<Probe>();
  auto current = std::make_shared<Probe>();
  Release releaseCurrent{current};

  CHECK(take(latest.replace(job(stale))) == BackgroundAdmission::RETAINED);
  CHECK(take(latest.replace(job(current))) == BackgroundAdmission::RETAINED);

  release.now();
  releaseSecond.now();

  REQUIRE(eventually([&] {
    pool->service();
    return take(latest.service()) == BackgroundAdmission::STARTED;
  }));
  CHECK(stale->runs == 0);
  REQUIRE(eventually([&] {
    return current->runs == 1;
  }));

  pool->requestStop();
}

TEST_CASE("background unpublished results occupy bounded worker reservations and run once") {
  auto pool = take(BackgroundPool::create({1, 1}));
  auto probe = std::make_shared<Probe>();
  auto entered = probe->entered.get_future();
  auto pending = job(probe);
  auto ticket = take(pool->submit(pending));
  Release release{probe};

  probe->available = false;
  REQUIRE(entered.wait_for(5s) == std::future_status::ready);
  release.now();
  REQUIRE(eventually([&] {
    return probe->publications == 1;
  }));

  auto second = std::make_shared<Probe>();
  auto queued = job(second);
  auto queuedTicket = take(pool->submit(queued));
  Release releaseSecond{second};
  auto rejected = job(std::make_shared<Probe>());

  CHECK_FALSE(pool->submit(rejected));
  CHECK(probe->runs == 1);
  CHECK(second->runs == 0);

  probe->available = true;

  REQUIRE(eventually([&] {
    pool->service();
    return probe->finished.load();
  }));
  CHECK(probe->runs == 1);

  pool->requestStop();
}

TEST_CASE("background stop releases an unpublished result without UI acknowledgement") {
  auto pool = take(BackgroundPool::create({1, 1}));
  auto probe = std::make_shared<Probe>();
  auto entered = probe->entered.get_future();
  auto pending = job(probe);
  auto ticket = take(pool->submit(pending));
  Release release{probe};

  probe->available = false;
  REQUIRE(entered.wait_for(5s) == std::future_status::ready);
  release.now();
  REQUIRE(eventually([&] {
    return probe->publications == 1;
  }));

  pool->requestStop();
  pool->join();

  CHECK(probe->runs == 1);
  CHECK_FALSE(probe->finished);
}

TEST_CASE("queued work wakes an idle worker alongside a blocked publisher without service") {
  auto pool = take(BackgroundPool::create({2, 2}));
  auto blocked = std::make_shared<Probe>();
  auto finishing = std::make_shared<Probe>();
  auto blockedEntered = blocked->entered.get_future();
  auto finishingEntered = finishing->entered.get_future();
  auto first = job(blocked);
  auto firstTicket = take(pool->submit(first));
  auto second = job(finishing);
  auto secondTicket = take(pool->submit(second));
  Release releaseFirst{blocked}, releaseSecond{finishing};

  blocked->available = false;
  REQUIRE(blockedEntered.wait_for(5s) == std::future_status::ready);
  REQUIRE(finishingEntered.wait_for(5s) == std::future_status::ready);

  releaseFirst.now();

  REQUIRE(eventually([&] {
    return blocked->publications != 0;
  }));
  // Establish an older publication waiter before the other worker becomes idle.
  // Scheduling is not a public synchronization seam; this is one bounded liveness
  // probe, supplemented by the admissible notify_one interleaving in review.
  std::this_thread::sleep_for(10ms);

  releaseSecond.now();

  REQUIRE(eventually([&] {
    return finishing->finished.load();
  }));
  std::this_thread::sleep_for(10ms);

  auto foreign = std::make_shared<Probe>();
  auto foreignEntered = foreign->entered.get_future();
  Release releaseForeign{foreign};

  releaseForeign.now();

  auto pending = job(foreign);
  auto ticket = take(pool->submit(pending));

  // No service, cancellation, or pool stop may rescue a missed queue wake.
  REQUIRE(foreignEntered.wait_for(300ms) == std::future_status::ready);
  CHECK(foreign->runs == 1);
}

struct Fixture {
  std::unique_ptr<Scheduler> scheduler;
  std::unique_ptr<WidgetTree> tree;
  std::unique_ptr<UiAsync> owner;

  explicit Fixture(SchedulerOptions options = {}) {
    scheduler = take(Scheduler::create(options));
    tree = take(WidgetTree::create(*scheduler));
    owner = take(UiAsync::create(*scheduler, *tree, take(tree->root())));
  }
};

enum class DomainError {
  REJECTED
};
using DomainResult = std::expected<int, DomainError>;

struct UiRecord {
  std::optional<DomainResult> result;
  std::optional<AsyncError> failure;
  std::thread::id resumed;
  const void *published{};
};

struct TransformInput {
  int value{};
  std::shared_ptr<Probe> probe;
};

std::shared_ptr<const DomainResult> transform(const TransformInput &input, std::stop_token) {
  ++input.probe->runs;
  input.probe->worker = std::this_thread::get_id();
  input.probe->entered.set_value();
  input.probe->release.wait();

  if (input.value < 0) {
    return std::make_shared<const DomainResult>(std::unexpected(DomainError::REJECTED));
  }

  auto result = std::make_shared<const DomainResult>(input.value * 2);

  input.probe->prepared = result.get();

  return result;
}

UiTask transformClient(UiAsyncContext context, std::shared_ptr<BackgroundLatest> source,
                       std::shared_ptr<const TransformInput> input, std::shared_ptr<UiRecord> record) {
  auto result = co_await context.receive<DomainResult>([source, input](AsyncReply<DomainResult> reply) {
    auto prepared = makeBackgroundJob(input, std::move(reply), transform);
    REQUIRE(prepared);
    REQUIRE(source->replace(std::move(*prepared)));
  });

  record->resumed = std::this_thread::get_id();

  if (!result) {
    record->failure = result.error();

    co_return std::unexpected(result.error());
  }

  record->result = **result;
  record->published = result->get();

  co_return std::expected<void, AsyncError>{};
}

TEST_CASE("background complete client resumes only through UI dispatch and preserves domain failures") {
  Fixture fixture;
  auto pool = take(BackgroundPool::create({1, 1}));
  auto source = std::make_shared<BackgroundLatest>(*pool);
  auto probe = std::make_shared<Probe>();
  auto entered = probe->entered.get_future();
  auto record = std::make_shared<UiRecord>();
  auto task = transformClient(fixture.owner->context(), source,
                              std::make_shared<const TransformInput>(TransformInput{-1, probe}), record);
  Release release{probe};

  REQUIRE(fixture.owner->start(std::move(task)));
  REQUIRE(entered.wait_for(5s) == std::future_status::ready);
  CHECK_FALSE(record->result);
  CHECK(probe->worker != std::this_thread::get_id());

  release.now();

  REQUIRE(eventually([&] {
    take(fixture.scheduler->dispatch());
    pool->service();
    take(source->service());

    return record->result.has_value();
  }));
  CHECK(record->resumed == std::this_thread::get_id());
  CHECK_FALSE(record->failure);
  CHECK_FALSE(*record->result);
  CHECK(record->result->error() == DomainError::REJECTED);
  CHECK(probe->runs == 1);
  REQUIRE(fixture.owner->close());
}

TEST_CASE("closing a UI frame does not join a worker held inside its transformation") {
  Fixture fixture;
  auto pool = take(BackgroundPool::create({1, 1}));
  auto source = std::make_shared<BackgroundLatest>(*pool);
  auto probe = std::make_shared<Probe>();
  auto entered = probe->entered.get_future();
  auto record = std::make_shared<UiRecord>();
  Release release{probe};

  REQUIRE(fixture.owner->start(transformClient(
    fixture.owner->context(), source, std::make_shared<const TransformInput>(TransformInput{1, probe}), record)));
  REQUIRE(entered.wait_for(5s) == std::future_status::ready);
  REQUIRE(fixture.owner->close());

  source->cancel();

  CHECK_FALSE(record->result);
  CHECK(record->resumed == std::thread::id{});
  CHECK(probe->runs == 1);
  CHECK_FALSE(probe->finished);
}

TEST_CASE("full UI queue retries exactly the same prepared transformation result") {
  SchedulerOptions options;

  options.data_capacity = 1;

  Fixture fixture(options);
  auto scope = take(fixture.scheduler->makeScope());
  auto occupied = take(fixture.scheduler->connect<int>(scope, [](auto) {
  }));

  REQUIRE(occupied.publisher.publish(std::make_shared<const int>(1)));

  auto pool = take(BackgroundPool::create({1, 1}));
  auto source = std::make_shared<BackgroundLatest>(*pool);
  auto probe = std::make_shared<Probe>();
  auto entered = probe->entered.get_future();
  auto record = std::make_shared<UiRecord>();
  Release release{probe};

  REQUIRE(fixture.owner->start(transformClient(
    fixture.owner->context(), source, std::make_shared<const TransformInput>(TransformInput{3, probe}), record)));
  REQUIRE(entered.wait_for(5s) == std::future_status::ready);

  release.now();

  REQUIRE(eventually([&] {
    return fixture.scheduler->metrics().data.rejected != 0;
  }));
  CHECK_FALSE(record->result);
  REQUIRE(eventually([&] {
    take(fixture.scheduler->dispatch());
    pool->service();

    return record->result.has_value();
  }));
  CHECK(probe->runs == 1);
  CHECK(record->published == probe->prepared.load());
  REQUIRE(*record->result);
  CHECK(**record->result == 6);
  REQUIRE(fixture.owner->close());
}

std::shared_ptr<const int> throwingTransform(const int &kind, std::stop_token) {
  if (kind == 1) {
    throw std::bad_alloc();
  }

  throw std::runtime_error("domain-independent callback failure");
}

UiTask failureClient(UiAsyncContext context, std::shared_ptr<BackgroundLatest> source, int kind,
                     std::shared_ptr<UiRecord> record) {
  auto result = co_await context.receive<int>([source, kind](AsyncReply<int> reply) {
    auto prepared = makeBackgroundJob(std::make_shared<const int>(kind), std::move(reply), throwingTransform);

    REQUIRE(prepared);
    REQUIRE(source->replace(std::move(*prepared)));
  });

  if (!result) {
    record->failure = result.error();

    co_return std::unexpected(result.error());
  }

  co_return std::expected<void, AsyncError>{};
}

TEST_CASE("default background transformation contains worker exceptions in typed UI outcomes") {
  int kind{};
  AsyncError expected{};

  SUBCASE("allocation failure") {
    kind = 1;
    expected = AsyncError::NO_MEMORY;
  }

  SUBCASE("other callback failure") {
    kind = 2;
    expected = AsyncError::CALLBACK_FAILED;
  }

  Fixture fixture;
  auto pool = take(BackgroundPool::create({1, 1}));
  auto source = std::make_shared<BackgroundLatest>(*pool);
  auto record = std::make_shared<UiRecord>();

  REQUIRE(fixture.owner->start(failureClient(fixture.owner->context(), source, kind, record)));
  REQUIRE(eventually([&] {
    take(fixture.scheduler->dispatch());
    pool->service();

    return record->failure.has_value();
  }));
  CHECK(record->failure == expected);
  CHECK(take(fixture.owner->outcome()).status == UiTaskStatus::FAILED);
  REQUIRE(fixture.owner->close());
}

TEST_CASE("closed scheduler and full completion queue never block pool shutdown") {
  SchedulerOptions options;

  options.data_capacity = 1;

  Fixture fixture(options);
  auto scope = take(fixture.scheduler->makeScope());
  auto occupied = take(fixture.scheduler->connect<int>(scope, [](auto) {
  }));

  REQUIRE(occupied.publisher.publish(std::make_shared<const int>(1)));

  auto pool = take(BackgroundPool::create({1, 1}));
  auto source = std::make_shared<BackgroundLatest>(*pool);
  auto probe = std::make_shared<Probe>();
  auto entered = probe->entered.get_future();
  auto record = std::make_shared<UiRecord>();
  auto task = transformClient(fixture.owner->context(), source,
                              std::make_shared<const TransformInput>(TransformInput{1, probe}), record);
  Release release{probe};

  REQUIRE(fixture.owner->start(std::move(task)));
  REQUIRE(entered.wait_for(5s) == std::future_status::ready);

  release.now();
  REQUIRE(fixture.scheduler->stop());
  pool->requestStop();
  pool->join();

  CHECK_FALSE(record->result);
  CHECK(probe->runs == 1);
  REQUIRE(fixture.owner->close());
}

class CountingReplyJob final : public BackgroundJob {
public:
  CountingReplyJob(AsyncReply<int> reply, std::shared_ptr<Probe> probe)
      : reply_(std::move(reply)), probe_(std::move(probe)), result_(std::make_shared<const int>(42)) {
  }

  void run(std::stop_token) noexcept override {
    ++probe_->runs;
    probe_->entered.set_value();
    probe_->release.wait();
  }

  std::expected<void, AsyncError> publish() override {
    ++probe_->publications;
    auto result = reply_.complete(result_);

    if (!result) {
      return std::unexpected(result.error());
    }

    return {};
  }

private:
  AsyncReply<int> reply_;
  std::shared_ptr<Probe> probe_;
  std::shared_ptr<const int> result_;
};

UiTask cancellationClient(UiAsyncContext context, std::shared_ptr<BackgroundLatest> source,
                          std::shared_ptr<Probe> probe) {
  auto result = co_await context.receive<int>([source, probe](AsyncReply<int> reply) {
    REQUIRE(source->replace(std::make_unique<CountingReplyJob>(std::move(reply), probe)));
  });

  if (!result) {
    co_return std::unexpected(result.error());
  }

  co_return std::expected<void, AsyncError>{};
}

TEST_CASE("cancelling borrowed jobs wakes full-queue publication waits without another UI service") {
  SchedulerOptions options;

  options.data_capacity = 1;

  Fixture fixture(options);
  auto scope = take(fixture.scheduler->makeScope());
  auto occupied = take(fixture.scheduler->connect<int>(scope, [](auto) {
  }));

  REQUIRE(occupied.publisher.publish(std::make_shared<const int>(1)));

  auto pool = take(BackgroundPool::create({2, 2}));
  auto secondOwner = take(UiAsync::create(*fixture.scheduler, *fixture.tree, take(fixture.tree->root())));
  auto firstSource = std::make_shared<BackgroundLatest>(*pool);
  auto secondSource = std::make_shared<BackgroundLatest>(*pool);
  auto first = std::make_shared<Probe>();
  auto second = std::make_shared<Probe>();
  auto firstEntered = first->entered.get_future();
  auto secondEntered = second->entered.get_future();
  Release releaseFirst{first}, releaseSecond{second};

  REQUIRE(fixture.owner->start(cancellationClient(fixture.owner->context(), firstSource, first)));
  REQUIRE(secondOwner->start(cancellationClient(secondOwner->context(), secondSource, second)));
  REQUIRE(firstEntered.wait_for(5s) == std::future_status::ready);
  REQUIRE(secondEntered.wait_for(5s) == std::future_status::ready);

  releaseFirst.now();
  releaseSecond.now();

  REQUIRE(eventually([&] {
    return first->publications != 0 && second->publications != 0;
  }));
  REQUIRE(fixture.owner->close());
  REQUIRE(secondOwner->close());

  firstSource->cancel();
  secondSource->cancel();

  auto foreign = std::make_shared<Probe>();
  auto foreignEntered = foreign->entered.get_future();
  Release releaseForeign{foreign};

  releaseForeign.now();

  auto pending = job(foreign);
  auto ticket = take(pool->submit(pending));

  // No service(), dispatch(), or pool stop is allowed to rescue this admission.
  REQUIRE(foreignEntered.wait_for(5s) == std::future_status::ready);
  REQUIRE(eventually([&] {
    return foreign->finished.load();
  }));
  CHECK(foreign->runs == 1);
}

class BackgroundScene final : public ApplicationScene {
public:
  std::shared_ptr<Probe> probe;
  std::future<void> entered;
  BackgroundTicket ticket;
  std::atomic<bool> stopped{};
  bool admitted{};
  bool failPrepare{};

  explicit BackgroundScene(std::shared_ptr<Probe> worker)
      : probe(std::move(worker)), entered(probe->entered.get_future()) {
  }

  bool requiresBackgroundExecutor() const noexcept override {
    return true;
  }

  std::expected<void, TreeError> prepare(Application &app, CellSize) override {
    if (!admitted) {
      auto pending = job(probe);

      ticket = take(app.background()->submit(pending));
      admitted = true;
      REQUIRE(entered.wait_for(5s) == std::future_status::ready);
    }

    if (failPrepare) {
      throw std::runtime_error("scene preparation");
    }

    return {};
  }

  void stop(Application &) override {
    ticket.cancel();
    stopped = true;
  }
};

class ReadyTerminal final : public ApplicationReadyTerminal {
public:
  bool closed{};
  std::size_t pending{};

  TerminalRead poll(std::stop_token) override {
    return {TerminalStatus::DEADLINE};
  }

  std::expected<CellSize, TerminalError> size() const override {
    return CellSize{10, 2};
  }

  std::expected<void, TerminalError> submit(std::string bytes) override {
    pending = bytes.size();

    return {};
  }

  TerminalWrite flushReady(std::stop_token) override {
    const auto accepted = std::exchange(pending, 0);

    return {TerminalStatus::PROGRESS, accepted, 0, {}};
  }

  std::expected<void, TerminalError> close() noexcept override {
    closed = true;

    return {};
  }
};

TEST_CASE("hosted background requires a borrowed executor and never joins or stops its foreign jobs") {
  SUBCASE("missing executor preserves the typed failure and closes the accepted transport") {
    auto app = take(Application::create());
    auto probe = std::make_shared<Probe>();
    BackgroundScene scene(probe);
    ReadyTerminal terminal;
    auto started = app->start(scene, terminal);

    REQUIRE_FALSE(started);
    CHECK(started.error().background == BackgroundError::UNAVAILABLE);
    CHECK(std::get<ApplicationFailure>(started.error().cause) == ApplicationFailure::BACKGROUND_FAILED);
    CHECK(terminal.closed);
    CHECK(scene.stopped);
    CHECK(probe->runs == 0);
  }

  SUBCASE("closing a hosted UI leaves the host pool usable") {
    auto pool = take(BackgroundPool::create({1, 2}));
    ApplicationOptions options;

    options.backgroundExecutor = pool.get();

    auto app = take(Application::create(options));
    auto probe = std::make_shared<Probe>();
    BackgroundScene scene(probe);
    ReadyTerminal terminal;
    Release release{probe};

    REQUIRE(app->start(scene, terminal));
    REQUIRE_FALSE(take(app->step()).finished);
    CHECK(probe->runs == 1);

    app->requestStop();

    CHECK(take(app->step()).finished);
    CHECK(terminal.closed);
    CHECK(scene.stopped);
    CHECK_FALSE(probe->finished);

    auto foreign = std::make_shared<Probe>();
    auto foreignEntered = foreign->entered.get_future();
    auto pending = job(foreign);
    auto ticket = take(pool->submit(pending));
    Release releaseForeign{foreign};

    release.now();

    REQUIRE(foreignEntered.wait_for(5s) == std::future_status::ready);
    CHECK(foreign->runs == 1);
  }
}

TEST_CASE("synchronous session closes terminal before its explicit final join even on restoration failure") {
  auto probe = std::make_shared<Probe>();
  BackgroundScene scene(probe);
  std::promise<void> closeObserved;
  auto closed = closeObserved.get_future();
  std::atomic<bool> returned{}, sawStopped{}, sawCloseBeforeReturn{};

  struct Terminal final : example::ReplayTerminal {
    std::promise<void> &closed;

    explicit Terminal(std::promise<void> &signal) : ReplayTerminal({}, 20, {10, 2}), closed(signal) {
    }

    std::expected<void, TerminalError> close() noexcept override {
      closed.set_value();

      return std::unexpected(TerminalError{TerminalErrorKind::SYSTEM, 17});
    }
  } terminal(closeObserved);

  auto app = take(Application::create({}, terminal.clock()));

  SUBCASE("EOF closes a successful session") {
  }

  SUBCASE("callback failure remains the first cause") {
    scene.failPrepare = true;
  }

  std::jthread releaseAfterClose([&] {
    if (closed.wait_for(5s) == std::future_status::ready) {
      sawStopped = scene.stopped.load();
      sawCloseBeforeReturn = !returned.load();
    }

    probe->release.count_down();
  });
  auto result = app->run(scene, terminal);

  returned = true;
  REQUIRE_FALSE(result);
  REQUIRE(result.error().restoration);
  CHECK(result.error().restoration->native_code == 17);

  if (scene.failPrepare) {
    CHECK(std::get<ApplicationFailure>(result.error().cause) == ApplicationFailure::CALLBACK_FAILED);
  } else {
    CHECK(std::get<TerminalError>(result.error().cause).native_code == 17);
  }

  CHECK(sawStopped);
  CHECK(sawCloseBeforeReturn);
  CHECK_FALSE(app->background());
  CHECK(probe->runs == 1);
}

TEST_CASE("native synchronous background startup failure is reported before terminal open") {
  ApplicationOptions options;

  options.background.workers = 0;

  auto app = take(Application::create(options));
  BackgroundScene scene(std::make_shared<Probe>());
  auto result = app->run(scene);

  REQUIRE_FALSE(result);
  CHECK(result.error().phase == ApplicationPhase::OPEN);
  CHECK(result.error().background == BackgroundError::INVALID_ARGUMENT);
  CHECK(std::get<ApplicationFailure>(result.error().cause) == ApplicationFailure::BACKGROUND_FAILED);
  CHECK_FALSE(scene.stopped);
}
} // namespace
