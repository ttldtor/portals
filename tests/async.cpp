// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/async.hpp>

#include <array>
#include <cstdlib>
#include <future>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace allocation_test {
thread_local bool fail{};
}

void *operator new(std::size_t size) {
  if (allocation_test::fail) {
    throw std::bad_alloc();
  }

  if (auto *value = std::malloc(size ? size : 1)) {
    return value;
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

namespace {
using namespace portals;

template <class T>
concept AsyncWaitAllowed = requires(UiTask::promise_type &promise, T &value) { promise.await_transform(value); };

struct ForeignTimer : AsyncTimer {};

static_assert(AsyncWaitAllowed<AsyncTimer>);
static_assert(AsyncWaitAllowed<AsyncReceive<int>>);
static_assert(!AsyncWaitAllowed<std::suspend_always>);
static_assert(!AsyncWaitAllowed<ForeignTimer>);

template <class T, class E> T take(std::expected<T, E> result) {
  REQUIRE(result);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

UiTask timerTask(UiAsyncContext context, int &calls) {
  auto waited = co_await context.until(10);

  if (!waited) {
    co_return std::unexpected(waited.error());
  }

  ++calls;

  co_return std::expected<void, AsyncError>{};
}

struct Fixture {
  SchedulerTime now{};
  std::unique_ptr<Scheduler> scheduler;
  std::unique_ptr<WidgetTree> tree;
  Node<Widget> node;
  std::unique_ptr<UiAsync> owner;

  explicit Fixture(SchedulerOptions options = {}) {
    auto clock = [](void *value) noexcept {
      return *static_cast<SchedulerTime *>(value);
    };
    scheduler = take(Scheduler::create(options, {clock, &now}));
    tree = take(WidgetTree::create(*scheduler));
    node = take(tree->root());
    owner = take(UiAsync::create(*scheduler, *tree, node));
  }
};

struct Record {
  std::vector<int> values;
  std::optional<AsyncError> error;
  std::thread::id resumeThread;
};

UiTask dataTask(UiAsyncContext context, std::function<void(AsyncReply<int>)> start, std::shared_ptr<Record> record,
                std::shared_ptr<void> lifetime = {}) {
  (void)lifetime;
  auto result = co_await context.receive<int>(std::move(start));
  record->resumeThread = std::this_thread::get_id();

  if (!result) {
    record->error = result.error();

    co_return std::unexpected(result.error());
  }

  record->values.push_back(**result);

  co_return std::expected<void, AsyncError>{};
}

UiTask immediateTask(std::function<void()> action) {
  action();

  co_return std::expected<void, AsyncError>{};
}

UiTask sequenceTask(UiAsyncContext context, std::shared_ptr<Record> record) {
  auto timer = co_await context.until(0);

  if (!timer) {
    co_return std::unexpected(timer.error());
  }

  auto first = co_await context.receive<int>([](AsyncReply<int> reply) {
    take(reply.complete(std::make_shared<const int>(3)));
  });

  if (!first) {
    co_return std::unexpected(first.error());
  }

  record->values.push_back(**first);
  auto second = co_await context.receive<int>([](AsyncReply<int> reply) {
    take(reply.complete(std::make_shared<const int>(4)));
  });

  if (!second) {
    co_return std::unexpected(second.error());
  }

  record->values.push_back(**second);

  co_return std::expected<void, AsyncError>{};
}

UiTask allocationTask(UiAsyncContext context) {
  allocation_test::fail = true;
  auto timer = co_await context.until(0);
  allocation_test::fail = false;

  co_return timer;
}
} // namespace

TEST_CASE("existing scheduler resumes coroutine timer only on UI dispatch") {
  SchedulerTime now{};
  auto clock = [](void *value) noexcept {
    return *static_cast<SchedulerTime *>(value);
  };
  auto scheduler = take(Scheduler::create({}, {clock, &now}));
  auto tree = take(WidgetTree::create(*scheduler));
  auto owner = take(UiAsync::create(*scheduler, *tree, take(tree->root())));
  int calls{};

  take(owner->start(timerTask(owner->context(), calls)));
  CHECK(calls == 0);
  now = 10;
  CHECK(calls == 0);
  take(scheduler->dispatch());
  CHECK(calls == 1);
  CHECK(take(owner->outcome()).status == UiTaskStatus::SUCCEEDED);
}

TEST_CASE("data starts on UI and synchronous completion queues without inline resume") {
  Fixture f;
  auto record = std::make_shared<Record>();
  record->resumeThread = std::this_thread::get_id();
  AsyncReply<int> retained;
  take(f.owner->start(dataTask(
    f.owner->context(),
    [&](AsyncReply<int> reply) {
      CHECK(std::this_thread::get_id() == record->resumeThread);
      retained = reply;
      take(reply.complete(std::make_shared<const int>(42)));
      CHECK(record->values.empty());
      CHECK(reply.complete(std::make_shared<const int>(99)).error() == AsyncError::WOULD_BLOCK);
    },
    record)));
  CHECK(record->values.empty());
  CHECK(take(f.owner->outcome()).status == UiTaskStatus::RUNNING);
  take(f.scheduler->dispatch());
  CHECK(record->values == std::vector<int>{42});
  CHECK(record->resumeThread == std::this_thread::get_id());
  CHECK(retained.complete(std::make_shared<const int>(99)).error() == AsyncError::CLOSED);
  CHECK(take(f.owner->outcome()).status == UiTaskStatus::SUCCEEDED);
}

TEST_CASE("worker endpoint never resumes or destroys the UI frame") {
  Fixture f;
  AsyncReply<int> reply;
  auto record = std::make_shared<Record>();
  std::thread::id destroyedOn;
  auto guard = std::shared_ptr<void>(new int, [&](void *p) {
    destroyedOn = std::this_thread::get_id();
    delete static_cast<int *>(p);
  });
  take(f.owner->start(dataTask(
    f.owner->context(),
    [&](AsyncReply<int> token) {
      reply = token;
    },
    record, std::move(guard))));
  std::expected<PublishResult, AsyncError> sent = std::unexpected(AsyncError::CLOSED);
  std::thread worker([&] {
    sent = reply.complete(std::make_shared<const int>(17));
  });
  worker.join();
  REQUIRE(sent);
  CHECK(record->values.empty());
  CHECK(destroyedOn == std::thread::id{});
  take(f.scheduler->dispatch());
  CHECK(record->values == std::vector<int>{17});
  CHECK(destroyedOn == std::this_thread::get_id());
}

TEST_CASE("queue rejection retains one reply and result for explicit retry") {
  SchedulerOptions options;
  options.data_capacity = 1;
  options.data_budget = 1;
  Fixture f(options);
  std::expected<SchedulerConnection<int>, SchedulerError> filler = std::unexpected(SchedulerError::CLOSED);
  take(f.tree->withScope(f.node, [&](const UiScope &scope) {
    filler = f.scheduler->connect<int>(scope, [](auto) {
    });
  }));
  REQUIRE(filler);
  take(filler->publisher.publish(std::make_shared<const int>(0)));
  AsyncReply<int> reply;
  auto value = std::make_shared<const int>(31);
  auto record = std::make_shared<Record>();
  take(f.owner->start(dataTask(
    f.owner->context(),
    [&](AsyncReply<int> token) {
      reply = token;
    },
    record)));
  CHECK(reply.complete(value).error() == AsyncError::WOULD_BLOCK);
  CHECK(take(f.owner->outcome()).status == UiTaskStatus::RUNNING);
  take(f.scheduler->dispatch());
  CHECK(record->values.empty());
  take(reply.complete(value));
  CHECK(reply.fail(AsyncError::CALLBACK_FAILED).error() == AsyncError::WOULD_BLOCK);
  take(f.scheduler->dispatch());
  CHECK(record->values == std::vector<int>{31});
  CHECK(f.scheduler->metrics().data.pending == 0);
}

TEST_CASE("cancel discards queued completion and releases frame on UI without provider join") {
  Fixture f;
  AsyncReply<int> reply;
  auto record = std::make_shared<Record>();
  std::thread::id destroyedOn;
  auto guard = std::shared_ptr<void>(new int, [&](void *p) {
    destroyedOn = std::this_thread::get_id();
    delete static_cast<int *>(p);
  });
  take(f.owner->start(dataTask(
    f.owner->context(),
    [&](AsyncReply<int> token) {
      reply = token;
    },
    record, std::move(guard))));
  take(reply.complete(std::make_shared<const int>(1)));
  take(f.owner->cancel());
  CHECK(destroyedOn == std::this_thread::get_id());
  auto result = take(f.owner->outcome());
  CHECK(result.status == UiTaskStatus::CANCELLED);
  CHECK(result.error == AsyncError::CANCELLED);
  CHECK(reply.complete(std::make_shared<const int>(2)).error() == AsyncError::CLOSED);
  take(f.scheduler->dispatch());
  CHECK(record->values.empty());
  CHECK_FALSE(record->error);
}

TEST_CASE("node removal cancels even when caller retains owner and worker endpoint") {
  Fixture f;
  AsyncReply<int> reply;
  auto record = std::make_shared<Record>();
  bool destroyed{};
  auto guard = std::shared_ptr<void>(new int, [&](void *p) {
    destroyed = true;
    delete static_cast<int *>(p);
  });
  take(f.owner->start(dataTask(
    f.owner->context(),
    [&](AsyncReply<int> token) {
      reply = token;
    },
    record, std::move(guard))));
  take(f.tree->remove(f.node));
  CHECK(destroyed);
  CHECK(take(f.owner->outcome()).error == AsyncError::CLOSED);
  CHECK(take(f.owner->outcome()).status == UiTaskStatus::CANCELLED);
  CHECK(reply.fail(AsyncError::NO_MEMORY).error() == AsyncError::CLOSED);
  auto lazy = immediateTask([] {
  });
  CHECK(f.owner->start(std::move(lazy)).error() == AsyncError::CLOSED);
  take(f.scheduler->dispatch());
  CHECK(record->values.empty());
}

TEST_CASE("scheduler shutdown and owner closure cancel pending timer or data") {
  Fixture f;
  int calls{};
  take(f.owner->start(timerTask(f.owner->context(), calls)));
  take(f.scheduler->stop());
  CHECK(take(f.owner->outcome()).error == AsyncError::CLOSED);
  CHECK(take(f.owner->outcome()).status == UiTaskStatus::CANCELLED);
  f.now = 100;
  take(f.scheduler->dispatch());
  CHECK(calls == 0);

  Fixture other;
  AsyncReply<int> reply;
  auto record = std::make_shared<Record>();
  take(other.owner->start(dataTask(
    other.owner->context(),
    [&](AsyncReply<int> token) {
      reply = token;
    },
    record)));
  take(other.owner->close());
  take(other.owner->close());
  CHECK(reply.complete(std::make_shared<const int>(8)).error() == AsyncError::CLOSED);
  CHECK(take(other.owner->outcome()).error == AsyncError::CLOSED);
}

TEST_CASE("repeated invocation policy preserves or replaces exactly one suspended frame") {
  Fixture f;
  AsyncReply<int> first;
  AsyncReply<int> second;
  auto record = std::make_shared<Record>();
  take(f.owner->start(dataTask(
    f.owner->context(),
    [&](AsyncReply<int> token) {
      first = token;
    },
    record)));
  auto generation = take(f.owner->outcome()).generation;
  auto task = dataTask(
    f.owner->context(),
    [&](AsyncReply<int> token) {
      second = token;
    },
    record);
  CHECK(f.owner->start(std::move(task)).error() == AsyncError::WOULD_BLOCK);
  CHECK(take(f.owner->outcome()).generation == generation);
  CHECK(second.complete(std::make_shared<const int>(1)).error() == AsyncError::CLOSED);
  take(f.owner->start(std::move(task), UiTaskRestart::CANCEL_PREVIOUS));
  CHECK(take(f.owner->outcome()).generation > generation);
  CHECK(first.complete(std::make_shared<const int>(1)).error() == AsyncError::CLOSED);
  take(second.complete(std::make_shared<const int>(2)));
  take(f.scheduler->dispatch());
  CHECK(record->values == std::vector<int>{2});
}

TEST_CASE("per-owner registration cost is one sentinel plus at most one wait") {
  SchedulerOptions options;
  options.subscription_capacity = 1;
  Fixture f(options);
  int calls{};
  take(f.owner->start(timerTask(f.owner->context(), calls)));
  auto result = take(f.owner->outcome());
  CHECK(result.status == UiTaskStatus::FAILED);
  CHECK(result.error == AsyncError::WOULD_BLOCK);
  CHECK(calls == 0);
  CHECK(UiAsync::create(*f.scheduler, *f.tree, f.node).error() == AsyncError::WOULD_BLOCK);
  take(f.owner->close());
  auto replacement = take(UiAsync::create(*f.scheduler, *f.tree, f.node));
  CHECK(take(replacement->outcome()).status == UiTaskStatus::IDLE);
}

TEST_CASE("due timer budget and sequential data waits use existing dispatch counts") {
  SchedulerOptions options;
  options.timer_budget = 1;
  options.data_budget = 1;
  Fixture f(options);
  auto second = take(UiAsync::create(*f.scheduler, *f.tree, f.node));
  int calls{};
  take(f.owner->start(timerTask(f.owner->context(), calls)));
  take(second->start(timerTask(second->context(), calls)));
  f.now = 20;
  CHECK(take(f.scheduler->dispatch()).timers == 1);
  CHECK(calls == 1);
  CHECK(take(f.scheduler->dispatch()).timers == 1);
  CHECK(calls == 2);

  auto record = std::make_shared<Record>();
  take(f.owner->start(sequenceTask(f.owner->context(), record)));
  CHECK(record->values.empty());
  auto dispatch = take(f.scheduler->dispatch());
  CHECK(dispatch.timers == 1);
  CHECK(dispatch.data == 1);
  CHECK(record->values == std::vector<int>{3});
  CHECK(take(f.scheduler->dispatch()).data == 1);
  CHECK(record->values == std::vector<int>{3, 4});
  CHECK(take(f.owner->outcome()).status == UiTaskStatus::SUCCEEDED);
}

TEST_CASE("provider failures and coroutine exceptions retain typed outcome") {
  Fixture f;
  auto record = std::make_shared<Record>();
  take(f.owner->start(dataTask(
    f.owner->context(),
    [](auto) {
      throw std::runtime_error("provider");
    },
    record)));
  CHECK(record->error == AsyncError::CALLBACK_FAILED);
  CHECK(take(f.owner->outcome()).error == AsyncError::CALLBACK_FAILED);
  CHECK(take(f.owner->outcome()).status == UiTaskStatus::FAILED);
  take(f.owner->start(dataTask(
    f.owner->context(),
    [](auto) {
      throw std::bad_alloc();
    },
    record)));
  CHECK(take(f.owner->outcome()).error == AsyncError::NO_MEMORY);
  take(f.owner->start(immediateTask([] {
    throw std::runtime_error("body");
  })));
  CHECK(take(f.owner->outcome()).error == AsyncError::CALLBACK_FAILED);
  take(f.owner->start(immediateTask([] {
    throw std::bad_alloc();
  })));
  CHECK(take(f.owner->outcome()).error == AsyncError::NO_MEMORY);
  take(f.owner->start(dataTask(
    f.owner->context(),
    [](AsyncReply<int> reply) {
      take(reply.fail(AsyncError::NO_MEMORY));
    },
    record)));
  CHECK(take(f.owner->outcome()).status == UiTaskStatus::RUNNING);
  take(f.scheduler->dispatch());
  CHECK(record->error == AsyncError::NO_MEMORY);
  CHECK(take(f.owner->outcome()).error == AsyncError::NO_MEMORY);
}

TEST_CASE("provider self-close and independent self-removal stop initial invocation") {
  SUBCASE("close") {
    Fixture f;
    auto record = std::make_shared<Record>();
    auto result = f.owner->start(dataTask(
      f.owner->context(),
      [&](auto) {
        take(f.owner->close());
      },
      record));
    CHECK(result.error() == AsyncError::CLOSED);
    CHECK(take(f.owner->outcome()).status == UiTaskStatus::CANCELLED);
    CHECK(record->error == AsyncError::CLOSED);
  }
  SUBCASE("remove") {
    Fixture f;
    auto record = std::make_shared<Record>();
    auto result = f.owner->start(dataTask(
      f.owner->context(),
      [&](auto) {
        take(f.tree->remove(f.node));
      },
      record));
    CHECK(result.error() == AsyncError::CLOSED);
    CHECK(take(f.owner->outcome()).status == UiTaskStatus::CANCELLED);
    CHECK(record->error == AsyncError::CLOSED);
  }
  SUBCASE("destroy owner inside provider") {
    Fixture f;
    auto record = std::make_shared<Record>();
    bool destroyed{};
    auto guard = std::shared_ptr<void>(new int, [&](void *p) {
      destroyed = true;
      delete static_cast<int *>(p);
    });
    auto result = f.owner->start(dataTask(
      f.owner->context(),
      [&](auto) {
        f.owner.reset();
        CHECK_FALSE(destroyed);
      },
      record, std::move(guard)));
    CHECK(result.error() == AsyncError::CLOSED);
    CHECK(destroyed);
    CHECK(record->error == AsyncError::CLOSED);
  }
}

TEST_CASE("resume self-cancel defers frame destruction and forbids a nested invocation") {
  Fixture f;
  bool destroyed{};
  auto guard = std::shared_ptr<void>(new int, [&](void *p) {
    destroyed = true;
    delete static_cast<int *>(p);
  });
  auto record = std::make_shared<Record>();
  auto result = f.owner->start(dataTask(
    f.owner->context(),
    [&](auto) {
      take(f.owner->cancel());
      CHECK_FALSE(destroyed);
      auto nested = immediateTask([] {
      });
      CHECK(f.owner->start(std::move(nested), UiTaskRestart::CANCEL_PREVIOUS).error() == AsyncError::WOULD_BLOCK);
    },
    record, std::move(guard)));
  CHECK(result.error() == AsyncError::CANCELLED);
  CHECK(destroyed);
  CHECK(record->error == AsyncError::CANCELLED);
  CHECK(take(f.owner->outcome()).status == UiTaskStatus::CANCELLED);
}

TEST_CASE("retiring frame parameters may reenter and preserve the newer invocation") {
  Fixture f;
  auto record = std::make_shared<Record>();
  AsyncReply<int> first;
  AsyncReply<int> nested;
  bool destructorRan{};
  auto guard = std::shared_ptr<void>(new int, [&](void *p) {
    delete static_cast<int *>(p);
    destructorRan = true;
    take(f.owner->start(dataTask(
      f.owner->context(),
      [&](auto token) {
        nested = token;
      },
      record)));
  });
  take(f.owner->start(dataTask(
    f.owner->context(),
    [&](auto token) {
      first = token;
    },
    record, std::move(guard))));
  auto outer = immediateTask([] {
  });
  CHECK(f.owner->start(std::move(outer), UiTaskRestart::CANCEL_PREVIOUS).error() == AsyncError::CANCELLED);
  CHECK(destructorRan);
  CHECK(first.complete(std::make_shared<const int>(1)).error() == AsyncError::CLOSED);
  take(nested.complete(std::make_shared<const int>(9)));
  take(f.scheduler->dispatch());
  CHECK(record->values == std::vector<int>{9});
  CHECK(take(f.owner->outcome()).status == UiTaskStatus::SUCCEEDED);
}

TEST_CASE("completion frame destructor may remove owner without writes to destroyed object") {
  Fixture f;
  auto record = std::make_shared<Record>();
  auto guard = std::shared_ptr<void>(new int, [&](void *p) {
    delete static_cast<int *>(p);
    f.owner.reset();
  });
  take(f.owner->start(dataTask(
    f.owner->context(),
    [](AsyncReply<int> reply) {
      take(reply.complete(std::make_shared<const int>(5)));
    },
    record, std::move(guard))));
  take(f.scheduler->dispatch());
  CHECK_FALSE(f.owner);
  CHECK(record->values == std::vector<int>{5});
}

TEST_CASE("rejected tasks remain lazy and frame/context/thread validation is typed") {
  Fixture f;
  int calls{};
  auto lazy = immediateTask([&] {
    ++calls;
  });
  CHECK(calls == 0);
  CHECK(f.owner->start(std::move(lazy), static_cast<UiTaskRestart>(99)).error() == AsyncError::INVALID_ARGUMENT);
  CHECK(calls == 0);
  take(f.owner->start(std::move(lazy)));
  CHECK(calls == 1);
  CHECK(f.owner->start(std::move(lazy)).error() == AsyncError::INVALID_ARGUMENT);
  auto record = std::make_shared<Record>();
  take(f.owner->start(dataTask(
    {},
    [](auto) {
    },
    record)));
  CHECK(take(f.owner->outcome()).error == AsyncError::CLOSED);
  Fixture other;
  take(f.owner->start(dataTask(
    other.owner->context(),
    [](auto) {
    },
    record)));
  CHECK(take(f.owner->outcome()).error == AsyncError::INVALID_ARGUMENT);
  CHECK(UiAsync::create(*other.scheduler, *f.tree, f.node).error() == AsyncError::INVALID_ARGUMENT);

  std::expected<void, AsyncError> cancelled;
  std::expected<void, AsyncError> closed;
  std::expected<UiTaskOutcome, AsyncError> outcome;
  std::thread foreign([&] {
    cancelled = f.owner->cancel();
    closed = f.owner->close();
    outcome = f.owner->outcome();
  });
  foreign.join();
  CHECK(cancelled.error() == AsyncError::WRONG_THREAD);
  CHECK(closed.error() == AsyncError::WRONG_THREAD);
  CHECK(outcome.error() == AsyncError::WRONG_THREAD);
}

TEST_CASE("foreign lazy frame is rejected without consuming or destroying it on UI") {
  Fixture f;
  std::promise<UiTask *> ready;
  auto exposed = ready.get_future();
  std::promise<void> release;
  auto released = release.get_future();
  bool called{};
  std::thread foreign([&] {
    auto task = immediateTask([&] {
      called = true;
    });
    ready.set_value(&task);
    released.wait();
  });
  auto *task = exposed.get();
  auto result = f.owner->start(std::move(*task));
  release.set_value();
  foreign.join();
  CHECK(result.error() == AsyncError::WRONG_THREAD);
  CHECK_FALSE(called);
  CHECK(take(f.owner->outcome()).status == UiTaskStatus::IDLE);
}

TEST_CASE("bounded randomized cancel replace and late completion preserve only current data") {
  Fixture f;
  std::mt19937 random(0xA51C);
  auto record = std::make_shared<Record>();
  std::vector<int> expected;

  for (int iteration = 0; iteration < 120; ++iteration) {
    AsyncReply<int> old;
    take(f.owner->start(dataTask(
      f.owner->context(),
      [&](auto token) {
        old = token;
      },
      record)));
    auto choice = random() % 4;

    if (choice == 0) {
      take(old.complete(std::make_shared<const int>(iteration)));
      expected.push_back(iteration);
    } else if (choice == 1) {
      take(old.complete(std::make_shared<const int>(-1)));
      take(f.owner->cancel());
    } else if (choice == 2) {
      AsyncReply<int> current;
      take(f.owner->start(dataTask(
                            f.owner->context(),
                            [&](auto token) {
                              current = token;
                            },
                            record),
                          UiTaskRestart::CANCEL_PREVIOUS));
      CHECK(old.complete(std::make_shared<const int>(-1)).error() == AsyncError::CLOSED);
      take(current.complete(std::make_shared<const int>(iteration)));
      expected.push_back(iteration);
    } else {
      take(old.fail(AsyncError::CALLBACK_FAILED));
    }

    take(f.scheduler->dispatch());
    CHECK(record->values == expected);
    CHECK(take(f.owner->outcome()).status != UiTaskStatus::RUNNING);
    CHECK(f.scheduler->metrics().data.pending == 0);
  }
}

TEST_CASE("actual allocation failure leaves completion admission retryable and wait failure typed") {
  Fixture f;
  allocation_test::fail = true;
  auto factory = UiAsync::create(*f.scheduler, *f.tree, f.node);
  allocation_test::fail = false;
  CHECK(factory.error() == AsyncError::NO_MEMORY);
  take(f.owner->start(allocationTask(f.owner->context())));
  CHECK(take(f.owner->outcome()).status == UiTaskStatus::FAILED);
  CHECK(take(f.owner->outcome()).error == AsyncError::NO_MEMORY);

  AsyncReply<int> reply;
  auto record = std::make_shared<Record>();
  auto value = std::make_shared<const int>(13);
  take(f.owner->start(dataTask(
    f.owner->context(),
    [&](auto token) {
      reply = token;
    },
    record)));
  allocation_test::fail = true;
  auto rejected = reply.complete(value);
  allocation_test::fail = false;
  CHECK(rejected.error() == AsyncError::NO_MEMORY);
  CHECK(take(f.owner->outcome()).status == UiTaskStatus::RUNNING);
  take(reply.complete(value));
  take(f.scheduler->dispatch());
  CHECK(record->values == std::vector<int>{13});
  allocation_test::fail = true;
  auto late = reply.complete(value);
  allocation_test::fail = false;
  CHECK(late.error() == AsyncError::CLOSED);
}

TEST_CASE("concurrent producers admit exactly one completion without retaining UI captures") {
  Fixture f;
  AsyncReply<int> reply;
  auto record = std::make_shared<Record>();
  take(f.owner->start(dataTask(
    f.owner->context(),
    [&](auto token) {
      reply = token;
    },
    record)));
  std::array<std::expected<PublishResult, AsyncError>, 2> results;
  std::atomic<int> ready{};
  std::atomic<bool> go{};
  auto publish = [&](std::size_t index) {
    auto value = std::make_shared<const int>(static_cast<int>(index + 1));
    ++ready;

    while (!go.load()) {
      std::this_thread::yield();
    }

    results[index] = reply.complete(std::move(value));
  };
  std::thread first(publish, 0);
  std::thread second(publish, 1);

  while (ready.load() != 2) {
    std::this_thread::yield();
  }

  go.store(true);
  first.join();
  second.join();
  CHECK(static_cast<int>(results[0].has_value()) + static_cast<int>(results[1].has_value()) == 1);
  auto &rejected = results[results[0] ? 1 : 0];
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error() == AsyncError::WOULD_BLOCK);
  CHECK(record->values.empty());
  CHECK(f.scheduler->metrics().data.pending == 1);
  take(f.scheduler->dispatch());
  REQUIRE(record->values.size() == 1);
  CHECK(record->values.front() == (results[0] ? 1 : 2));
}

TEST_CASE("rejected sole payload may reset or destroy its publishing endpoint") {
  for (bool destroyObject : {false, true}) {
    Fixture f;
    SchedulerConnection<int> filler;
    take(f.tree->withScope(f.node, [&](const UiScope &scope) {
      filler = take(f.scheduler->connect<int>(scope, [](auto) {
      }));
    }));
    auto record = std::make_shared<Record>();
    auto reply = std::make_unique<AsyncReply<int>>();
    take(f.owner->start(dataTask(
      f.owner->context(),
      [&](auto token) {
        *reply = token;
      },
      record)));
    auto spare = *reply;
    auto payload = std::shared_ptr<const int>(new int(23), [&](const int *value) {
      delete value;

      if (destroyObject) {
        reply.reset();
      } else {
        *reply = AsyncReply<int>{};
      }
    });
    // Fill all default data positions before the sole payload enters the rejected call.
    for (std::size_t index = 0; index < SchedulerOptions{}.data_capacity; ++index) {
      take(filler.publisher.publish(std::make_shared<const int>(0)));
    }

    auto rejected = reply->complete(std::move(payload));
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error() == AsyncError::WOULD_BLOCK);
    CHECK_FALSE(payload);
    CHECK(record->values.empty());

    while (f.scheduler->metrics().data.pending) {
      take(f.scheduler->dispatch());
    }

    take(spare.complete(std::make_shared<const int>(29)));
    take(f.scheduler->dispatch());
    CHECK(record->values == std::vector<int>{29});
  }
}

TEST_CASE("old frame parameter destructor may empty the incoming replacement task") {
  Fixture f;
  UiTask next;
  bool retired{};
  bool nextBodyRan{};
  auto record = std::make_shared<Record>();
  auto lifetime = std::shared_ptr<void>(new int, [&](void *value) {
    delete static_cast<int *>(value);
    retired = true;
    next = UiTask{};
  });
  take(f.owner->start(dataTask(
    f.owner->context(),
    [](auto) {
    },
    record, std::move(lifetime))));
  next = immediateTask([&] {
    nextBodyRan = true;
  });
  auto result = f.owner->start(std::move(next), UiTaskRestart::CANCEL_PREVIOUS);
  REQUIRE_FALSE(result);
  CHECK(result.error() == AsyncError::INVALID_ARGUMENT);
  CHECK(retired);
  CHECK_FALSE(nextBodyRan);
  CHECK(take(f.owner->outcome()).status == UiTaskStatus::CANCELLED);
  CHECK(f.scheduler->metrics().data.pending == 0);
}

TEST_CASE("old parameter destructor may replace incoming task with a foreign lazy frame") {
  Fixture f;
  std::promise<UiTask *> ready;
  auto exposed = ready.get_future();
  std::promise<void> release;
  auto released = release.get_future();
  std::thread::id foreignThread;
  std::thread::id destroyedOn;
  bool foreignBodyRan{};
  std::thread foreign([&] {
    foreignThread = std::this_thread::get_id();
    auto lifetime = std::shared_ptr<void>(new int, [&](void *value) {
      delete static_cast<int *>(value);
      destroyedOn = std::this_thread::get_id();
    });
    auto task = immediateTask([&, lifetime = std::move(lifetime)] {
      foreignBodyRan = true;
    });
    ready.set_value(&task);
    released.wait();
  });
  auto *foreignTask = exposed.get();
  UiTask next;
  auto record = std::make_shared<Record>();
  auto lifetime = std::shared_ptr<void>(new int, [&](void *value) {
    delete static_cast<int *>(value);
    next = std::move(*foreignTask);
  });
  take(f.owner->start(dataTask(
    f.owner->context(),
    [](auto) {
    },
    record, std::move(lifetime))));
  next = immediateTask([] {
  });
  auto result = f.owner->start(std::move(next), UiTaskRestart::CANCEL_PREVIOUS);

  // Return an unconsumed foreign frame to its creator before releasing the worker.
  *foreignTask = std::move(next);
  release.set_value();
  foreign.join();
  REQUIRE_FALSE(result);
  CHECK(result.error() == AsyncError::WRONG_THREAD);
  CHECK_FALSE(foreignBodyRan);
  CHECK(destroyedOn == foreignThread);
  CHECK(take(f.owner->outcome()).status == UiTaskStatus::CANCELLED);
}
