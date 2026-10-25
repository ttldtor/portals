// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/detail/search_index.hpp>
#include <portals/ui.hpp>
#include <portals/ui_suggestions.hpp>

#include <chrono>
#include <future>
#include <iostream>
#include <latch>
#include <random>

namespace {
using namespace portals;
using namespace std::chrono_literals;

template <class T, class E> T take(std::expected<T, E> value) {
  if (!value) {
    throw std::runtime_error("expected operation failed in suggestions fixture");
  }
  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

struct Probe {
  std::atomic<std::size_t> projections{}, reads{};
  std::function<void()> projected;
  std::thread::id worker;
};

class Rows final : public ModelSnapshot<std::string> {
public:
  Rows(std::vector<ModelRow<std::string>> rows, std::shared_ptr<Probe> probe, std::uint64_t revision = 1)
      : ModelSnapshot(revision, rows.size()), rows_(std::move(rows)), probe_(std::move(probe)) {
  }

  ModelRows<std::string> read(RowRange range) const override {
    ++probe_->reads;
    return {RangeState::READY, std::span(rows_).subspan(static_cast<std::size_t>(range.first), range.count), {}};
  }

private:
  std::vector<ModelRow<std::string>> rows_;
  std::shared_ptr<Probe> probe_;
};

auto project(std::shared_ptr<Probe> probe) {
  return [probe](const std::string &value) -> std::string_view {
    ++probe->projections;
    probe->worker = std::this_thread::get_id();
    if (probe->projected) {
      probe->projected();
    }
    return value;
  };
}

std::shared_ptr<const ModelSnapshot<std::string>> snapshot(const std::vector<std::string> &values,
                                                           std::shared_ptr<Probe> probe, std::uint64_t revision = 1) {
  std::vector<ModelRow<std::string>> rows(0);
  for (const auto &value : values) {
    rows.push_back({{rows.size() + 1, 1}, value});
  }
  return std::make_shared<const Rows>(std::move(rows), std::move(probe), revision);
}

auto workerSource(std::shared_ptr<const ModelSnapshot<std::string>> rows, std::shared_ptr<Probe> probe) {
  auto projection = project(std::move(probe));
  return std::make_shared<const portals::detail::FindWorkerSourceOf<std::string, decltype(projection)>>(
    std::move(rows), std::make_shared<const decltype(projection)>(std::move(projection)));
}

template <class Function> auto worker(Function function) {
  return std::async(std::launch::async, std::move(function)).get();
}

auto build(const std::shared_ptr<const portals::detail::FindWorkerSource> &source, SearchOptions options = {}) {
  SearchSourceIdentity key{source->pin, source->pin.get(), source->revision, 1, 1, {}};
  return portals::detail::buildSearchIndex(*source, key, options, {});
}

std::size_t editDistance(std::string_view left, std::string_view right) {
  std::vector<std::size_t> previous(right.size() + 1), next(previous.size());
  for (std::size_t column = 0; column < previous.size(); ++column) {
    previous[column] = column;
  }
  for (std::size_t row = 1; row <= left.size(); ++row) {
    next[0] = row;
    for (std::size_t column = 1; column <= right.size(); ++column) {
      next[column] = std::min(
        {previous[column] + 1, next[column - 1] + 1, previous[column - 1] + (left[row - 1] != right[column - 1])});
    }
    previous.swap(next);
  }
  return previous.back();
}

// Independent exhaustive ASCII oracle: enumerate every source row, term and term-prefix.
std::vector<SearchRank> oracle(const std::vector<std::string> &labels, std::string_view query, std::size_t topK) {
  std::vector<SearchRank> ranked;
  for (std::size_t row = 0; row < labels.size(); ++row) {
    std::vector<std::pair<std::string_view, bool>> terms{{labels[row], false}};
    auto remaining = std::string_view(labels[row]);
    while (!remaining.empty()) {
      const auto first = remaining.find_first_not_of(' ');
      if (first == std::string_view::npos) {
        break;
      }
      remaining.remove_prefix(first);
      const auto end = remaining.find(' ');
      terms.emplace_back(remaining.substr(0, end), true);
      remaining.remove_prefix(end == std::string_view::npos ? remaining.size() : end);
    }
    std::optional<SearchRank> best;
    for (auto [term, token] : terms) {
      for (std::size_t depth = 1; depth <= term.size(); ++depth) {
        const auto distance = editDistance(query, term.substr(0, depth));
        if (distance > (query.size() >= 3 ? 1u : 0u)) {
          continue;
        }
        SearchRank rank{static_cast<std::uint8_t>(distance), token, term.size() - depth, row, {row + 1, 1}};
        if (!best || rank.tuple() < best->tuple()) {
          best = rank;
        }
      }
    }
    if (best) {
      ranked.push_back(*best);
    }
  }
  std::ranges::sort(ranked, [](const auto &left, const auto &right) {
    return left.tuple() < right.tuple();
  });
  ranked.resize(std::min(topK, ranked.size()));
  return ranked;
}

TEST_CASE("indexed fuzzy top-K agrees with an independent exhaustive row/term/prefix oracle") {
  std::mt19937 random(263);
  std::vector<std::string> labels{"open file", "open recent file", "opener", "opn", "pen", "alpha alpha"};
  for (std::size_t row = 0; row < 200; ++row) {
    std::string label;
    for (std::size_t position = 0; position < 3 + random() % 10; ++position) {
      label.push_back("abcde "[random() % 6]);
    }
    labels.push_back(std::move(label));
  }
  auto probe = std::make_shared<Probe>();
  auto source = workerSource(snapshot(labels, probe), probe);
  worker([&] {
    auto index = take(build(source));
    for (std::string_view query : {"a", "ab", "abc", "aaa", "bad", "open", "opn", "file", "alpha", "zzzz"}) {
      auto result = take(portals::detail::querySearchIndex(*index, query, 1, {}));
      const auto expected = oracle(labels, query, index->options.topK);
      REQUIRE(result->rows.size() == expected.size());
      for (std::size_t position = 0; position < expected.size(); ++position) {
        CHECK(result->rows[position].value.rank.tuple() == expected[position].tuple());
      }
    }
  });
  CHECK(probe->projections == labels.size());
  CHECK(probe->worker != std::this_thread::get_id());
}

TEST_CASE("Unicode casefold expansion and canonical grapheme highlights own only bounded display") {
  auto probe = std::make_shared<Probe>();
  auto source = workerSource(
    snapshot({"Show cafe\u0301 notes", "Straße", "Открыть справку", "👩‍💻 workspace"}, probe), probe);
  auto retained = worker([&] {
    auto index = take(build(source));
    auto cafe = take(portals::detail::querySearchIndex(*index, "CAFÉ", 1, {}));
    REQUIRE(cafe->rows.size() == 1);
    CHECK(cafe->rows.front().id == RowId{1, 1});
    CHECK(cafe->rows.front().value.highlights == std::vector<SearchHighlight>{{5, 11}});
    auto street = take(portals::detail::querySearchIndex(*index, "STRASSE", 2, {}));
    REQUIRE(street->rows.size() == 1);
    CHECK(street->rows.front().value.highlights == std::vector<SearchHighlight>{{0, 7}});
    auto cyrillic = take(portals::detail::querySearchIndex(*index, "ОТКРЫТЬ", 3, {}));
    REQUIRE(cyrillic->rows.size() == 1);
    CHECK(cyrillic->rows.front().id == RowId{3, 1});
    auto emoji = take(portals::detail::querySearchIndex(*index, "👩‍💻", 4, {}));
    REQUIRE(emoji->rows.size() == 1);
    CHECK(emoji->rows.front().value.highlights == std::vector<SearchHighlight>{{0, std::string("👩‍💻").size()}});
    CHECK(take(portals::detail::querySearchIndex(*index, "👩", 5, {}))->rows.empty());
    CHECK(portals::detail::querySearchIndex(*index, "\xff", 6, {}).error() == SearchError::INVALID_TEXT);
    return street;
  });
  source.reset();
  CHECK(retained->rows.front().value.text().bytes() == "Straße");
}

TEST_CASE("default-valid nested canonical casefold prefixes build on a real worker stack") {
  std::vector<std::string> labels(0);
  for (std::size_t length = 1; length <= 2048; ++length) {
    std::string label(0, '\0');
    for (std::size_t pair = 0; pair < length / 2; ++pair) {
      label += "ß";
    }
    if (length % 2) {
      label += 's';
    }
    labels.push_back(std::move(label));
  }
  auto probe = std::make_shared<Probe>();
  auto source = workerSource(snapshot(labels, probe), probe);
  worker([&] {
    auto index = take(build(source));
    CHECK(index->nodes.size() == labels.size());
    auto result = take(portals::detail::querySearchIndex(*index, "sss", 1, {}));
    CHECK(result->rows.size() == 32);
  });
  CHECK(probe->projections == labels.size());
}

TEST_CASE("new allocation budgets are checked before growth including transient capacities") {
  portals::detail::SearchMemory memory(100);
  std::pmr::vector<int> values(0, &memory);
  values.reserve(16);
  CHECK(memory.used() >= 64);
  CHECK_THROWS_AS(values.reserve(32), portals::detail::SearchBudgetExceeded);
  CHECK(memory.peak() <= 100);
  auto probe = std::make_shared<Probe>();
  auto source = workerSource(snapshot({"prefix prefix prefix", "other"}, probe), probe);
  worker([&] {
    auto options = SearchOptions{};
    options.maxTermsPerRow = 2;
    auto index = take(build(source, options));
    CHECK(index->terms.size() == 3);
    index->options.maxQuerySteps = 1;
    CHECK(portals::detail::querySearchIndex(*index, "prefix", 1, {}).error() == SearchError::LIMIT_EXCEEDED);
    options.maxIndexBytes = sizeof(portals::detail::SearchIndex) + 16;
    CHECK(build(source, options).error() == SearchError::LIMIT_EXCEEDED);
  });
}

struct Fixture {
  std::unique_ptr<BackgroundPool> pool;
  std::unique_ptr<Application> app;
  std::unique_ptr<ManagedScene> scene;
  BackgroundExecutor *executor{};
  ui::Ref<LineEdit> input;
  ui::Ref<VirtualList<SearchItem>> list;
  ui::Ref<Label> status;
  ManagedModel<std::string> model;
  std::shared_ptr<Probe> probe = std::make_shared<Probe>();
  std::vector<SearchSelection> selected;
  std::vector<SearchError> errors;
  std::size_t preparedFrames{};

  explicit Fixture(std::vector<std::string> labels = {"open file", "open recent file", "other"},
                   std::string_view initial = "open", ui::SuggestionsConfig config = {},
                   BackgroundExecutor *external = nullptr, ui::LineEditConfig editor = {}, bool committed = false) {
    if (!external) {
      pool = take(BackgroundPool::create());
    }
    executor = external ? external : pool.get();
    ApplicationOptions options;
    options.backgroundExecutor = executor;
    app = take(Application::create(options));
    scene = take(ManagedScene::create(*app));
    model = take(scene->model<std::string>(snapshot(labels, probe)));
    config.status = status;
    config.activated = [this, callback = std::move(config.activated)](SearchSelection value) {
      selected.push_back(value);
      if (callback) {
        callback(std::move(value));
      }
    };
    config.failed = [this, callback = std::move(config.failed)](SearchError error) {
      errors.push_back(error);
      if (callback) {
        callback(error);
      }
    };
    take(ui::mount(
      *scene,
      ui::vbox(ui::lineEdit(input, initial, editor),
               ui::virtualList(
                 list, ui::suggestions(committed ? input.committedText() : input.draftText(), model, project(probe)),
                 std::move(config))
                 .height(5),
               ui::label(status, "Starting"))));
  }

  ~Fixture() {
    scene.reset();
    app.reset();
    if (pool) {
      pool->requestStop();
      pool->join();
    }
  }

  void pump(bool demandDriven = false) {
    auto dispatched = app->scheduler().dispatch();
    take(std::move(dispatched));
    executor->service();
    auto serviced = scene->afterDispatch(*app);
    take(std::move(serviced));
    if (demandDriven && take(app->tree().consumeFrameRequest()) == Invalidation::NONE) {
      return;
    }
    ++preparedFrames;
    auto prepared = scene->prepare(*app, {70, 12});
    take(std::move(prepared));
    const auto bounds = take(CellRect::from({}, {70, 12}));
    bool stable{};

    for (unsigned pass = 0; pass < 3; ++pass) {
      take(app->tree().consumeFrameRequest());
      std::optional<LayoutSnapshot> layout;
      take(app->tree().frame([&] {
        take(layoutTree(app->tree(), app->root(), bounds, bounds));
        layout.emplace(take(LayoutSnapshot::capture(app->tree(), app->root())));
      }));
      take(scene->prepareViews(*app, *layout));
      if (take(app->tree().consumeFrameRequest()) != Invalidation::LAYOUT) {
        stable = true;
        break;
      }
    }
    if (!stable) {
      throw std::runtime_error("layout did not stabilize within three passes");
    }
    take(scene->completedLayout(*app));
    executor->service();
  }

  template <class Predicate> bool until(Predicate predicate, bool demandDriven = false) {
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    do {
      pump(demandDriven);
      if (predicate()) {
        return true;
      }
      std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    std::cerr << "suggestions timeout: projections=" << probe->projections << " reads=" << probe->reads
              << " rows=" << count() << " status=" << take(status.text().get()) << " domain_errors=" << errors.size()
              << " managed_errors=" << take(scene->diagnostics()).errors.size() << '\n';
    return predicate();
  }

  std::uint64_t count() {
    std::uint64_t count{};
    take(app->tree().read(list.node(), [&](const auto &view) {
      count = view.dataStatus().rowCount;
    }));
    return count;
  }

  std::uint64_t resultRevision() {
    std::uint64_t revision{};
    take(app->tree().read(list.node(), [&](const auto &view) {
      revision = view.dataStatus().dataRevision;
    }));
    return revision;
  }

  void query(std::string_view bytes) {
    take((input.text() = bytes).result());
    pump();
  }

  void activate(RowId id) {
    take(setVirtualListSelection(app->tree(), list.node(), id));
    take(app->router().invoke(list.node(), {Action::ACTIVATE}));
    pump();
  }
};

TEST_CASE("native model binding reuses one index and activation retains stable IDs and readable bytes") {
  Fixture fixture;
  REQUIRE(fixture.until([&] {
    return fixture.count() == 2;
  }));
  CHECK(fixture.probe->projections == 3);
  CHECK(fixture.probe->worker != std::this_thread::get_id());
  const auto sourceReads = fixture.probe->reads.load();
  for (unsigned frame = 0; frame < 10; ++frame) {
    fixture.pump();
  }
  CHECK(fixture.probe->reads == sourceReads);
  CHECK(fixture.probe->projections == 3);
  fixture.activate({1, 1});
  REQUIRE(fixture.selected.size() == 1);
  auto selected = fixture.selected.front();
  CHECK(take(selected.current()));
  auto wrongThread = worker([selected] {
    return selected.current();
  });
  REQUIRE_FALSE(wrongThread);
  CHECK(std::get<TreeError>(wrongThread.error().cause) == TreeError::WRONG_THREAD);
  fixture.query("other");
  REQUIRE(fixture.until([&] {
    return fixture.count() == 1;
  }));
  CHECK(fixture.probe->projections == 3);
  CHECK_FALSE(take(selected.current()));
  CHECK(selected.text() == "open file");
  fixture.activate({3, 1});
  REQUIRE(fixture.selected.size() == 2);
  CHECK(fixture.selected.back().rowId() == RowId{3, 1});
  take(fixture.scene->close());
  CHECK_FALSE(selected.current());
  CHECK(selected.text() == "open file");
}

struct GateSignal {
  std::atomic<bool> holdPublications{};
};

struct RunRecord {
  std::atomic<std::size_t> runs{}, publications{}, completedPublications{};
};

class GateExecutor final : public BackgroundExecutor {
  class Job final : public BackgroundJob {
  public:
    Job(std::unique_ptr<BackgroundJob> value, std::shared_ptr<GateSignal> gate, std::shared_ptr<RunRecord> record)
        : inner(std::move(value)), gate(std::move(gate)), record(std::move(record)) {
    }

    void run(std::stop_token stop) noexcept override {
      ++record->runs;
      inner->run(stop);
    }

    std::expected<void, AsyncError> publish() override {
      ++record->publications;
      if (gate->holdPublications) {
        return std::unexpected(AsyncError::WOULD_BLOCK);
      }
      auto result = inner->publish();
      if (result) {
        ++record->completedPublications;
      }
      return result;
    }

    std::unique_ptr<BackgroundJob> inner;
    std::shared_ptr<GateSignal> gate;
    std::shared_ptr<RunRecord> record;
  };

public:
  explicit GateExecutor(BackgroundOptions options = {}) : pool(take(BackgroundPool::create(options))) {
  }

  std::unique_ptr<BackgroundPool> pool;
  std::shared_ptr<GateSignal> signal = std::make_shared<GateSignal>();
  std::vector<std::shared_ptr<RunRecord>> records;
  bool full{};
  std::optional<BackgroundError> rejected;
  std::uint64_t epoch{};
  std::size_t submissions{};

  std::expected<BackgroundTicket, BackgroundError> submit(std::unique_ptr<BackgroundJob> &job) override {
    ++submissions;
    if (full) {
      return std::unexpected(BackgroundError::WOULD_BLOCK);
    }
    if (rejected) {
      return std::unexpected(*rejected);
    }
    auto record = std::make_shared<RunRecord>();
    std::unique_ptr<BackgroundJob> wrapped = std::make_unique<Job>(std::move(job), signal, record);
    auto admitted = pool->submit(wrapped);
    if (!admitted) {
      job = std::move(static_cast<Job &>(*wrapped).inner);
    } else {
      records.push_back(std::move(record));
    }
    return admitted;
  }

  void service() noexcept override {
    pool->service();
  }

  std::uint64_t capacityEpoch() const noexcept override {
    return epoch + pool->capacityEpoch();
  }

  BackgroundWorkerStorage *workerStorage() noexcept override {
    return pool->workerStorage();
  }

  void release() {
    full = false;
    ++epoch;
    pool->service();
  }
};

TEST_CASE("full source admission keeps one latest draft and starts it after capacity release without another key") {
  GateExecutor executor;
  executor.full = true;
  Fixture fixture({"alpha", "beta", "gamma"}, "alpha", {}, &executor);
  fixture.pump();
  for (std::string_view query : {"b", "be", "bet", "beta", "g", "gam", "gamma"}) {
    fixture.query(query);
  }
  const auto submissions = executor.submissions;
  for (unsigned frame = 0; frame < 10; ++frame) {
    fixture.pump();
  }
  CHECK(executor.submissions == submissions);
  CHECK(fixture.probe->projections == 0);
  executor.release();
  REQUIRE(fixture.until([&] {
    return fixture.count() == 1;
  }));
  fixture.activate({3, 1});
  REQUIRE(fixture.selected.size() == 1);
  CHECK(fixture.selected.front().text() == "gamma");
  CHECK(fixture.probe->projections == 3);
  CHECK(executor.records.size() == 2);
}

TEST_CASE("demand-driven preparation wakes latest admission once after capacity release then returns to idle") {
  GateExecutor executor;
  executor.full = true;
  Fixture fixture({"alpha", "beta", "gamma"}, "alpha", {}, &executor);
  fixture.pump(true);
  fixture.pump(true);
  const auto waitingFrames = fixture.preparedFrames;
  for (unsigned turn = 0; turn < 20; ++turn) {
    fixture.pump(true);
  }
  CHECK(fixture.preparedFrames == waitingFrames);
  CHECK(fixture.probe->projections == 0);
  take((fixture.input.text() = "gamma").result());
  fixture.pump(true);
  executor.release();
  REQUIRE(fixture.until(
    [&] {
      return fixture.count() == 1;
    },
    true));
  fixture.activate({3, 1});
  REQUIRE(fixture.selected.size() == 1);
  CHECK(fixture.selected.back().text() == "gamma");
  CHECK(fixture.probe->projections == 3);
  // Consume any final publication/capacity event, then show an unchanged idle
  // registration does not manufacture a permanent redraw/prepare loop.
  fixture.pump(true);
  const auto readyFrames = fixture.preparedFrames;
  for (unsigned turn = 0; turn < 20; ++turn) {
    fixture.pump(true);
  }
  CHECK(fixture.preparedFrames == readyFrames);
}

TEST_CASE("a retried slow index build consumes storage readiness and does not redraw while running") {
  struct BlockPoint {
    std::promise<void> entered;
    std::future<void> ready = entered.get_future();
    std::latch released{1};
    bool done{};

    void block() {
      entered.set_value();
      released.wait();
    }

    void release() {
      if (!done) {
        released.count_down();
        done = true;
      }
    }

    ~BlockPoint() {
      release();
    }
  } reservation, retry;

  class Reservation final : public BackgroundJob {
  public:
    Reservation(BackgroundWorkerStorage &storage, WorkerStorageId slot, BlockPoint &point)
        : storage(storage), slot(slot), point(point) {
    }

    void run(std::stop_token) noexcept override {
      (void)storage.withBuild(slot, 1048576, 1048576,
                              [&](WorkerStorageBuild &) -> std::expected<void, BackgroundError> {
                                point.block();
                                return std::unexpected(BackgroundError::NO_MEMORY);
                              });
    }

    std::expected<void, AsyncError> publish() override {
      return {};
    }

    BackgroundWorkerStorage &storage;
    WorkerStorageId slot;
    BlockPoint &point;
  };

  GateExecutor executor({2, 2, 2, 2097152});

  struct ReleaseBoth {
    BlockPoint &reservation, &retry;

    ~ReleaseBoth() {
      reservation.release();
      retry.release();
    }
  } cleanup{reservation, retry};

  auto lease = take(executor.workerStorage()->registerSlot());
  std::unique_ptr<BackgroundJob> job =
    std::make_unique<Reservation>(*executor.workerStorage(), lease.id(), reservation);
  auto ticket = take(executor.pool->submit(job));
  REQUIRE(reservation.ready.wait_for(5s) == std::future_status::ready);
  ui::SuggestionsConfig config;
  config.search.maxIndexBytes = 1048576;
  config.search.maxBuildScratchBytes = 1048576;
  Fixture fixture({"alpha"}, "alpha", config, &executor);
  fixture.probe->projected = [&] {
    retry.block();
  };
  REQUIRE(fixture.until(
    [&] {
      return !executor.records.empty() && executor.records.front()->completedPublications == 1;
    },
    true));
  fixture.pump(true);
  CHECK(fixture.probe->projections == 0);
  reservation.release();
  REQUIRE(fixture.until(
    [&] {
      return retry.ready.wait_for(0ms) == std::future_status::ready;
    },
    true));
  fixture.pump(true);
  fixture.pump(true);
  const auto runningFrames = fixture.preparedFrames;
  for (unsigned turn = 0; turn < 20; ++turn) {
    fixture.pump(true);
  }
  CHECK(fixture.preparedFrames == runningFrames);
  retry.release();
  REQUIRE(fixture.until(
    [&] {
      return fixture.count() == 1;
    },
    true));
  CHECK(fixture.probe->projections == 1);
}

TEST_CASE("publication refusal retries the prepared result without repeating index or query execution") {
  GateExecutor executor;
  Fixture fixture({"alpha", "beta", "gamma"}, "alpha", {}, &executor);
  REQUIRE(fixture.until([&] {
    return fixture.count() == 1;
  }));
  executor.signal->holdPublications = true;
  fixture.query("gamma");
  REQUIRE(fixture.until([&] {
    return executor.records.size() == 3 && executor.records.back()->publications >= 2;
  }));
  CHECK(executor.records.back()->runs == 1);
  CHECK(fixture.probe->projections == 3);
  executor.signal->holdPublications = false;
  executor.release();
  REQUIRE(fixture.until([&] {
    return fixture.count() == 1;
  }));
  fixture.activate({3, 1});
  REQUIRE(fixture.selected.size() == 1);
  CHECK(fixture.selected.front().text() == "gamma");
  CHECK(executor.records.back()->runs == 1);
}

TEST_CASE("terminal executor rejection preserves exact cause once and a new query retries without stale admission") {
  for (const auto error : {BackgroundError::CLOSED, BackgroundError::START_FAILED, BackgroundError::NO_MEMORY,
                           BackgroundError::UNAVAILABLE, BackgroundError::INVALID_ARGUMENT}) {
    GateExecutor executor;
    executor.rejected = error;
    Fixture fixture({"alpha", "beta"}, "alpha", {}, &executor);
    fixture.pump();
    auto diagnostic = take(fixture.scene->diagnostics());
    REQUIRE(diagnostic.errors.size() == 1);
    CHECK(diagnostic.errors.front().error.background == error);
    CHECK(std::string_view(diagnostic.errors.front().error.operation) == "ui::suggestions/background");
    CHECK(fixture.errors.empty());
    const auto submissions = executor.submissions;
    for (unsigned frame = 0; frame < 10; ++frame) {
      fixture.pump();
    }
    CHECK(executor.submissions == submissions);
    CHECK(take(fixture.scene->diagnostics()).errors.size() == 1);
    CHECK(fixture.probe->projections == 0);
    CHECK(take(fixture.input.text().get()) == "alpha");
    executor.rejected.reset();
    fixture.query("beta");
    REQUIRE(fixture.until([&] {
      return fixture.count() == 1;
    }));
    fixture.activate({2, 1});
    REQUIRE(fixture.selected.size() == 1);
    CHECK(fixture.selected.back().text() == "beta");
    CHECK(fixture.probe->projections == 2);
    CHECK(take(fixture.scene->diagnostics()).errors.size() == 1);
  }
}

TEST_CASE("clearing the newest query fences a retained old query before capacity retry or its terminal cause") {
  GateExecutor executor;
  Fixture fixture({"alpha", "beta"}, "alpha", {}, &executor);
  REQUIRE(fixture.until([&] {
    return fixture.count() == 1;
  }));
  executor.full = true;
  fixture.query("beta");
  const auto submissions = executor.submissions;
  executor.rejected = BackgroundError::CLOSED;
  executor.release();
  take((fixture.input.text() = "").result());
  fixture.pump();
  CHECK(executor.submissions == submissions);
  CHECK(take(fixture.scene->diagnostics()).errors.empty());
  REQUIRE(fixture.until([&] {
    return fixture.count() == 0;
  }));
  executor.rejected.reset();
  fixture.query("beta");
  REQUIRE(fixture.until([&] {
    return fixture.count() == 1;
  }));
  CHECK(fixture.probe->projections == 2);
  fixture.activate({2, 1});
  REQUIRE(fixture.selected.size() == 1);
  CHECK(fixture.selected.back().text() == "beta");
}

TEST_CASE("terminal worker publication reports its exact async cause while the live list remains readable") {
  Fixture fixture({"alpha"}, "alpha");
  std::promise<void> entered;
  auto ready = entered.get_future();
  std::latch release{1};
  fixture.probe->projected = [&] {
    entered.set_value();
    release.wait();
  };

  struct Release {
    std::latch &latch;
    bool done{};

    void now() {
      if (!done) {
        latch.count_down();
        done = true;
      }
    }

    ~Release() {
      now();
    }
  } cleanup{release};

  fixture.pump();
  REQUIRE(ready.wait_for(5s) == std::future_status::ready);
  take(fixture.app->scheduler().stop());
  cleanup.now();
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  bool reported{};
  do {
    take(fixture.scene->prepare(*fixture.app, {70, 12}));
    auto diagnostic = take(fixture.scene->diagnostics());
    if (!diagnostic.errors.empty()) {
      CHECK(diagnostic.errors.front().error.async == AsyncError::CLOSED);
      CHECK(std::string_view(diagnostic.errors.front().error.operation) == "ui::suggestions/publication");
      CHECK_FALSE(diagnostic.errors.front().error.background);
      reported = true;
      break;
    }
    std::this_thread::sleep_for(1ms);
  } while (std::chrono::steady_clock::now() < deadline);
  REQUIRE(reported);
  CHECK(fixture.probe->projections == 1);
  CHECK(take(fixture.input.text().get()) == "alpha");
}

TEST_CASE("unsupported host fails before projection and newest domain failure preserves draft") {
  class Unsupported final : public BackgroundExecutor {
  public:
    std::expected<BackgroundTicket, BackgroundError> submit(std::unique_ptr<BackgroundJob> &) override {
      return std::unexpected(BackgroundError::UNAVAILABLE);
    }

    void service() noexcept override {
    }

    std::uint64_t capacityEpoch() const noexcept override {
      return 0;
    }
  } executor;

  Fixture fixture({"alpha"}, "alpha", {}, &executor);
  fixture.pump();
  REQUIRE(fixture.errors.size() == 1);
  CHECK(fixture.errors.front() == SearchError::UNAVAILABLE);
  CHECK(fixture.probe->projections == 0);
  CHECK(take(fixture.input.text().get()) == "alpha");
  for (unsigned frame = 0; frame < 10; ++frame) {
    fixture.pump();
  }
  CHECK(fixture.errors.size() == 1);
}

TEST_CASE("source replacement invalidates old selections before a new worker result") {
  Fixture fixture;
  REQUIRE(fixture.until([&] {
    return fixture.count() == 2;
  }));
  fixture.activate({1, 1});
  REQUIRE(fixture.selected.size() == 1);
  auto old = fixture.selected.front();
  auto publisher = take(fixture.model.publisher());
  take(publisher.publish(snapshot({"open replacement"}, fixture.probe, 2)));
  fixture.pump();
  CHECK_FALSE(take(old.current()));
  REQUIRE(fixture.until([&] {
    return fixture.count() == 1 && fixture.probe->projections == 4;
  }));
  fixture.activate({1, 1});
  REQUIRE(fixture.selected.size() == 2);
  CHECK(fixture.selected.back().text() == "open replacement");
}

TEST_CASE("typing bursts cannot cancel or restart the independent source index build") {
  Fixture fixture({"open file", "open recent file", "other"});
  std::promise<void> entered;
  auto enteredFuture = entered.get_future();
  std::latch release{1};
  std::atomic<bool> first{true};
  fixture.probe->projected = [&] {
    if (first.exchange(false)) {
      entered.set_value();
      release.wait();
    }
  };

  struct Release {
    std::latch &latch;
    bool released{};

    void now() {
      if (!released) {
        latch.count_down();
        released = true;
      }
    }

    ~Release() {
      now();
    }
  } cleanup{release};

  fixture.pump();
  REQUIRE(enteredFuture.wait_for(5s) == std::future_status::ready);
  for (std::string_view query : {"o", "op", "ope", "open", "oth", "othe", "other"}) {
    fixture.query(query);
  }
  CHECK(fixture.probe->projections == 1);
  cleanup.now();
  REQUIRE(fixture.until([&] {
    return fixture.count() == 1 && fixture.probe->projections == 3;
  }));
  fixture.activate({3, 1});
  REQUIRE(fixture.selected.size() == 1);
  CHECK(fixture.selected.front().text() == "other");
}

TEST_CASE("deferred editor draft and committed sources preserve their explicit publication boundary and Undo") {
  ui::LineEditConfig editor;
  editor.options.editMode = EditMode::ON_COMPLETION;
  Fixture draft({"alpha", "beta"}, "alpha", {}, nullptr, editor);
  Fixture committed({"alpha", "beta"}, "alpha", {}, nullptr, editor, true);
  REQUIRE(draft.until([&] {
    return draft.count() == 1;
  }));
  REQUIRE(committed.until([&] {
    return committed.count() == 1;
  }));
  draft.query("beta");
  committed.query("beta");
  REQUIRE(draft.until([&] {
    return draft.count() == 1;
  }));
  draft.activate({2, 1});
  committed.activate({1, 1});
  REQUIRE(draft.selected.size() == 1);
  REQUIRE(committed.selected.size() == 1);
  CHECK(draft.selected.back().text() == "beta");
  CHECK(committed.selected.back().text() == "alpha");
  const auto previousResult = committed.resultRevision();
  take(committed.app->router().invoke(committed.input.node(), {Action::ACTIVATE}));
  REQUIRE(committed.until([&] {
    return committed.count() == 1 && committed.resultRevision() > previousResult;
  }));
  committed.activate({2, 1});
  REQUIRE(committed.selected.size() == 2);
  CHECK(committed.selected.back().text() == "beta");
  CHECK(take(draft.input.text().get()) == "beta");
  take(draft.input.undo().result());
  REQUIRE(draft.until([&] {
    return draft.count() == 1;
  }));
  CHECK(take(draft.input.text().get()) == "alpha");
  CHECK(draft.probe->projections == 2);
  CHECK(committed.probe->projections == 2);
}

TEST_CASE("queued activation fences a changed query and a published source pin") {
  Fixture fixture;
  REQUIRE(fixture.until([&] {
    return fixture.count() == 2;
  }));
  take(setVirtualListSelection(fixture.app->tree(), fixture.list.node(), RowId{1, 1}));
  take(fixture.app->router().invoke(fixture.list.node(), {Action::ACTIVATE}));
  take((fixture.input.text() = "other").result());
  fixture.pump();
  CHECK(fixture.selected.empty());
  REQUIRE(fixture.until([&] {
    return fixture.count() == 1;
  }));
  fixture.activate({3, 1});
  REQUIRE(fixture.selected.size() == 1);
  auto old = fixture.selected.back();
  take(setVirtualListSelection(fixture.app->tree(), fixture.list.node(), RowId{3, 1}));
  take(fixture.app->router().invoke(fixture.list.node(), {Action::ACTIVATE}));
  auto publisher = take(fixture.model.publisher());
  take(publisher.publish(snapshot({"other replacement"}, fixture.probe, 2)));
  fixture.pump();
  CHECK_FALSE(take(old.current()));
  CHECK(fixture.selected.size() == 1);
  REQUIRE(fixture.until([&] {
    return fixture.count() == 1 && fixture.probe->projections == 4;
  }));
  fixture.activate({1, 1});
  REQUIRE(fixture.selected.size() == 2);
  CHECK(fixture.selected.back().text() == "other replacement");
  CHECK_FALSE(old.sourceIdentity() == fixture.selected.back().sourceIdentity());
}

TEST_CASE("equal domain revisions cannot alias separate source pins or projection sessions") {
  auto probe = std::make_shared<Probe>();
  auto first = snapshot({"alpha"}, probe, 1);
  auto second = snapshot({"beta"}, probe, 1);
  SearchSourceIdentity left{first, first.get(), 1, 1, 1, {}};
  SearchSourceIdentity right{second, second.get(), 1, 1, 1, {}};
  CHECK_FALSE(left == right);
  right = left;
  CHECK(left == right);
  ++right.session;
  CHECK_FALSE(left == right);
  right = left;
  ++right.normalizationVersion;
  CHECK_FALSE(left == right);
}

TEST_CASE("UI callbacks report exceptions and can remove their own list without worker UI ownership") {
  ui::SuggestionsConfig throwing;
  throwing.activated = [](SearchSelection) {
    throw std::runtime_error("selection failure");
  };
  Fixture failed({"alpha"}, "alpha", throwing);
  REQUIRE(failed.until([&] {
    return failed.count() == 1;
  }));
  failed.activate({1, 1});
  auto diagnostics = take(failed.scene->diagnostics());
  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(std::get<TreeError>(diagnostics.errors.front().error.cause) == TreeError::CALLBACK_FAILED);
  Fixture *owner{};
  ui::SuggestionsConfig removing;
  removing.activated = [&](SearchSelection) {
    take(owner->scene->remove(owner->list.node()));
  };
  Fixture removed({"alpha"}, "alpha", removing);
  owner = &removed;
  REQUIRE(removed.until([&] {
    return removed.count() == 1;
  }));
  removed.activate({1, 1});
  REQUIRE(removed.selected.size() == 1);
  CHECK_FALSE(removed.selected.back().current());
  CHECK(removed.selected.back().text() == "alpha");
}

TEST_CASE("bounded indexed workloads report worker/index/query figures separately from UI frames" * doctest::skip()) {
  for (const std::size_t size : {1000u, 10000u, 100000u}) {
    std::vector<std::string> labels;
    labels.reserve(size);
    for (std::size_t row = 0; row < size; ++row) {
      labels.push_back("command " + std::to_string(row));
    }
    auto probe = std::make_shared<Probe>();
    auto source = workerSource(snapshot(labels, probe), probe);
    worker([&] {
      const auto begin = std::chrono::steady_clock::now();
      auto index = take(build(source));
      const auto built = std::chrono::steady_clock::now();
      std::size_t steps{};
      for (std::string_view query : {"command", "commnd", "999", "zzzz"}) {
        auto result = take(portals::detail::querySearchIndex(*index, query, 1, {}));
        steps += result->querySteps;
        CHECK(result->rows.size() <= 32);
      }
      const auto queried = std::chrono::steady_clock::now();
      std::cout << "{\"rows\":" << size << ",\"index_bytes\":" << index->bytes()
                << ",\"index_peak_bytes\":" << index->memory.peak() + sizeof(*index) << ",\"build_worker_us\":"
                << std::chrono::duration_cast<std::chrono::microseconds>(built - begin).count()
                << ",\"four_queries_worker_us\":"
                << std::chrono::duration_cast<std::chrono::microseconds>(queried - built).count()
                << ",\"query_steps\":" << steps << ",\"ui_frame_time\":null}\n";
    });
    CHECK(probe->projections == size);
    Fixture fixture(labels, "command");
    REQUIRE(fixture.until([&] {
      return fixture.count() == 32;
    }));
    std::vector<std::int64_t> frames(0);
    for (unsigned sample = 0; sample < 32; ++sample) {
      const auto begin = std::chrono::steady_clock::now();
      fixture.pump();
      frames.push_back(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count());
    }
    std::ranges::sort(frames);
    const auto queryBegin = std::chrono::steady_clock::now();
    fixture.query("zzzz");
    REQUIRE(fixture.until([&] {
      return take(fixture.status.text().get()) == "No matches";
    }));
    const auto queryEnd = std::chrono::steady_clock::now();
    labels.front() = "replacement command";
    auto replacement = snapshot(labels, fixture.probe, 2);
    auto publisher = take(fixture.model.publisher());
    const auto replacementBegin = std::chrono::steady_clock::now();
    take(publisher.publish(std::move(replacement)));
    fixture.query("replacement");
    const auto replacementPrepared = std::chrono::steady_clock::now();
    REQUIRE(fixture.until([&] {
      return fixture.count() == 1;
    }));
    const auto replacementInstalled = std::chrono::steady_clock::now();
    const auto closeBegin = std::chrono::steady_clock::now();
    take(fixture.scene->close());
    const auto closeEnd = std::chrono::steady_clock::now();
    const auto joinBegin = std::chrono::steady_clock::now();
    fixture.pool->requestStop();
    fixture.pool->join();
    const auto joined = std::chrono::steady_clock::now();
    const auto micros = [](auto duration) {
      return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    };
    std::cout << "{\"rows\":" << size << ",\"ui_pipeline\":\"scheduler_prepare_layout_views_no_terminal_paint\""
              << ",\"steady_prepare_p50_us\":" << frames[16] << ",\"steady_prepare_p95_us\":" << frames[30]
              << ",\"steady_prepare_max_us\":" << frames.back()
              << ",\"programmatic_draft_to_install_us\":" << micros(queryEnd - queryBegin)
              << ",\"replacement_publish_first_prepare_us\":" << micros(replacementPrepared - replacementBegin)
              << ",\"replacement_to_install_us\":" << micros(replacementInstalled - replacementBegin)
              << ",\"scene_close_us\":" << micros(closeEnd - closeBegin)
              << ",\"post_ui_worker_join_us\":" << micros(joined - joinBegin) << "}\n";
  }
}
} // namespace
