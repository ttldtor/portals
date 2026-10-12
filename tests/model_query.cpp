// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/model_query.hpp>

#include <array>
#include <cstdlib>
#include <new>
#include <random>
#include <thread>

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

// Standard algorithms may request a temporary buffer through the nothrow form.
// Keep every allocation/deallocation pair on the same injected allocator.
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size);
  } catch (const std::bad_alloc &) {
    return nullptr;
  }
}

void operator delete(void *value, const std::nothrow_t &) noexcept {
  ::operator delete(value);
}

void operator delete(void *value, std::size_t) noexcept {
  std::free(value);
}

void *operator new[](std::size_t size) {
  return ::operator new(size);
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  return ::operator new(size, std::nothrow);
}

void operator delete[](void *value, const std::nothrow_t &) noexcept {
  ::operator delete(value);
}

void operator delete[](void *value) noexcept {
  ::operator delete(value);
}

void operator delete[](void *value, std::size_t) noexcept {
  ::operator delete(value);
}

namespace {
using namespace portals;

struct Criteria {
  int minimum{};
  bool descending{};
  std::shared_ptr<const int> lifetime;
};

struct Rows : ModelSnapshot<int> {
  std::vector<ModelRow<int>> values;

  explicit Rows(std::uint64_t revision = 7) : ModelSnapshot(revision, 6) {
    for (int i = 0; i < 6; ++i) {
      values.push_back({{static_cast<std::uint64_t>(i + 1), 3}, i});
    }
  }

  ModelRows<int> read(RowRange range) const override {
    return {RangeState::READY, std::span(values).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }
};

struct Filter {
  bool operator()(const int &value, const Criteria &criteria) const {
    return value >= criteria.minimum;
  }
};

struct Compare {
  bool operator()(const int &left, const int &right, const Criteria &criteria) const {
    return criteria.descending ? left > right : left < right;
  }
};

using Request = QueryRequest<int, Criteria>;
using Controller = QueryController<int, Criteria>;
using Memory = MemoryQueryProvider<int, Criteria, Filter, Compare>;

struct Delayed final : QueryProvider<int, Criteria> {
  std::vector<Request> work;

  void start(Request request) override {
    work.push_back(std::move(request));
  }

  std::expected<PublishResult, SchedulerError> finish(std::size_t index) {
    auto request = work.at(index);
    std::expected<PublishResult, SchedulerError> accepted = std::unexpected(SchedulerError::CLOSED);

    std::thread worker([&] {
      auto result = transformSnapshot(*request.query(), Filter{}, Compare{}, request.options(), [&] {
        return request.stopRequested();
      });

      accepted = request.complete(std::move(result));
    });

    worker.join();
    work.erase(work.begin() + static_cast<std::ptrdiff_t>(index));

    return accepted;
  }
};

template <class T, class E> T take(std::expected<T, E> value) {
  REQUIRE(value);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

struct Fixture {
  std::unique_ptr<Scheduler> scheduler;
  std::unique_ptr<WidgetTree> tree;
  Node<Widget> root;
  std::shared_ptr<const ModelSnapshot<int>> source = std::make_shared<const Rows>();
  std::unique_ptr<Controller> controller;

  Fixture(std::unique_ptr<QueryProvider<int, Criteria>> provider, QueryOptions options = {},
          SchedulerOptions schedulerOptions = {}) {
    scheduler = take(Scheduler::create(schedulerOptions));
    tree = take(WidgetTree::create(*scheduler));
    root = take(tree->root());
    controller = take(Controller::create(*tree, root, source, std::move(provider), options));
  }

  void drain() {
    // The query completion queues an existing model publication for a later delivery.
    for (int pass = 0; pass < 3; ++pass) {
      take(scheduler->dispatch());
    }
  }
};
} // namespace

TEST_CASE("local projection retains source identities and exact revision") {
  std::shared_ptr<const ModelSnapshot<int>> source = std::make_shared<const Rows>();
  Query<int, Criteria> query{source, {2, true}, {1, 2}};
  auto result = transformSnapshot(query, Filter{}, Compare{});

  REQUIRE(result);
  CHECK(result->totalRows == 4);
  CHECK(result->snapshot->revision() == 7);

  auto range = ModelRange<int>::read(result->snapshot, {0, 2}, 2);

  REQUIRE(range);
  CHECK(range->rows()[0].value == 4);
  CHECK(range->rows()[0].id == RowId{5, 3});
  CHECK(range->rows()[1].id == RowId{4, 3});
  CHECK(source->revision() == 7);
}

TEST_CASE("in-memory and delayed providers obey identical query and projection contracts") {
  for (bool delayed : {false, true}) {
    auto provider = std::make_unique<Delayed>();
    auto *simulator = provider.get();
    std::unique_ptr<QueryProvider<int, Criteria>> chosen;

    if (delayed) {
      chosen = std::move(provider);
    } else {
      chosen = std::make_unique<Memory>(Filter{}, Compare{});
    }

    Fixture fixture(std::move(chosen));
    auto initial = take(fixture.controller->pin());
    auto request = take(fixture.controller->submit({fixture.source, {2, true}, {1, 2}}));

    if (delayed) {
      REQUIRE(simulator->finish(0));
    }

    CHECK(take(fixture.controller->pin()).dataRevision() == 1);
    fixture.drain();

    auto first = take(fixture.controller->pin());
    auto provenance = take(fixture.controller->snapshot());
    auto rows = take(first.read({0, 2}));

    CHECK(first.dataRevision() == 2);
    CHECK(first.rowCount() == 2);
    CHECK(provenance->sourceRevision() == 7);
    CHECK(provenance->revision() == 2);
    CHECK(provenance->sourceIdentity() != 0);
    CHECK(provenance->queryGeneration() == 1);
    CHECK(provenance->totalRows() == 4);
    CHECK(provenance->source() == fixture.source);
    CHECK(rows.rows()[0].id == RowId{5, 3});
    CHECK(initial.rowCount() == 6);

    auto secondRequest = take(fixture.controller->submit({fixture.source, {4, false}, {0, 2}}));

    if (delayed) {
      REQUIRE(simulator->finish(0));
    }

    fixture.drain();

    auto second = take(fixture.controller->pin());

    CHECK(second.dataRevision() == 3);
    CHECK(take(fixture.controller->snapshot())->sourceRevision() == 7);
    CHECK(take(second.read({0, 2})).rows()[0].value == 4);
    CHECK(take(first.read({0, 2})).rows()[0].value == 4);
    CHECK(take(first.read({0, 2})).rows()[1].value == 3);
    CHECK(take(second.read({0, 2})).rows()[1].value == 5);
  }
}

TEST_CASE("late query and source completions cannot replace current projection") {
  auto provider = std::make_unique<Delayed>();
  auto *simulator = provider.get();
  Fixture fixture(std::move(provider));
  auto old = take(fixture.controller->submit({fixture.source, {0, false}, {0, 2}}));
  auto newerSource = std::make_shared<const Rows>(8);
  auto current = take(fixture.controller->submit({newerSource, {4, true}, {0, 2}}));

  CHECK(old.stopRequested());
  CHECK(simulator->finish(0).error() == SchedulerError::CLOSED);
  REQUIRE(simulator->finish(0));
  fixture.drain();

  auto provenance = take(fixture.controller->snapshot());

  CHECK(provenance->sourceRevision() == 8);
  CHECK(provenance->queryGeneration() == 2);
  auto stale = fixture.controller->submit({fixture.source, {}, {0, 2}});

  REQUIRE_FALSE(stale);
  CHECK(stale.error() == TreeError::INVALID_ARGUMENT);
}

TEST_CASE("cancelled tokens retain reservation until every work owner releases") {
  auto provider = std::make_unique<Delayed>();
  auto *simulator = provider.get();
  Fixture fixture(std::move(provider), {1, 256, 65536, 1048576});
  auto request = take(fixture.controller->submit({fixture.source, {}, {0, 2}}));

  request.cancel();

  auto denied = fixture.controller->submit({fixture.source, {}, {0, 2}});

  REQUIRE_FALSE(denied);
  CHECK(denied.error() == TreeError::WOULD_BLOCK);
  request = {};
  CHECK_FALSE(fixture.controller->submit({fixture.source, {}, {0, 2}}));
  simulator->work.clear();
  REQUIRE(fixture.controller->submit({fixture.source, {}, {0, 2}}));
}

TEST_CASE("close cancels worker owners and retained source outlives UI") {
  auto provider = std::make_unique<Delayed>();
  Fixture fixture(std::move(provider));
  auto request = take(fixture.controller->submit({fixture.source, {}, {0, 2}}));
  auto frame = take(fixture.controller->pin());

  take(fixture.controller->close());
  CHECK(request.stopRequested());
  CHECK(request.complete(std::unexpected(QueryError::CANCELLED)).error() == SchedulerError::CLOSED);
  CHECK(fixture.controller->pin().error() == TreeError::CLOSED);
  CHECK(take(frame.read({0, 2})).rows()[1].id == RowId{2, 3});
}

TEST_CASE("completion shape and exact source revision are validated before queueing") {
  auto provider = std::make_unique<Delayed>();
  Fixture fixture(std::move(provider));
  auto request = take(fixture.controller->submit({fixture.source, {}, {0, 6}}));

  CHECK(request.complete(QueryPage<int>{std::make_shared<const Rows>(8), 6}).error() ==
        SchedulerError::INVALID_ARGUMENT);
  CHECK(request.complete(QueryPage<int>{fixture.source, 5}).error() == SchedulerError::INVALID_ARGUMENT);
  CHECK(take(fixture.controller->pin()).dataRevision() == 1);
  REQUIRE(request.complete(std::unexpected(QueryError::NOT_READY)));
  fixture.drain();
  CHECK(take(fixture.controller->lastQueryError()) == QueryError::NOT_READY);
  CHECK(take(fixture.controller->pin()).dataRevision() == 1);
}

TEST_CASE("bounded local evaluation translates work limits callback errors and cancellation") {
  auto source = std::make_shared<const Rows>();
  Query<int, Criteria> query{source, {}, {0, 2}};

  CHECK(transformSnapshot(query, Filter{}, Compare{}, {4, 1, 65536, 1048576}).error() == QueryError::INVALID_ARGUMENT);
  CHECK(transformSnapshot(query, Filter{}, Compare{}, {4, 256, 2, 1048576}).error() == QueryError::LIMIT_EXCEEDED);
  CHECK(transformSnapshot(query, Filter{}, Compare{}, {4, 256, 65536, 1}).error() == QueryError::LIMIT_EXCEEDED);
  CHECK(transformSnapshot(query, Filter{}, Compare{}, {}, [] {
          return true;
        }).error() == QueryError::CANCELLED);
  CHECK(transformSnapshot(
          query,
          [](int, const Criteria &) -> bool {
            throw std::runtime_error("filter");
          },
          Compare{})
          .error() == QueryError::CALLBACK_FAILED);
  CHECK(transformSnapshot(query, Filter{}, [](int, int, const Criteria &) -> bool {
          throw std::runtime_error("comparison");
        }).error() == QueryError::CALLBACK_FAILED);
}

TEST_CASE("randomized local projection preserves stable identity and deterministic pages") {
  std::mt19937 random(67253);

  for (int iteration = 0; iteration < 100; ++iteration) {
    auto source = std::make_shared<Rows>();

    std::shuffle(source->values.begin(), source->values.end(), random);

    const int minimum = static_cast<int>(random() % 8);
    const bool descending = random() % 2 != 0;
    const auto first = random() % 8;
    const auto count = static_cast<std::size_t>(random() % 7);
    auto result =
      take(transformSnapshot(Query<int, Criteria>{source, {minimum, descending}, {first, count}}, Filter{}, Compare{}));
    std::vector<ModelRow<int>> expected;

    for (const auto &row : source->values) {
      if (row.value >= minimum) {
        expected.push_back(row);
      }
    }

    std::stable_sort(expected.begin(), expected.end(), [descending](const auto &left, const auto &right) {
      return descending ? left.value > right.value : left.value < right.value;
    });

    const auto begin = std::min<std::size_t>(first, expected.size());
    const auto size = std::min(count, expected.size() - begin);
    auto rows = take(ModelRange<int>::read(result.snapshot, {0, size}, 256));

    CHECK(result.totalRows == expected.size());

    for (std::size_t offset = 0; offset < size; ++offset) {
      CHECK(rows.rows()[offset].id == expected[begin + offset].id);
      CHECK(rows.rows()[offset].value == expected[begin + offset].value);
    }
  }
}

TEST_CASE("rejected in-memory completion retains ownership and retries without reevaluation") {
  Fixture fixture(std::make_unique<Memory>(Filter{}, Compare{}), {}, {1, 32, 1, 32, 32, 256});
  auto filler = take(fixture.tree->connect<int>(fixture.root, [](Widget &, std::shared_ptr<const int>) {
  }));

  REQUIRE(filler.publisher.publish(std::make_shared<const int>(1)));

  auto request = take(fixture.controller->submit({fixture.source, {4, false}, {0, 2}}));
  auto denied = fixture.controller->retryPublication();

  REQUIRE_FALSE(denied);
  CHECK(denied.error() == TreeError::WOULD_BLOCK);
  CHECK(take(fixture.controller->lastError()) == TreeError::WOULD_BLOCK);
  take(fixture.scheduler->dispatch());
  take(fixture.controller->retryPublication());
  CHECK(take(fixture.controller->pin()).dataRevision() == 1);
  fixture.drain();
  take(fixture.controller->retryPublication());
  CHECK_FALSE(take(fixture.controller->lastError()));
  CHECK(take(fixture.controller->pin()).dataRevision() == 2);
  CHECK(take(take(fixture.controller->pin()).read({0, 2})).rows()[0].value == 4);
}

TEST_CASE("queued superseded completion is fenced before model publication") {
  auto provider = std::make_unique<Delayed>();
  auto *simulator = provider.get();
  Fixture fixture(std::move(provider));
  auto old = take(fixture.controller->submit({fixture.source, {}, {0, 2}}));

  REQUIRE(simulator->finish(0));

  auto current = take(fixture.controller->submit({fixture.source, {5, false}, {0, 2}}));

  REQUIRE(simulator->finish(0));
  fixture.drain();

  CHECK(take(fixture.controller->snapshot())->queryGeneration() == 2);
  CHECK(take(fixture.controller->pin()).dataRevision() == 2);
  CHECK(take(fixture.controller->pin()).rowCount() == 1);
}

TEST_CASE("controller and provider callbacks enforce UI affinity and translate failures") {
  struct Throwing final : QueryProvider<int, Criteria> {
    void start(Request request) override {
      take(request.complete(transformSnapshot(*request.query(), Filter{}, Compare{})));

      throw std::runtime_error("provider");
    }
  };

  Fixture fixture(std::make_unique<Throwing>());
  auto rejected = fixture.controller->submit({fixture.source, {}, {0, 2}});

  REQUIRE_FALSE(rejected);
  CHECK(rejected.error() == TreeError::CALLBACK_FAILED);
  CHECK(take(fixture.controller->lastQueryError()) == QueryError::CALLBACK_FAILED);
  fixture.drain();

  bool wrongThread{};

  std::thread worker([&] {
    wrongThread = fixture.controller->submit({fixture.source, {}, {0, 2}}).error() == TreeError::WRONG_THREAD &&
                  fixture.controller->close().error() == TreeError::WRONG_THREAD;
  });

  worker.join();
  CHECK(wrongThread);
  CHECK(take(fixture.controller->pin()).dataRevision() == 1);
}

TEST_CASE("reentrant criteria destruction observes its still reserved work budget") {
  struct OwningCriteria {
    std::shared_ptr<const int> lifetime;
  };

  using OwningRequest = QueryRequest<int, OwningCriteria>;
  using OwningController = QueryController<int, OwningCriteria>;

  struct HoldingProvider final : QueryProvider<int, OwningCriteria> {
    OwningRequest retained;

    void start(OwningRequest request) override {
      retained = std::move(request);
    }
  };

  auto scheduler = take(Scheduler::create());
  auto tree = take(WidgetTree::create(*scheduler));
  auto root = take(tree->root());
  auto source = std::make_shared<const Rows>();
  auto provider = std::make_unique<HoldingProvider>();
  auto *holder = provider.get();
  auto controller = take(OwningController::create(*tree, root, source, std::move(provider), {1, 256, 65536, 1048576}));
  bool reservedDuringDeletion{};

  auto storage = std::shared_ptr<const int>(new int(1), [&](const int *value) {
    auto reentered = controller->submit({source, {}, {0, 2}});

    reservedDuringDeletion = !reentered && reentered.error() == TreeError::WOULD_BLOCK;
    delete value;
  });

  auto request = take(controller->submit({source, {std::move(storage)}, {0, 2}}));

  holder->retained = {};
  request = {};
  CHECK(reservedDuringDeletion);
  REQUIRE(controller->submit({source, {}, {0, 2}}));
}

TEST_CASE("local projection retains source storage reached through copied rows") {
  bool destroyed{};

  struct BorrowedRows final : ModelSnapshot<std::string_view> {
    std::string storage{"owned by source"};
    std::array<ModelRow<std::string_view>, 1> rows{{{{1, 1}, storage}}};
    bool &destroyed;

    explicit BorrowedRows(bool &value) : ModelSnapshot(1, 1), destroyed(value) {
    }

    ~BorrowedRows() override {
      destroyed = true;
    }

    ModelRows<std::string_view> read(RowRange range) const override {
      return {RangeState::READY, std::span(rows).subspan(static_cast<std::size_t>(range.first), range.count), {}};
    }
  };

  auto source = std::make_shared<const BorrowedRows>(destroyed);
  auto result = take(transformSnapshot(
    Query<std::string_view, int>{source, 0, {0, 1}},
    [](auto, int) {
      return true;
    },
    [](auto left, auto right, int) {
      return left < right;
    }));

  source.reset();
  CHECK_FALSE(destroyed);
  CHECK(take(ModelRange<std::string_view>::read(result.snapshot, {0, 1}, 1)).rows()[0].value == "owned by source");
  result.snapshot.reset();
  CHECK(destroyed);
}

TEST_CASE("retiring original source may destroy the controller during provenance observation") {
  struct RetiringRows final : Rows {
    std::function<void()> onDestroy;

    ~RetiringRows() override {
      onDestroy();
    }
  };

  auto scheduler = take(Scheduler::create());
  auto tree = take(WidgetTree::create(*scheduler));
  auto root = take(tree->root());
  auto source = std::make_shared<RetiringRows>();
  auto controller = take(Controller::create(*tree, root, source, std::make_unique<Memory>(Filter{}, Compare{})));
  bool destroyed{};

  source->onDestroy = [&] {
    destroyed = true;
    controller.reset();
  };

  source.reset();

  auto request = take(controller->submit({std::make_shared<const Rows>(8), {4, false}, {0, 2}}));

  for (int pass = 0; pass < 3; ++pass) {
    take(scheduler->dispatch());
  }

  auto result = take(controller->snapshot());

  CHECK(destroyed);
  CHECK_FALSE(controller);
  CHECK(result->sourceRevision() == 8);
  CHECK(result->revision() == 2);
}

TEST_CASE("retiring rejected criteria may submit a newer query during memory provider replacement") {
  Fixture fixture(std::make_unique<Memory>(Filter{}, Compare{}), {4, 256, 65536, 1048576}, {1, 32, 1, 32, 32, 256});
  auto filler = take(fixture.tree->connect<int>(fixture.root, [](Widget &, std::shared_ptr<const int>) {
  }));

  REQUIRE(filler.publisher.publish(std::make_shared<const int>(1)));

  bool reentered{};
  auto criteriaStorage = std::shared_ptr<const int>(new int(1), [&](const int *value) {
    delete value;

    auto third = fixture.controller->submit({fixture.source, {5, false}, {0, 1}});

    reentered = third.has_value();
  });
  auto first = take(fixture.controller->submit({fixture.source, {1, false, std::move(criteriaStorage)}, {0, 1}}));

  first = {};

  auto second = fixture.controller->submit({fixture.source, {4, false}, {0, 1}});

  CHECK(reentered);
  CHECK_FALSE(second);

  if (!second) {
    CHECK(second.error() == TreeError::CLOSED);
  }

  take(fixture.scheduler->dispatch());
  take(fixture.controller->retryPublication());
  fixture.drain();

  auto snapshot = take(fixture.controller->snapshot());
  auto rows = take(take(fixture.controller->pin()).read({0, 1}));

  CHECK(snapshot->queryGeneration() == 3);
  CHECK(rows.rows()[0].value == 5);
  CHECK(rows.rows()[0].id == RowId{6, 3});
}

TEST_CASE("provider start closing its controller returns a typed closed failure") {
  struct Closing final : QueryProvider<int, Criteria> {
    std::function<void()> close;
    Request retained;

    void start(Request request) override {
      retained = std::move(request);
      close();
    }
  };

  auto provider = std::make_unique<Closing>();
  auto *closer = provider.get();
  Fixture fixture(std::move(provider));

  closer->close = [&] {
    take(fixture.controller->close());
  };

  auto result = fixture.controller->submit({fixture.source, {}, {0, 1}});

  REQUIRE_FALSE(result);
  CHECK(result.error() == TreeError::CLOSED);
}

TEST_CASE("stable ordering of equal keys retains exact identity and generation lookup") {
  auto source = std::make_shared<Rows>();

  for (auto &row : source->values) {
    row.value %= 2;
  }

  auto result = take(transformSnapshot(Query<int, Criteria>{source, {}, {0, 6}}, Filter{}, Compare{}));
  auto rows = take(ModelRange<int>::read(result.snapshot, {0, 6}, 6));
  const std::array<std::uint64_t, 6> expected{1, 3, 5, 2, 4, 6};

  for (std::size_t index = 0; index < expected.size(); ++index) {
    CHECK(rows.rows()[index].id == RowId{expected[index], 3});
  }

  CHECK(result.snapshot->locateRow({2, 3}).state == RowLookupState::FOUND);
  CHECK(result.snapshot->locateRow({2, 3}).index == 3);
  CHECK(result.snapshot->locateRow({2, 2}).state == RowLookupState::NOT_FOUND);
}

namespace {
struct MoveControl {
  int moves{};
  std::function<void()> callback;
};

struct MovingCriteria {
  std::shared_ptr<MoveControl> control;

  explicit MovingCriteria(std::shared_ptr<MoveControl> value = {}) : control(std::move(value)) {
  }

  MovingCriteria(MovingCriteria &&other) : control(std::move(other.control)) {
    // Move one constructs submit's argument; move two executes inside its body.
    if (control && ++control->moves == 2) {
      control->callback();
    }
  }
};

using MovingRequest = QueryRequest<int, MovingCriteria>;
using MovingController = QueryController<int, MovingCriteria>;

struct MovingProvider final : QueryProvider<int, MovingCriteria> {
  std::vector<MovingRequest> requests;

  void start(MovingRequest request) override {
    requests.push_back(std::move(request));
  }
};
} // namespace

TEST_CASE("criteria move inside submit cannot bypass one-reservation budget or overwrite newer query") {
  auto scheduler = take(Scheduler::create());
  auto tree = take(WidgetTree::create(*scheduler));
  auto root = take(tree->root());
  auto source = std::make_shared<const Rows>();
  auto provider = std::make_unique<MovingProvider>();
  auto *holder = provider.get();
  auto controller = take(MovingController::create(*tree, root, source, std::move(provider), {1, 256, 65536, 1048576}));
  auto control = std::make_shared<MoveControl>();
  MovingRequest nested;

  control->callback = [&] {
    nested = take(controller->submit({source, MovingCriteria{}, {0, 1}}));
  };

  Query<int, MovingCriteria> query{source, MovingCriteria{control}, {0, 1}};
  auto outer = controller->submit(std::move(query));

  CHECK(control->moves == 2);
  CHECK_FALSE(outer);

  if (!outer) {
    CHECK(outer.error() == TreeError::CLOSED);
  }

  CHECK(holder->requests.size() == 1);
  CHECK_FALSE(nested.stopRequested());
}

TEST_CASE("provider removing context after start cancels the token and returns original tree failure") {
  struct Removing final : QueryProvider<int, Criteria> {
    std::function<void()> remove;
    Request retained;

    void start(Request request) override {
      retained = std::move(request);
      remove();
    }
  };

  auto provider = std::make_unique<Removing>();
  auto *remover = provider.get();
  Fixture fixture(std::move(provider));

  remover->remove = [&] {
    take(fixture.tree->remove(fixture.root));
  };

  auto result = fixture.controller->submit({fixture.source, {}, {0, 1}});

  REQUIRE_FALSE(result);
  CHECK(remover->retained.stopRequested());
  CHECK(result.error() == fixture.controller->pin().error());
}

TEST_CASE("core model application allocation failure retains exact projection for retry") {
  Fixture fixture(std::make_unique<Memory>(Filter{}, Compare{}), {}, {256, 32, 1, 32, 32, 256});
  auto request = take(fixture.controller->submit({fixture.source, {4, false}, {0, 2}}));

  take(fixture.scheduler->dispatch());
  CHECK(take(fixture.controller->pin()).dataRevision() == 1);

  allocation_test::fail = true;

  auto delivery = fixture.scheduler->dispatch();

  allocation_test::fail = false;
  REQUIRE(delivery);
  CHECK(take(fixture.controller->lastError()) == TreeError::NO_MEMORY);
  CHECK(take(fixture.controller->pin()).dataRevision() == 1);
  take(fixture.controller->retryPublication());
  take(fixture.scheduler->dispatch());

  auto snapshot = take(fixture.controller->snapshot());

  CHECK(snapshot->revision() == 2);
  CHECK(snapshot->sourceRevision() == 7);
  CHECK(snapshot->queryGeneration() == 1);
  CHECK(take(fixture.controller->pin()).dataRevision() == 2);
  CHECK(take(take(fixture.controller->pin()).read({0, 2})).rows()[0].value == 4);
}

TEST_CASE("criteria move inside submit may destroy the controller without touching its members afterward") {
  auto scheduler = take(Scheduler::create());
  auto tree = take(WidgetTree::create(*scheduler));
  auto root = take(tree->root());
  auto source = std::make_shared<const Rows>();
  auto controller = take(MovingController::create(*tree, root, source, std::make_unique<MovingProvider>()));
  auto control = std::make_shared<MoveControl>();

  control->callback = [&] {
    controller.reset();
  };

  Query<int, MovingCriteria> query{source, MovingCriteria{control}, {0, 1}};
  auto result = controller->submit(std::move(query));

  CHECK(control->moves == 2);
  CHECK_FALSE(controller);
  REQUIRE_FALSE(result);
  CHECK(result.error() == TreeError::CLOSED);
}
