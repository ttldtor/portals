// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include <portals/window_source_model.hpp>

#include <chrono>
#include <cstdlib>
#include <future>
#include <memory_resource>
#include <string>
#include <thread>

namespace source_allocation_test {
thread_local bool failNext{};
}

void *operator new(std::size_t bytes) {
  if (std::exchange(source_allocation_test::failNext, false)) {
    throw std::bad_alloc();
  }

  if (auto value = std::malloc(bytes ? bytes : 1)) {
    return value;
  }

  throw std::bad_alloc();
}

void *operator new[](std::size_t bytes) {
  return ::operator new(bytes);
}

void operator delete(void *value) noexcept {
  std::free(value);
}

void operator delete(void *value, std::size_t) noexcept {
  std::free(value);
}

void operator delete[](void *value) noexcept {
  std::free(value);
}

void operator delete[](void *value, std::size_t) noexcept {
  std::free(value);
}

namespace {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> value) {
  REQUIRE(value.has_value());

  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

struct Generated final : WindowProvider<int> {
  std::size_t batch{1024};
  SourceEdge edge{SourceEdge::MORE};
  SourceReadState state{SourceReadState::READY};
  bool loop{};
  bool cycle{};
  bool wrongPending{};
  bool duplicate{};
  bool throwLoad{};
  SourceCapabilities caps;
  std::atomic<int> calls{};

  SourceCapabilities capabilities() const noexcept override {
    return caps;
  }

  std::expected<SourceReadReply<int>, SourceError> load(const SourceReadCommand &command, PageBuilder<int> &out,
                                                        std::stop_token stop) override {
    ++calls;

    if (throwLoad) {
      throw std::runtime_error("provider fault");
    }

    if (state != SourceReadState::READY) {
      SourceReadReply<int> result;
      result.state = state;
      result.observedRevision = command.dataRevision + (wrongPending ? 1 : 0);

      return result;
    }

    auto position = command.anchor.kind == SourceAnchorKind::CURSOR ? command.anchor.cursor.token[0] : 0;
    auto count = std::min(command.maxRows, batch);

    for (std::size_t i = 0; i < count; ++i) {
      if (stop.stop_requested()) {
        return std::unexpected(SourceError::CANCELLED);
      }

      auto result = out.emplace({duplicate ? 1 : position + i + 1, 1}, 1, static_cast<int>(position + i));

      if (!result) {
        return std::unexpected(result.error());
      }
    }

    auto next = loop ? position : (cycle && position == 1 ? 0 : position + std::max<std::size_t>(1, count));

    return out.ready({command.source, {position, 0}}, {command.source, {next, 0}}, edge);
  }
};

struct Fixture {
  std::unique_ptr<Scheduler> scheduler = take(Scheduler::create());
  std::unique_ptr<WidgetTree> tree = take(WidgetTree::create(*scheduler));
  Node<Widget> root = take(tree->root());
  std::shared_ptr<Generated> provider = std::make_shared<Generated>();
  std::shared_ptr<SourceBudget> budget;
  std::unique_ptr<WindowSource<int>> source;
  WindowConsumer<int> consumer;

  explicit Fixture(SourceBudgetOptions limits = {}, WindowSourceOptions options = {}) {
    budget = take(SourceBudget::create(limits));
    source = take(WindowSource<int>::create(*tree, root, provider, options, budget));
    consumer = take(source->openConsumer());
  }

  void dispatch() {
    take(scheduler->dispatch());
  }

  void deliver(WindowRequest<int> request) {
    auto result = std::async(std::launch::async, [request] {
                    return request.load();
                  }).get();
    take(request.complete(result));
    dispatch();
  }

  WindowRequest<int> next() {
    auto result = take(consumer.takeRequest());
    REQUIRE(result.has_value());

    return std::move(*result);
  }
};
} // namespace

TEST_CASE("window source is opt-in, at-most-N and queue admission precedes apply") {
  static_assert(std::is_nothrow_copy_constructible_v<WindowPage<int>>);
  static_assert(std::is_nothrow_copy_constructible_v<WindowPin<int>>);
  static_assert(std::is_nothrow_copy_constructible_v<WindowRequest<int>>);
  static_assert(std::is_nothrow_copy_constructible_v<SourceReadReply<int>>);
  static_assert(sizeof(SourceCursor) == 32);
  Fixture f;
  auto request = take(f.consumer.request({}, SourceDirection::FORWARD, 7));
  CHECK(request.command().maxRows == 7);
  CHECK(request.load().error() == SourceError::WRONG_THREAD);
  auto reply = std::async(std::launch::async, [request] {
                 return request.load();
               }).get();
  REQUIRE(reply);
  CHECK(reply->page.rows().size() == 7);
  take(request.complete(reply));
  CHECK(take(f.consumer.pin()).size() == 0);
  CHECK_FALSE(request.complete(reply));
  f.dispatch();
  CHECK(take(f.consumer.pin()).size() == 7);
  CHECK(take(f.consumer.status()) == WindowStatus::READY);
  CHECK_FALSE(take(f.consumer.takeRequest()));
  f.dispatch();
  CHECK(f.provider->calls == 1);
  CHECK_FALSE(take(f.consumer.takeRequest()));
}

TEST_CASE("four short pages fill exactly; remaining demand caps commands") {
  Fixture f;
  f.provider->batch = 2;
  f.deliver(take(f.consumer.request({}, SourceDirection::FORWARD, 8)));

  for (std::size_t remaining : {6U, 4U, 2U}) {
    auto request = f.next();
    CHECK(request.command().maxRows == remaining);
    f.deliver(std::move(request));
  }

  CHECK(take(f.consumer.status()) == WindowStatus::READY);
  CHECK_FALSE(take(f.consumer.lastError()));
  CHECK(take(f.consumer.pin()).size() == 8);
  CHECK(f.provider->calls == 4);
  CHECK_FALSE(take(f.consumer.takeRequest()));
}

TEST_CASE("zero MORE is not EOF but its fill stops without per-frame spin") {
  Fixture f;
  f.provider->batch = 0;
  f.deliver(take(f.consumer.request({}, SourceDirection::FORWARD, 1)));

  for (int i = 0; i < 3; ++i) {
    f.deliver(f.next());
  }

  CHECK(take(f.consumer.lastError()) == SourceError::CONTINUATION_LIMIT);
  CHECK(take(f.consumer.pin()).edge() == SourceEdge::MORE);
  f.dispatch();
  CHECK_FALSE(take(f.consumer.takeRequest()));
  CHECK(f.provider->calls == 4);
}

TEST_CASE("self-loop and two cursor cycle are explicit errors") {
  SUBCASE("A to A") {
    Fixture f;
    f.provider->loop = true;
    f.deliver(take(f.consumer.request({}, SourceDirection::FORWARD, 8)));
    CHECK(take(f.consumer.lastError()) == SourceError::NO_PROGRESS);
  }
  SUBCASE("A to B to A") {
    Fixture f;
    f.provider->batch = 0;
    f.provider->cycle = true;
    f.deliver(take(f.consumer.request({}, SourceDirection::FORWARD, 8)));
    f.deliver(f.next());
    CHECK(take(f.consumer.lastError()) == SourceError::CURSOR_CYCLE);
  }
}

TEST_CASE("pending, temporary tail, final end and unavailable history are distinct") {
  Fixture f;
  f.provider->batch = 0;
  f.provider->state = SourceReadState::PENDING;
  f.deliver(take(f.consumer.request({}, SourceDirection::FORWARD, 2)));
  CHECK(take(f.consumer.status()) == WindowStatus::PENDING);
  CHECK_FALSE(take(f.consumer.takeRequest()));
  f.provider->state = SourceReadState::HISTORY_UNAVAILABLE;
  f.deliver(take(f.consumer.request({}, SourceDirection::FORWARD, 2)));
  CHECK(take(f.consumer.status()) == WindowStatus::HISTORY_UNAVAILABLE);
  f.provider->state = SourceReadState::READY;
  f.provider->edge = SourceEdge::TEMPORARY_TAIL;
  f.deliver(take(f.consumer.request({}, SourceDirection::FORWARD, 2)));
  CHECK(take(f.consumer.status()) == WindowStatus::TEMPORARY_TAIL);
  f.provider->edge = SourceEdge::FINAL_END;
  f.deliver(take(f.consumer.request({}, SourceDirection::FORWARD, 2)));
  CHECK(take(f.consumer.status()) == WindowStatus::FINAL_END);
}

TEST_CASE("new readiness during load is reconciled after stale completion without a second wake") {
  Fixture f;
  f.deliver(take(f.consumer.request({}, SourceDirection::FORWARD, 3)));
  auto previous = take(f.consumer.pin());
  SourceAnchor intent;
  intent.kind = SourceAnchorKind::CURSOR;
  intent.cursor = previous.continuation();
  auto old = take(f.consumer.request(intent, SourceDirection::FORWARD, 4));
  auto result = std::async(std::launch::async, [old] {
                  return old.load();
                }).get();
  take(f.source->notifyReady(f.source->stamp(), 2));
  take(f.source->notifyReady(f.source->stamp(), 3));
  CHECK(f.source->notifyReady(f.source->stamp(), 2).error() == SourceError::REVISION_CHANGED);
  f.dispatch();
  CHECK_FALSE(take(f.consumer.takeRequest()));
  take(old.complete(result));
  f.dispatch();
  CHECK(take(f.consumer.pin()).size() == 3);
  CHECK(take(f.consumer.pin()).dataRevision() == 1);
  auto latest = f.next();
  CHECK(latest.command().dataRevision == 3);
  CHECK(latest.command().anchor.cursor == intent.cursor);
  CHECK(latest.command().maxRows == 4);
  old = {};
  result = std::unexpected(SourceError::CANCELLED);
  f.deliver(std::move(latest));
  CHECK(take(f.consumer.pin()).dataRevision() == 3);
  f.dispatch();
  CHECK_FALSE(take(f.consumer.takeRequest()));
}

TEST_CASE("notification queued after pending reply cannot lose the retry") {
  Fixture f;
  f.provider->state = SourceReadState::PENDING;
  auto request = take(f.consumer.request({}, SourceDirection::FORWARD, 2));
  auto result = std::async(std::launch::async, [request] {
                  return request.load();
                }).get();
  take(request.complete(result));
  take(f.source->notifyReady(f.source->stamp(), 2));
  f.dispatch();
  auto retry = f.next();
  CHECK(retry.command().dataRevision == 2);
  CHECK_FALSE(take(f.consumer.takeRequest()));
}

TEST_CASE("capabilities and immutable session reject unsupported or foreign navigation") {
  Fixture f;
  CHECK(f.consumer.request({}, SourceDirection::BACKWARD, 1).error() == SourceError::UNSUPPORTED);
  SourceAnchor tail;
  tail.kind = SourceAnchorKind::TAIL;
  CHECK(f.consumer.request(tail, SourceDirection::FORWARD, 1).error() == SourceError::UNSUPPORTED);
  tail.kind = SourceAnchorKind::ROW;
  tail.row = {1, 1};
  CHECK(f.consumer.request(tail, SourceDirection::FORWARD, 1).error() == SourceError::UNSUPPORTED);
  auto wrong = f.source->stamp();
  ++wrong.epoch;
  CHECK(f.source->notifyReady(wrong, 2).error() == SourceError::STALE_SOURCE);
  SourceAnchor foreign;
  foreign.kind = SourceAnchorKind::CURSOR;
  foreign.cursor.source = wrong;
  CHECK(f.consumer.request(foreign, SourceDirection::FORWARD, 1).error() == SourceError::STALE_SOURCE);
  take(f.tree->frame([&] {
    CHECK(f.consumer.request({}, SourceDirection::FORWARD, 1).error() == SourceError::WOULD_BLOCK);
  }));
}

TEST_CASE("old pins exert backpressure and final storage release restores page credit") {
  Fixture f({8 * 1024 * 1024, 2, 4, 8});
  f.deliver(take(f.consumer.request({}, SourceDirection::FORWARD, 2)));
  auto old = take(f.consumer.pin());
  f.deliver(take(f.consumer.request({}, SourceDirection::FORWARD, 2)));
  CHECK(f.budget->usage().pages == 2);
  auto rejected = f.consumer.request({}, SourceDirection::FORWARD, 2);
  CHECK(rejected.error() == SourceError::WOULD_BLOCK);
  CHECK(old.page(0).rows()[0].value == 0);
  CHECK(take(f.consumer.pin()).size() == 2);
  old = {};
  CHECK(f.budget->usage().pages == 1);
  auto admitted = take(f.consumer.request({}, SourceDirection::FORWARD, 2));
  CHECK(f.budget->usage().pages == 2);
  admitted.cancel();
  CHECK(f.budget->usage().pages == 2);
  CHECK(f.budget->usage().work == 1);
  admitted = {};
  CHECK(f.budget->usage().pages == 1);
  CHECK(f.budget->usage().work == 0);
}

TEST_CASE("queued and cancelled owners retain work reservation until final release") {
  Fixture f;
  auto request = take(f.consumer.request({}, SourceDirection::FORWARD, 2));
  auto reply = std::async(std::launch::async, [request] {
                 return request.load();
               }).get();
  take(request.complete(reply));
  request.cancel();
  CHECK(f.budget->usage().work == 1);
  f.dispatch();
  CHECK(take(f.consumer.pin()).size() == 0);
  CHECK(take(f.consumer.lastError()) == SourceError::CANCELLED);
  request = {};
  CHECK(f.budget->usage().work == 0);
  CHECK(f.budget->usage().pages == 1);
  reply = std::unexpected(SourceError::CANCELLED);
  CHECK(f.budget->usage().pages == 0);
}

TEST_CASE("tree destruction cannot cause raw-tree access through surviving source or consumer") {
  Fixture f;
  f.deliver(take(f.consumer.request({}, SourceDirection::FORWARD, 2)));
  auto pin = take(f.consumer.pin());
  auto work = take(f.consumer.request({}, SourceDirection::FORWARD, 2));
  auto reply = std::async(std::launch::async, [work] {
                 return work.load();
               }).get();
  take(work.complete(reply));
  f.tree.reset();
  CHECK(f.source->openConsumer().error() == SourceError::CLOSED);
  CHECK(f.consumer.request({}, SourceDirection::FORWARD, 1).error() == SourceError::CLOSED);
  CHECK_FALSE(work.complete(reply));
  CHECK(pin.page(0).rows()[0].value == 0);
  f.source.reset();
  CHECK(pin.page(0).rows()[1].value == 1);
  f.dispatch();
}

TEST_CASE("node removal during worker load rejects late completion and preserves pinned credit") {
  struct GatedProvider final : WindowProvider<int> {
    Generated generated;
    std::promise<void> entered;
    std::shared_future<void> release;
    bool gated{};

    explicit GatedProvider(std::shared_future<void> gate) : release(std::move(gate)) {
    }

    SourceCapabilities capabilities() const noexcept override {
      return {};
    }

    std::expected<SourceReadReply<int>, SourceError> load(const SourceReadCommand &command, PageBuilder<int> &out,
                                                          std::stop_token stop) override {
      auto reply = generated.load(command, out, stop);

      if (gated) {
        // Retain the prepared owning page and workspace until UI removes the context node.
        entered.set_value();
        release.wait();
      }

      return reply;
    }
  };

  struct GateOpener {
    std::promise<void> &signal;
    bool armed{true};

    void open() {
      if (std::exchange(armed, false)) {
        signal.set_value();
      }
    }

    ~GateOpener() {
      open();
    }
  };

  auto scheduler = take(Scheduler::create());
  auto tree = take(WidgetTree::create(*scheduler));
  auto root = take(tree->root());
  auto context = take(tree->emplace<Widget>(root));
  auto budget = take(SourceBudget::create());
  auto empty = budget->usage();
  std::promise<void> release;
  auto provider = std::make_shared<GatedProvider>(release.get_future().share());
  auto source = take(WindowSource<int>::create(*tree, context, provider, {}, budget));
  auto consumer = take(source->openConsumer());

  {
    auto first = take(consumer.request({}, SourceDirection::FORWARD, 2));
    auto reply = std::async(std::launch::async, [first] {
                   return first.load();
                 }).get();
    take(first.complete(reply));
    take(scheduler->dispatch());
  }

  auto pin = take(consumer.pin());
  auto retained = budget->usage();
  auto work = take(consumer.request({}, SourceDirection::FORWARD, 2));
  auto entered = provider->entered.get_future();
  provider->gated = true;
  auto pending = std::async(std::launch::async, [work] {
    return work.load();
  });
  // Unblock before future destruction even if an assertion below unwinds the test.
  GateOpener gate{release};
  constexpr auto WORKER_ENTRY_TIMEOUT = std::chrono::seconds(5);
  REQUIRE(entered.wait_for(WORKER_ENTRY_TIMEOUT) == std::future_status::ready);
  entered.get();
  auto blocked = budget->usage();
  CHECK(blocked.pages == retained.pages + 1);
  CHECK(blocked.work == 1);

  take(tree->remove(context));
  CHECK(tree->inspect(root).has_value());
  CHECK_FALSE(tree->inspect(context));
  auto closedConsumer = source->openConsumer();
  REQUIRE_FALSE(closedConsumer);
  CHECK(closedConsumer.error() == SourceError::CLOSED);
  auto closedRequest = consumer.request({}, SourceDirection::FORWARD, 1);
  REQUIRE_FALSE(closedRequest);
  CHECK(closedRequest.error() == SourceError::CLOSED);
  CHECK(work.stopRequested());
  CHECK(budget->usage().pages == blocked.pages);
  CHECK(budget->usage().work == blocked.work);
  CHECK(budget->usage().bytes == blocked.bytes);
  CHECK(take(consumer.pin()).size() == 2);
  CHECK(pin.page(0).rows()[1].value == 1);

  gate.open();
  auto reply = pending.get();
  REQUIRE_FALSE(reply);
  CHECK(reply.error() == SourceError::CANCELLED);
  auto completion = work.complete(reply);
  REQUIRE_FALSE(completion);
  CHECK(completion.error() == SchedulerError::CLOSED);
  take(scheduler->dispatch());
  CHECK(take(consumer.pin()).size() == 2);
  CHECK(budget->usage().pages == retained.pages);
  CHECK(budget->usage().work == 1);
  work = {};
  CHECK(budget->usage().work == 0);
  CHECK(budget->usage().bytes == retained.bytes);

  consumer.close();
  source.reset();
  CHECK(pin.page(0).rows()[0].value == 0);
  CHECK(budget->usage().pages == 1);
  pin = {};
  CHECK(budget->usage().pages == 0);
  CHECK(budget->usage().bytes == empty.bytes);
  CHECK(budget->usage().consumers == 0);
}

TEST_CASE("protocol and provider errors preserve the previously committed pin") {
  Fixture f;
  f.deliver(take(f.consumer.request({}, SourceDirection::FORWARD, 2)));
  f.provider->duplicate = true;
  f.deliver(take(f.consumer.request({}, SourceDirection::FORWARD, 2)));
  CHECK(take(f.consumer.lastError()) == SourceError::INVALID_REPLY);
  CHECK(take(f.consumer.pin()).size() == 2);
  f.provider->duplicate = false;
  f.provider->state = SourceReadState::PENDING;
  f.provider->wrongPending = true;
  f.deliver(take(f.consumer.request({}, SourceDirection::FORWARD, 2)));
  CHECK(take(f.consumer.lastError()) == SourceError::INVALID_REPLY);
  f.provider->state = SourceReadState::READY;
  f.provider->throwLoad = true;
  f.deliver(take(f.consumer.request({}, SourceDirection::FORWARD, 2)));
  CHECK(take(f.consumer.lastError()) == SourceError::CALLBACK_FAILED);
  CHECK(take(f.consumer.pin()).size() == 2);
}

TEST_CASE("shared root spans replacement sessions without revoking old pins") {
  Fixture f;
  f.deliver(take(f.consumer.request({}, SourceDirection::FORWARD, 2)));
  auto pin = take(f.consumer.pin());
  auto stamp = f.source->stamp();
  f.source->close();
  auto replacement = take(WindowSource<int>::create(*f.tree, f.root, f.provider, {}, f.budget));
  CHECK(replacement->stamp() != stamp);
  CHECK(replacement->budget() == f.budget);
  CHECK(pin.page(0).rows()[0].value == 0);
  CHECK(f.budget->usage().pages == 1);
}

TEST_CASE("finite adapter preserves Model count and exact reads") {
  struct Finite final : ModelSnapshot<int> {
    std::array<ModelRow<int>, 3> rows{{{{1, 1}, 10}, {{2, 1}, 20}, {{3, 1}, 30}}};

    Finite() : ModelSnapshot<int>(7, 3) {
    }

    ModelRows<int> read(RowRange range) const override {
      return {RangeState::READY, std::span(rows).subspan(static_cast<std::size_t>(range.first), range.count), {}};
    }
  };

  auto scheduler = take(Scheduler::create());
  auto tree = take(WidgetTree::create(*scheduler));
  auto snapshot = std::make_shared<Finite>();
  auto provider = std::make_shared<FiniteWindowProvider<int>>(snapshot);
  auto source = take(WindowSource<int>::create(*tree, take(tree->root()), provider, {.initialRevision = 7}));
  auto consumer = take(source->openConsumer());
  auto request = take(consumer.request({}, SourceDirection::FORWARD, 10));
  auto reply = std::async(std::launch::async, [request] {
                 return request.load();
               }).get();
  REQUIRE(reply);
  CHECK(reply->page.rows().size() == 3);
  CHECK(reply->edge == SourceEdge::FINAL_END);
  take(request.complete(reply));
  take(scheduler->dispatch());
  CHECK(snapshot->rowCount() == 3);
  CHECK(snapshot->revision() == 7);
  CHECK(take(consumer.pin()).page(0).rows()[2].value == 30);
}

TEST_CASE("budget split and arena allocations share one root until final release") {
  auto root = take(SourceBudget::create({64 * 1024, 1, 1, 1}));
  auto initial = root->usage();
  auto credit = take(root->reserve({32 * 1024, 1, 1, 0}));
  auto page = take(credit.split({16 * 1024, 1, 0, 0}));
  auto arena = take(SourceArena::create(std::move(page)));
  auto alias = arena;
  CHECK(root->usage().pages == 1);
  CHECK_THROWS_AS((void)arena->allocate(32 * 1024), std::bad_alloc);
  arena.reset();
  CHECK(root->usage().pages == 1);
  alias.reset();
  CHECK(root->usage().pages == 0);
  CHECK(root->usage().work == 1);
  credit = {};
  CHECK(root->usage().bytes == initial.bytes);
  CHECK(root->usage().work == 0);
}

TEST_CASE("targeted allocation rejection preserves pin, reservations and retryable publication") {
  Fixture f;
  f.deliver(take(f.consumer.request({}, SourceDirection::FORWARD, 2)));
  auto before = f.budget->usage();
  source_allocation_test::failNext = true;
  auto denied = f.consumer.request({}, SourceDirection::FORWARD, 2);
  CHECK(denied.error() == SourceError::NO_MEMORY);
  CHECK(f.budget->usage().bytes == before.bytes);
  CHECK(f.budget->usage().work == before.work);
  CHECK(take(f.consumer.pin()).size() == 2);
  auto request = take(f.consumer.request({}, SourceDirection::FORWARD, 2));
  auto result = std::async(std::launch::async, [request] {
                  source_allocation_test::failNext = true;

                  return request.load();
                }).get();
  CHECK(result.error() == SourceError::NO_MEMORY);
  CHECK(f.budget->usage().work == 1);
  source_allocation_test::failNext = true;
  auto publication = request.complete(result);
  CHECK(publication.error() == SchedulerError::NO_MEMORY);
  CHECK(take(f.consumer.pin()).size() == 2);
  take(request.complete(result));
  f.dispatch();
  CHECK(take(f.consumer.lastError()) == SourceError::NO_MEMORY);
  request = {};
  CHECK(f.budget->usage().work == 0);
}

TEST_CASE("oversized transitive row payload is charged and rejected without replacing valid window") {
  struct TextProvider final : WindowProvider<std::pmr::string> {
    std::size_t bytes{16};

    SourceCapabilities capabilities() const noexcept override {
      return {};
    }

    std::expected<SourceReadReply<std::pmr::string>, SourceError>
    load(const SourceReadCommand &command, PageBuilder<std::pmr::string> &out, std::stop_token) override {
      auto added = out.emplace({1, 1}, 1, bytes, 'x', out.resource());

      if (!added) {
        return std::unexpected(added.error());
      }

      return out.ready({command.source, {0, 0}}, {command.source, {1, 0}}, SourceEdge::MORE);
    }
  };

  auto scheduler = take(Scheduler::create());
  auto tree = take(WidgetTree::create(*scheduler));
  auto provider = std::make_shared<TextProvider>();
  auto source = take(WindowSource<std::pmr::string>::create(*tree, take(tree->root()), provider));
  auto consumer = take(source->openConsumer());
  auto deliver = [&] {
    auto request = take(consumer.request({}, SourceDirection::FORWARD, 1));
    auto reply = std::async(std::launch::async, [request] {
                   return request.load();
                 }).get();
    take(request.complete(reply));
    take(scheduler->dispatch());
  };
  deliver();
  CHECK(take(consumer.pin()).page(0).rows()[0].value.size() == 16);
  provider->bytes = 128 * 1024;
  deliver();
  CHECK(take(consumer.lastError()) == SourceError::ROW_TOO_LARGE);
  CHECK(take(consumer.pin()).page(0).rows()[0].value.size() == 16);
}

TEST_CASE("a full existing mailbox retains the prepared reply for one publication retry") {
  auto scheduler = take(Scheduler::create({.data_capacity = 1}));
  auto tree = take(WidgetTree::create(*scheduler));
  auto provider = std::make_shared<Generated>();
  auto source = take(WindowSource<int>::create(*tree, take(tree->root()), provider));
  auto a = take(source->openConsumer());
  auto b = take(source->openConsumer());
  auto first = take(a.request({}, SourceDirection::FORWARD, 1));
  auto second = take(b.request({}, SourceDirection::FORWARD, 1));
  auto replyA = std::async(std::launch::async, [first] {
                  return first.load();
                }).get();
  auto replyB = std::async(std::launch::async, [second] {
                  return second.load();
                }).get();
  take(first.complete(replyA));
  CHECK(second.complete(replyB).error() == SchedulerError::WOULD_BLOCK);
  CHECK(source->budget()->usage().work == 2);
  take(scheduler->dispatch());
  take(second.complete(replyB));
  take(scheduler->dispatch());
  CHECK(take(a.pin()).size() == 1);
  CHECK(take(b.pin()).size() == 1);
  CHECK(provider->calls == 2);
}

TEST_CASE("explicit consumer cancellation wins over later readiness without losing the old pin") {
  Fixture f;
  f.deliver(take(f.consumer.request({}, SourceDirection::FORWARD, 2)));
  auto work = take(f.consumer.request({}, SourceDirection::FORWARD, 3));
  take(f.consumer.cancel());
  CHECK(work.stopRequested());
  work = {};
  take(f.source->notifyReady(f.source->stamp(), 2));
  f.dispatch();
  CHECK_FALSE(take(f.consumer.takeRequest()));
  CHECK(take(f.consumer.pin()).size() == 2);
  CHECK(take(f.consumer.lastError()) == SourceError::CANCELLED);
}

TEST_CASE("final immutable pin release on a worker returns the same page credit") {
  Fixture f;
  f.deliver(take(f.consumer.request({}, SourceDirection::FORWARD, 2)));
  auto pin = take(f.consumer.pin());
  f.consumer.close();
  f.source.reset();
  CHECK(f.budget->usage().pages == 1);
  std::thread worker([pin = std::move(pin)]() mutable {
    pin = {};
  });
  worker.join();
  CHECK(f.budget->usage().pages == 0);
  CHECK(f.budget->usage().work == 0);
}

TEST_CASE("a copied notifier is worker-safe after source and tree destruction") {
  Fixture f;
  auto notifier = f.source->notifier();
  auto stamp = f.source->stamp();
  f.source.reset();
  f.tree.reset();
  auto result = std::async(std::launch::async, [notifier, stamp] {
                  return notifier.notifyReady(stamp, 2);
                }).get();
  CHECK(result.error() == SourceError::CLOSED);
}

TEST_CASE("trusted domain loader preserves the standard request gates and per-call cause") {
  Fixture f;
  f.deliver(take(f.consumer.request({}, SourceDirection::FORWARD, 2)));
  auto request = take(f.consumer.request({}, SourceDirection::FORWARD, 1));
  int diagnostic{};
  auto loader = [&](WindowProvider<int> &provider, const SourceReadCommand &, PageBuilder<int> &,
                    std::stop_token) -> std::expected<SourceReadReply<int>, SourceError> {
    CHECK(&provider == f.provider.get());
    diagnostic = 42;
    return std::unexpected(SourceError::IO_ERROR);
  };

  CHECK(detail::WindowRequestAccess::load(request, loader).error() == SourceError::WRONG_THREAD);
  CHECK(diagnostic == 0);
  auto result = std::async(std::launch::async, [&] {
                  return detail::WindowRequestAccess::load(request, loader);
                }).get();

  CHECK(result.error() == SourceError::IO_ERROR);
  CHECK(diagnostic == 42);
  take(request.complete(result));
  f.dispatch();
  CHECK(take(f.consumer.lastError()) == SourceError::IO_ERROR);
  CHECK(take(f.consumer.pin()).size() == 2);
  auto again = std::async(std::launch::async, [&] {
                 return request.load();
               }).get();

  CHECK(again.error() == SourceError::ALREADY_COMPLETED);
  auto cancelled = take(f.consumer.request({}, SourceDirection::FORWARD, 1));
  cancelled.cancel();
  diagnostic = 0;
  auto stopped = std::async(std::launch::async, [&] {
                   return detail::WindowRequestAccess::load(cancelled, loader);
                 }).get();

  CHECK(stopped.error() == SourceError::CANCELLED);
  CHECK(diagnostic == 0);
}
