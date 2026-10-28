// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/background.hpp>
#include <portals/detail/worker_storage_wake.hpp>
#include <portals/worker_storage.hpp>

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

struct Record {
  std::promise<std::thread::id> destroyed;
  std::function<void()> onDestroy;
  std::function<void()> afterDestroyNotification;
};

struct Value final : WorkerStorageValue {
  explicit Value(std::shared_ptr<Record> record) : record(std::move(record)) {
  }

  ~Value() override {
    if (record->onDestroy) {
      record->onDestroy();
    }
    record->destroyed.set_value(std::this_thread::get_id());

    if (record->afterDestroyNotification) {
      record->afterDestroyNotification();
    }
  }

  std::shared_ptr<Record> record;
};

class Call final : public BackgroundJob {
public:
  explicit Call(std::function<void()> function, std::shared_ptr<std::promise<void>> finished)
      : function_(std::move(function)), finished_(std::move(finished)) {
  }

  void run(std::stop_token) noexcept override {
    try {
      function_();
      finished_->set_value();
    } catch (...) {
      finished_->set_exception(std::current_exception());
    }
  }

  std::expected<void, AsyncError> publish() override {
    return {};
  }

private:
  std::function<void()> function_;
  std::shared_ptr<std::promise<void>> finished_;
};

struct Running {
  BackgroundTicket ticket;
  std::future<void> finished;

  void wait() {
    REQUIRE(finished.wait_for(5s) == std::future_status::ready);
    finished.get();
  }
};

struct BlockedWorker {
  std::promise<void> entered;
  std::future<void> ready = entered.get_future();
  std::latch released{1};
  bool done{};

  void block() {
    entered.set_value();
    released.wait();
  }

  void wait() {
    REQUIRE(ready.wait_for(5s) == std::future_status::ready);
  }

  void release() {
    if (!done) {
      released.count_down();
      done = true;
    }
  }

  ~BlockedWorker() {
    release();
  }
};

Running call(BackgroundPool &pool, std::function<void()> action) {
  auto promise = std::make_shared<std::promise<void>>();
  auto finished = promise->get_future();
  std::unique_ptr<BackgroundJob> job = std::make_unique<Call>(std::move(action), promise);
  auto ticket = take(pool.submit(job));

  return {std::move(ticket), std::move(finished)};
}

void install(BackgroundWorkerStorage &storage, WorkerStorageId id, std::shared_ptr<Record> record,
             std::size_t resident = 32, std::size_t scratch = 32) {
  take(storage.withBuild(id, resident, scratch, [&](WorkerStorageBuild &builder) {
    std::unique_ptr<WorkerStorageValue> value = std::make_unique<Value>(record);

    return builder.commit(value, resident);
  }));
}

TEST_CASE("worker storage registration is light and rejects UI payload access") {
  auto pool = take(BackgroundPool::create({1, 1, 2, 128}));
  auto storage = pool->workerStorage();
  auto first = take(storage->registerSlot());
  auto second = take(storage->registerSlot());

  CHECK(storage->stats().chargedBytes == 0);
  CHECK(storage->stats().slots == 2);
  CHECK(storage->registerSlot().error() == BackgroundError::WOULD_BLOCK);
  CHECK(storage
          ->withRead(first.id(),
                     [](const auto &) -> std::expected<void, BackgroundError> {
                       return {};
                     })
          .error() == BackgroundError::WRONG_THREAD);
  CHECK(storage
          ->withBuild(second.id(), 32, 32,
                      [](auto &) -> std::expected<void, BackgroundError> {
                        return {};
                      })
          .error() == BackgroundError::WRONG_THREAD);
}

TEST_CASE("light wake closes retirement after a false predicate before wait registration") {
  detail::WorkerStorageWake wake({});
  const auto observed = wake.observe();
  BlockedWorker falsePredicate;
  std::promise<void> completed;
  auto done = completed.get_future();
  std::jthread waiter([&] {
    wake.wait(observed, [&] {
      const bool retired = wake.retired.load();
      if (!retired) {
        falsePredicate.block();
      }
      return retired;
    });
    completed.set_value();
  });
  falsePredicate.wait();
  // Retirement is forced after the false read and before CV wait registration.
  // notify must serialize through the light mutex; no service/submit/stop follows.
  wake.retired.store(true);
  std::jthread notifier([&] {
    wake.notify();
  });
  falsePredicate.release();
  CHECK(done.wait_for(5s) == std::future_status::ready);
}

TEST_CASE("idle worker reclaims lease retirement without a later submission or service") {
  auto pool = take(BackgroundPool::create({1, 1, 1, 128}));
  // On assertion failure this promise releases the owned future before pool teardown joins the worker.
  std::promise<void> finishDestruction;
  auto storage = pool->workerStorage();
  auto lease = take(storage->registerSlot());
  auto record = std::make_shared<Record>();
  auto destroyed = record->destroyed.get_future();

  record->afterDestroyNotification = [released = finishDestruction.get_future().share()] {
    released.wait();
  };

  auto installed = call(*pool, [&, id = lease.id()] {
    install(*storage, id, record);
  });
  installed.wait();
  lease.retire();
  REQUIRE(destroyed.wait_for(5s) == std::future_status::ready);
  CHECK(destroyed.get() != std::this_thread::get_id());
  // The notification precedes destructor completion and retirement accounting.
  CHECK(storage->stats().chargedBytes == 32);
  finishDestruction.set_value();

  const auto deadline = std::chrono::steady_clock::now() + 5s;

  while (storage->stats().chargedBytes != 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }

  CHECK(storage->stats().chargedBytes == 0);
}

TEST_CASE("retirement bypasses a full job queue and never destroys payload on UI") {
  auto pool = take(BackgroundPool::create({1, 1, 1, 128}));
  auto storage = pool->workerStorage();
  auto lease = take(storage->registerSlot());
  const auto id = lease.id();
  auto record = std::make_shared<Record>();
  auto destroyed = record->destroyed.get_future();
  auto installed = call(*pool, [=] {
    install(*storage, id, record);
  });
  installed.wait();
  BlockedWorker barrier;
  auto blocked = call(*pool, [&] {
    barrier.block();
  });
  barrier.wait();
  auto queued = call(*pool, [] {
  });
  lease.retire();

  CHECK(destroyed.wait_for(0ms) == std::future_status::timeout);
  barrier.release();
  blocked.wait();
  queued.wait();
  REQUIRE(destroyed.wait_for(5s) == std::future_status::ready);
  CHECK(destroyed.get() != std::this_thread::get_id());
  CHECK(storage->stats().chargedBytes == 0);
}

TEST_CASE("active reader fences replacement and retiring cleanup is outside the mutex") {
  auto pool = take(BackgroundPool::create({2, 2, 2, 128}));
  auto storage = pool->workerStorage();
  auto lease = take(storage->registerSlot());
  const auto id = lease.id();
  auto record = std::make_shared<Record>();
  auto destroyed = record->destroyed.get_future();
  record->onDestroy = [=] {
    (void)storage->stats();
  };
  auto installed = call(*pool, [=] {
    install(*storage, id, record);
  });
  installed.wait();
  BlockedWorker barrier;
  auto reader = call(*pool, [&] {
    take(storage->withRead(id, [&](const auto &) -> std::expected<void, BackgroundError> {
      barrier.block();
      return {};
    }));
  });
  barrier.wait();
  const auto epoch = storage->changeEpoch();
  auto replacement = call(*pool, [&] {
    auto result = storage->withBuild(id, 32, 32, [](auto &) -> std::expected<void, BackgroundError> {
      return {};
    });
    CHECK_FALSE(result);
    CHECK(result.error() == BackgroundError::WOULD_BLOCK);
  });
  replacement.wait();
  lease.retire();
  CHECK(destroyed.wait_for(0ms) == std::future_status::timeout);
  barrier.release();
  reader.wait();
  REQUIRE(destroyed.wait_for(5s) == std::future_status::ready);
  CHECK(destroyed.get() != std::this_thread::get_id());
  CHECK(storage->changeEpoch() != epoch);
}

TEST_CASE("aggregate build accounting retains scratch until callback destruction and evicts idle LRU") {
  auto pool = take(BackgroundPool::create({2, 2, 3, 128}));
  auto storage = pool->workerStorage();
  auto first = take(storage->registerSlot());
  auto second = take(storage->registerSlot());
  auto firstRecord = std::make_shared<Record>();
  auto destroyed = firstRecord->destroyed.get_future();
  auto installed = call(*pool, [&, id = first.id()] {
    install(*storage, id, firstRecord, 64, 32);
  });
  installed.wait();
  CHECK(storage->stats().chargedBytes == 64);
  auto secondRecord = std::make_shared<Record>();
  auto rebuilding = call(*pool, [&, id = second.id()] {
    take(storage->withBuild(id, 64, 64, [&](WorkerStorageBuild &builder) {
      CHECK(storage->stats().chargedBytes == 128);
      std::unique_ptr<WorkerStorageValue> value = std::make_unique<Value>(secondRecord);
      auto committed = builder.commit(value, 32);
      CHECK(storage->stats().chargedBytes == 128);
      return committed;
    }));
  });
  rebuilding.wait();
  REQUIRE(destroyed.wait_for(5s) == std::future_status::ready);
  CHECK(destroyed.get() != std::this_thread::get_id());
  CHECK(storage->stats().chargedBytes == 32);
  CHECK(storage->stats().peakBytes <= 128);
  CHECK(storage->stats().evictions == 1);
  auto impossible = call(*pool, [&, id = second.id()] {
    auto result = storage->withBuild(id, 129, 1, [](auto &) -> std::expected<void, BackgroundError> {
      return {};
    });
    CHECK(result.error() == BackgroundError::LIMIT_EXCEEDED);
  });
  impossible.wait();
}

TEST_CASE("active build reservations backpressure without waiting and release epochs enable retry") {
  auto pool = take(BackgroundPool::create({2, 2, 2, 128}));
  auto storage = pool->workerStorage();
  auto first = take(storage->registerSlot());
  auto second = take(storage->registerSlot());
  auto firstRecord = std::make_shared<Record>();
  auto secondRecord = std::make_shared<Record>();
  BlockedWorker barrier;
  auto building = call(*pool, [&] {
    take(storage->withBuild(first.id(), 64, 64, [&](WorkerStorageBuild &builder) {
      struct Scratch {
        BackgroundWorkerStorage &storage;
        ~Scratch() {
          CHECK(storage.stats().chargedBytes == 128);
        }
      } scratch{*storage};
      barrier.block();
      std::unique_ptr<WorkerStorageValue> value = std::make_unique<Value>(firstRecord);
      return builder.commit(value, 32);
    }));
  });
  barrier.wait();
  const auto epoch = storage->changeEpoch();
  auto backpressured = call(*pool, [&] {
    auto result = storage->withBuild(second.id(), 64, 64, [](auto &) -> std::expected<void, BackgroundError> {
      FAIL("another active reservation must prevent callback entry");
      return {};
    });
    CHECK(result.error() == BackgroundError::WOULD_BLOCK);
  });
  backpressured.wait();
  barrier.release();
  building.wait();
  CHECK(storage->changeEpoch() != epoch);
  auto retry = call(*pool, [&] {
    install(*storage, second.id(), secondRecord, 64, 64);
  });
  retry.wait();
  CHECK(storage->stats().chargedBytes == 64);
  CHECK(storage->stats().peakBytes == 128);
}

TEST_CASE("lease outlives host and concurrent final host destruction touches only light control") {
  for (unsigned iteration = 0; iteration < 50; ++iteration) {
    auto pool = take(BackgroundPool::create({1, 1, 1, 128}));
    auto lease = take(pool->workerStorage()->registerSlot());
    const auto stale = lease.id();
    auto record = std::make_shared<Record>();
    auto destroyed = record->destroyed.get_future();
    auto installed = call(*pool, [&, storage = pool->workerStorage()] {
      install(*storage, stale, record);
    });
    installed.wait();
    pool->requestStop();
    pool->join();
    // This host boundary represents the documented post-UI/post-terminal cleanup.
    std::jthread retire([&] {
      lease.retire();
    });
    pool.reset();
    retire.join();
    REQUIRE(destroyed.wait_for(5s) == std::future_status::ready);
    CHECK(destroyed.get() == std::this_thread::get_id());
    lease.retire();
  }
}

TEST_CASE("failed callbacks and retired or closed active builds release their committed value on worker") {
  for (unsigned outcome = 0; outcome < 3; ++outcome) {
    auto pool = take(BackgroundPool::create({1, 1, 1, 128}));
    auto storage = pool->workerStorage();
    auto lease = take(storage->registerSlot());
    auto record = std::make_shared<Record>();
    auto destroyed = record->destroyed.get_future();
    BlockedWorker barrier;
    auto running = call(*pool, [&, id = lease.id()] {
      auto result =
        storage->withBuild(id, 64, 64, [&](WorkerStorageBuild &builder) -> std::expected<void, BackgroundError> {
          std::unique_ptr<WorkerStorageValue> value = std::make_unique<Value>(record);
          take(builder.commit(value, 32));
          barrier.block();
          if (!outcome) {
            throw std::runtime_error("post-commit callback failure");
          }
          return {};
        });
      REQUIRE_FALSE(result);
      CHECK(result.error() == (outcome ? BackgroundError::CLOSED : BackgroundError::CALLBACK_FAILED));
    });
    barrier.wait();
    if (outcome == 1) {
      lease.retire();
    }
    if (outcome == 2) {
      pool->requestStop();
    }
    CHECK(storage->stats().chargedBytes == 128);
    barrier.release();
    running.wait();
    REQUIRE(destroyed.wait_for(5s) == std::future_status::ready);
    CHECK(destroyed.get() != std::this_thread::get_id());
    CHECK(storage->stats().chargedBytes == 0);
  }
}

TEST_CASE("a retired generation cannot retire a later registration in the same slot") {
  auto pool = take(BackgroundPool::create({1, 1, 1, 128}));
  auto storage = pool->workerStorage();
  auto first = take(storage->registerSlot());
  const auto stale = first.id();
  first.retire();
  std::optional<WorkerStorageLease> second;
  const auto deadline = std::chrono::steady_clock::now() + 5s;

  while (!second && std::chrono::steady_clock::now() < deadline) {
    if (auto next = storage->registerSlot()) {
      second.emplace(std::move(*next));
    } else {
      std::this_thread::yield();
    }
  }
  REQUIRE(second);
  CHECK(second->id().value == stale.value);
  CHECK(second->id().generation != stale.generation);
  auto probe = call(*pool, [=] {
    CHECK(storage
            ->withRead(stale,
                       [](const auto &) -> std::expected<void, BackgroundError> {
                         return {};
                       })
            .error() == BackgroundError::CLOSED);
  });
  probe.wait();
}
} // namespace
