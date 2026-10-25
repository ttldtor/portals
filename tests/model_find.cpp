// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/background.hpp>
#include <portals/detail/find_search.hpp>
#include <portals/ui.hpp>
#include <portals/ui_model_find.hpp>

#include <atomic>
#include <thread>

namespace {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> result) {
  REQUIRE(result);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*result);
  }
}

struct Probe {
  std::size_t projections{};
  std::thread::id worker;
  std::size_t reads{};
  bool throws{};
};

class Rows final : public ModelSnapshot<std::string> {
public:
  Rows(std::uint64_t revision, std::vector<ModelRow<std::string>> rows, std::shared_ptr<Probe> probe)
      : ModelSnapshot(revision, rows.size()), rows_(std::move(rows)), probe_(std::move(probe)) {
  }

  ModelRows<std::string> read(RowRange range) const override {
    ++probe_->reads;

    return {state,
            state == RangeState::READY ? std::span(rows_).subspan(static_cast<std::size_t>(range.first), range.count)
                                       : std::span<const ModelRow<std::string>>{},
            {}};
  }

  RangeState state{RangeState::READY};

private:
  std::vector<ModelRow<std::string>> rows_;
  std::shared_ptr<Probe> probe_;
};

std::shared_ptr<const ModelSnapshot<std::string>> source(std::initializer_list<std::string_view> values,
                                                         std::shared_ptr<Probe> probe, std::uint64_t revision = 1) {
  std::vector<ModelRow<std::string>> rows(0);

  for (auto text : values) {
    rows.push_back({{rows.size() + 1, 1}, std::string(text)});
  }

  return std::make_shared<const Rows>(revision, std::move(rows), std::move(probe));
}

auto projection(std::shared_ptr<Probe> probe) {
  return [probe](const std::string &text) -> std::string_view {
    ++probe->projections;
    probe->worker = std::this_thread::get_id();

    if (probe->throws) {
      throw std::runtime_error("projection failure");
    }

    return text;
  };
}

// Deterministic host executor: run and publication are separate, allowing real out-of-order completion.
class Executor final : public BackgroundExecutor {
public:
  struct Item {
    std::shared_ptr<BackgroundJob> job;
    std::size_t runs{};
    std::size_t publications{};
  };

  std::expected<BackgroundTicket, BackgroundError> submit(std::unique_ptr<BackgroundJob> &job) override {
    ++submissions;

    if (terminalFailure) {
      rejectedToken = job->stopToken();

      return std::unexpected(*terminalFailure);
    }

    if (full) {
      rejectedToken = job->stopToken();

      if (std::exchange(releaseDuringReject, false)) {
        release();
      }

      return std::unexpected(BackgroundError::WOULD_BLOCK);
    }

    auto owner = std::shared_ptr<BackgroundJob>(std::move(job));

    jobs.push_back({owner});

    return BackgroundTicket(owner);
  }

  void service() noexcept override {
  }

  std::uint64_t capacityEpoch() const noexcept override {
    return epoch;
  }

  void release() {
    full = false;
    ++epoch;
  }

  void run(std::size_t index) {
    auto &item = jobs.at(index);

    ++item.runs;

    std::jthread worker([job = item.job] {
      job->run(job->stopToken());
    });
  }

  std::expected<void, AsyncError> publish(std::size_t index) {
    auto &item = jobs.at(index);

    ++item.publications;

    return item.job->publish();
  }

  std::vector<Item> jobs = std::vector<Item>(0);
  std::size_t submissions{};
  std::uint64_t epoch{};
  bool full{};
  bool releaseDuringReject{};
  std::optional<BackgroundError> terminalFailure;
  std::stop_token rejectedToken;
};

struct Fixture {
  Executor executor;
  std::unique_ptr<Application> app;
  std::unique_ptr<ManagedScene> scene;
  ManagedModel<std::string> model;
  ui::Ref<FindBar> find;
  std::vector<FindMatch> selected = std::vector<FindMatch>(0);
  std::vector<FindSearchError> failures = std::vector<FindSearchError>(0);
  std::shared_ptr<Probe> probe = std::make_shared<Probe>();

  explicit Fixture(std::initializer_list<std::string_view> values = {"aaaa", "e\u0301 e 界 👩‍💻 👩"},
                   std::string_view query = "a", ui::FindBarConfig config = {}, std::size_t queue = 8) {
    ApplicationOptions options;

    options.backgroundExecutor = &executor;
    options.scheduler.data_capacity = queue;
    app = take(Application::create(options));
    scene = take(ManagedScene::create(*app));
    model = take(scene->model(source(values, probe)));
    config.selected = [this, callback = std::move(config.selected)](FindMatch match) {
      selected.push_back(match);

      if (callback) {
        callback(std::move(match));
      }
    };
    config.searchFailed = [this, callback = std::move(config.searchFailed)](FindSearchError failure) {
      failures.push_back(failure);

      if (callback) {
        callback(failure);
      }
    };

    take(ui::mount(*scene, ui::findBar(find, model, projection(probe), query, std::move(config))));
    prepare();
  }

  void prepare() {
    take(scene->prepare(*app, {60, 10}));
  }

  void dispatch() {
    take(app->scheduler().dispatch());
    take(scene->afterDispatch(*app));
  }

  void complete(std::size_t index) {
    executor.run(index);
    take(executor.publish(index));
    dispatch();
    prepare();
  }

  void query(std::string_view value) {
    take((find.text() = value).result());
    prepare();
  }

  FindMatch match() {
    return take(find.match());
  }

  std::string summary() {
    const auto children = take(app->tree().inspect(find.node())).children;
    std::string value;

    take(app->tree().read(children.back(), [&](const Widget &widget) {
      value = dynamic_cast<const Label &>(widget).text().bytes();
    }));

    return value;
  }
};

TEST_CASE("model FindBar scans on a worker and owns multiple matches in each source row") {
  Fixture f;

  CHECK(f.probe->projections == 0);
  CHECK_FALSE(f.match());
  REQUIRE(f.executor.jobs.size() == 1);
  f.complete(0);

  auto first = f.match();

  REQUIRE(first);
  CHECK(first.storedCount() == 4);
  CHECK(first.rowId() == RowId{1, 1});
  CHECK(first.sourceRevision() == 1);
  CHECK(first.queryGeneration() != 0);
  CHECK(f.probe->worker != std::this_thread::get_id());
  CHECK(f.selected.size() == 1);
  CHECK(take(f.find.current(first)));

  CHECK(f.find.previous());
  CHECK(f.match().begin() == 3);
  CHECK(f.find.next());
  CHECK(f.match().begin() == 0);

  const auto reads = f.probe->reads;
  f.query("aa");

  CHECK(f.probe->reads == reads);
  CHECK_FALSE(take(f.find.current(first)));
  CHECK(first.text() == "a");
  f.complete(1);
  CHECK(f.match().storedCount() == 3);
  CHECK_FALSE(f.find.replaceDocument("other"));

  f.query("e");
  f.complete(2);
  CHECK(f.match().storedCount() == 1);
  CHECK(f.match().rowId() == RowId{2, 1});
  CHECK(f.match().begin() == std::string("e\u0301 ").size());

  f.query("👩");
  f.complete(3);
  CHECK(f.match().storedCount() == 1);
  CHECK(f.match().context().find(f.match().text()) != std::string_view::npos);
  CHECK(GraphemeText::from(f.match().context()));
}

TEST_CASE("query replacement cancels immediately and retains only the newest full-admission intent") {
  Fixture f({"alpha beta gamma"}, "alpha");

  f.complete(0);
  auto old = f.match();

  f.executor.full = true;
  f.query("beta");
  CHECK_FALSE(take(f.find.current(old)));
  CHECK(f.executor.jobs.front().job->stopToken().stop_requested());
  CHECK(f.executor.jobs.size() == 1);

  f.query("gamma");
  const auto submissions = f.executor.submissions;

  f.dispatch();
  f.dispatch();
  CHECK(f.executor.submissions == submissions);
  f.executor.release();
  f.dispatch();
  REQUIRE(f.executor.jobs.size() == 2);
  f.complete(1);
  CHECK(f.match().text() == "gamma");
  CHECK(f.probe->projections == 2);
}

TEST_CASE("a stale completion cannot beat a new query or a source pin in the same UI dispatch") {
  Fixture f({"alpha beta"}, "alpha");

  f.executor.run(0);
  f.query("beta");
  take(f.executor.publish(0));
  f.dispatch();
  f.prepare();
  CHECK_FALSE(f.match());
  CHECK(f.selected.empty());
  f.complete(1);
  auto old = f.match();

  f.query("alpha");
  f.executor.run(2);
  auto publisher = take(f.model.publisher());
  auto updated = source({"alpha alpha"}, f.probe, 2);

  take(publisher.publish(updated));
  take(f.executor.publish(2));
  f.dispatch();
  CHECK(f.selected.size() == 1);
  f.prepare();
  CHECK_FALSE(f.match());
  CHECK_FALSE(take(f.find.current(old)));
  REQUIRE(f.executor.jobs.size() == 4);
  f.complete(3);
  CHECK(f.match().sourceRevision() == 2);
  CHECK(f.match().storedCount() == 2);
  CHECK(old.text() == "beta");
}

TEST_CASE("a later stale callback cannot overwrite the newest completion waiting for the shared pin") {
  Fixture f({"alpha beta"}, "alpha");

  f.executor.run(0);
  f.query("beta");
  f.executor.run(1);
  take(f.executor.publish(1));
  take(f.executor.publish(0));
  f.dispatch();
  CHECK(f.selected.empty());
  f.prepare();
  REQUIRE(f.match());
  CHECK(f.match().text() == "beta");
  CHECK(f.selected.size() == 1);
}

TEST_CASE("a new success followed by an old domain error keeps the newest match and no provider failure") {
  Fixture f({"alpha beta"}, "alpha");

  f.probe->throws = true;
  f.executor.run(0);
  f.probe->throws = false;
  f.query("beta");
  f.executor.run(1);
  take(f.executor.publish(1));
  take(f.executor.publish(0));
  f.dispatch();
  f.prepare();
  REQUIRE(f.match());
  CHECK(f.match().text() == "beta");
  CHECK(f.selected.size() == 1);
  CHECK(f.failures.empty());
}

TEST_CASE("terminal admission preserves exact background error and retires the pending intent without retry") {
  for (const auto error : {BackgroundError::CLOSED, BackgroundError::NO_MEMORY}) {
    Fixture f({"alpha beta"}, "alpha");

    f.complete(0);
    auto old = f.match();

    f.executor.full = true;
    f.query("beta");
    CHECK(f.executor.rejectedToken.stop_possible());
    f.executor.terminalFailure = error;
    f.executor.release();
    f.dispatch();

    auto diagnostics = take(f.scene->diagnostics());

    REQUIRE(diagnostics.errors.size() == 1);
    const auto &failure = diagnostics.errors.front().error;

    CHECK(failure.background == error);
    CHECK(std::get<TreeError>(failure.cause) ==
          (error == BackgroundError::CLOSED ? TreeError::CLOSED : TreeError::NO_MEMORY));
    REQUIRE(failure.operation);
    CHECK(std::string_view(failure.operation) == "ui::findBar/background");
    CHECK_FALSE(take(f.find.current(old)));
    CHECK(f.summary() == "Search failed");
    CHECK(f.failures.empty());
    CHECK_FALSE(f.executor.rejectedToken.stop_possible());

    const auto submissions = f.executor.submissions;

    f.executor.terminalFailure.reset();
    f.executor.release();
    f.dispatch();
    f.prepare();
    f.dispatch();
    CHECK(f.executor.submissions == submissions);
    CHECK(f.executor.jobs.size() == 1);
    CHECK(f.summary() == "Search failed");

    if (error == BackgroundError::CLOSED) {
      f.executor.terminalFailure = error;
    }

    f.query("alpha");
    CHECK(take(f.find.text().get()) == "alpha");

    if (error == BackgroundError::NO_MEMORY) {
      REQUIRE(f.executor.jobs.size() == 2);
      f.complete(1);
      CHECK(f.match().text() == "alpha");
      CHECK(take(f.scene->diagnostics()).errors.size() == 1);
    } else {
      CHECK(f.executor.submissions == submissions + 1);
      CHECK(f.executor.jobs.size() == 1);
      CHECK(f.summary() == "Search failed");

      auto repeated = take(f.scene->diagnostics());

      REQUIRE(repeated.errors.size() == 2);
      CHECK(repeated.errors.back().error.background == BackgroundError::CLOSED);
    }
  }
}

TEST_CASE("terminal admission ends pending presentation before observer removal or a guarded close request") {
  for (const bool close : {false, true}) {
    Fixture f({"alpha beta"}, "alpha");
    bool observed{};

    take(f.scene->onError([&](const ManagedDiagnostic &diagnostic) {
      observed = true;
      CHECK(diagnostic.error.background == BackgroundError::CLOSED);
      CHECK(f.summary() == "Search failed");
      CHECK_FALSE(f.executor.rejectedToken.stop_possible());

      if (close) {
        auto closing = f.scene->close();

        REQUIRE_FALSE(closing);
        CHECK(std::get<TreeError>(closing.error().cause) == TreeError::WOULD_BLOCK);
      } else {
        take(f.app->tree().remove(f.find.node()));
      }
    }));
    f.executor.full = true;
    f.query("beta");
    f.executor.terminalFailure = BackgroundError::CLOSED;
    f.executor.release();
    f.dispatch();
    CHECK(observed);

    if (close) {
      take(f.scene->close());
    }

    CHECK_FALSE(f.find.match());

    const auto submissions = f.executor.submissions;

    f.executor.terminalFailure.reset();
    f.executor.release();
    f.dispatch();
    CHECK(f.executor.submissions == submissions);
  }
}

TEST_CASE("capacity released during rejected admission is retried without another query or busy loop") {
  Fixture f({"alpha beta"}, "alpha");

  f.executor.full = true;
  f.executor.releaseDuringReject = true;
  f.query("beta");
  REQUIRE(f.executor.jobs.size() == 1);
  f.dispatch();
  REQUIRE(f.executor.jobs.size() == 2);
  f.complete(1);
  CHECK(f.match().text() == "beta");
}

TEST_CASE("full delivery queue retries one prepared result without repeating provider reads") {
  Fixture f({"aaaa"}, "a", {}, 1);
  auto scope = take(f.app->scheduler().makeScope());
  auto occupied = take(f.app->scheduler().connect<int>(scope, [](auto) {
  }));

  take(occupied.publisher.publish(std::make_shared<const int>(1)));
  f.executor.run(0);
  const auto reads = f.probe->reads;
  auto blocked = f.executor.publish(0);

  REQUIRE_FALSE(blocked);
  CHECK(blocked.error() == AsyncError::WOULD_BLOCK);
  CHECK_FALSE(f.match());
  f.dispatch();
  take(f.executor.publish(0));
  f.dispatch();
  f.prepare();
  CHECK(f.match().storedCount() == 4);
  CHECK(f.executor.jobs[0].runs == 1);
  CHECK(f.executor.jobs[0].publications == 2);
  CHECK(f.probe->reads == reads);
}

TEST_CASE("domain failures and truncation are explicit and do not corrupt ordinary query state") {
  ui::FindBarConfig config;

  config.search.maxMatches = 2;
  Fixture f({"aaaa"}, "a", config);

  f.complete(0);
  CHECK(f.match().storedCount() == 2);
  CHECK(f.match().truncated());
  CHECK(f.find.previous());
  CHECK(f.match().begin() == 1);

  f.probe->throws = true;
  f.query("b");
  f.complete(1);
  CHECK_FALSE(f.match());
  REQUIRE(f.failures.size() == 1);
  CHECK(f.failures.front() == FindSearchError::CALLBACK_FAILED);
  CHECK(take(f.find.text().get()) == "b");

  f.query("");
  CHECK_FALSE(f.match());
  CHECK(f.executor.jobs.size() == 2);
  CHECK(f.executor.jobs.back().job->stopToken().stop_requested());

  ui::FindBarConfig tiny;

  tiny.search.maxSourceRows = 1;
  Fixture over({"a", "a"}, "a", tiny);

  over.complete(0);
  REQUIRE(over.failures.size() == 1);
  CHECK(over.failures.front() == FindSearchError::LIMIT_EXCEEDED);
  CHECK(over.probe->reads == 0);
}

TEST_CASE("late worker results cannot call removed bars and old ranges retain their context") {
  Fixture f({"old alpha context"}, "alpha");

  f.complete(0);
  auto old = f.match();

  f.query("context");
  f.executor.run(1);
  const auto selected = f.selected.size();

  take(f.scene->remove(f.find.node()));
  auto late = f.executor.publish(1);

  CHECK_FALSE(late);
  f.dispatch();
  CHECK(f.selected.size() == selected);
  CHECK(old.text() == "alpha");
  CHECK(old.context() == "old alpha context");

  Fixture closed;

  closed.executor.run(0);
  take(closed.scene->close());
  CHECK_FALSE(closed.executor.publish(0));
  CHECK(closed.selected.empty());
}

TEST_CASE("ordinary deferred drafts publish only at their editor boundary and Clear cancels search") {
  ui::FindBarConfig config;

  config.editor.options.editMode = EditMode::ON_COMPLETION;
  Fixture f({"alpha beta"}, "alpha", config);

  f.complete(0);
  const auto old = f.match();
  take((f.find.text() = "beta").result());
  f.prepare();

  // A programmatic write is still an ordinary deferred draft, with no alternate query store.
  CHECK(take(f.find.text().get()) == "beta");
  CHECK(f.executor.jobs.size() == 1);
  CHECK(take(f.find.current(old)));
  take(f.app->router().invoke(f.find.editor().node(), {portals::Action::ACTIVATE}));
  f.prepare();
  REQUIRE(f.executor.jobs.size() == 2);
  CHECK_FALSE(take(f.find.current(old)));
  f.complete(1);
  take(f.find.clearQuery().result());
  f.prepare();
  CHECK(take(f.find.text().get()).empty());
  CHECK_FALSE(f.match());

  take(f.find.editor().undo().result());
  CHECK(take(f.find.text().get()) == "beta");
  CHECK(f.executor.jobs.size() == 2);
  take(f.app->router().invoke(f.find.editor().node(), {portals::Action::ACTIVATE}));
  f.prepare();
  REQUIRE(f.executor.jobs.size() == 3);
  f.complete(2);
  CHECK(f.match().text() == "beta");
}

TEST_CASE("row byte, cumulative byte and query caps report domain errors before unbounded copies") {
  ui::FindBarConfig config;

  config.search.maxProjectedBytes = 12;
  config.search.maxRowBytes = 8;
  config.search.maxQueryBytes = 2;
  Fixture exact({"aaaaa", "aaaaaaa"}, "z", config);

  exact.complete(0);
  CHECK(exact.failures.empty());
  CHECK_FALSE(exact.match());

  Fixture total({"aaaaaa", "aaaaaaa"}, "z", config);

  total.complete(0);
  REQUIRE(total.failures.size() == 1);
  CHECK(total.failures.front() == FindSearchError::LIMIT_EXCEEDED);

  Fixture row({"aaaaaaaaa"}, "z", config);

  row.complete(0);
  REQUIRE(row.failures.size() == 1);
  CHECK(row.failures.front() == FindSearchError::LIMIT_EXCEEDED);

  Fixture query({"a"}, "aaa", config);

  query.complete(0);
  REQUIRE(query.failures.size() == 1);
  CHECK(query.failures.front() == FindSearchError::LIMIT_EXCEEDED);
  CHECK(query.probe->reads == 0);

  Fixture invalid({"\xff"}, "a");

  invalid.complete(0);
  REQUIRE(invalid.failures.size() == 1);
  CHECK(invalid.failures.front() == FindSearchError::INVALID_TEXT);
}

TEST_CASE("model rows exceed the legacy UI document cap while remaining within worker byte budgets") {
  const std::string large = std::string(128 * 1024, 'x') + "needle";
  ui::FindBarConfig config;

  config.limits.maxDocumentBytes = FindLimits::MAX_DOCUMENT_BYTES + 1;
  Fixture f({large}, "needle", config);

  CHECK(f.probe->projections == 0);
  f.complete(0);
  REQUIRE(f.match());
  CHECK(f.match().begin() == 128 * 1024);
  CHECK(f.match().text() == "needle");
  CHECK(f.failures.empty());

  const std::string oversized(FindSearchOptions{}.maxRowBytes + 1, 'x');
  Fixture rejected({oversized}, "needle");

  rejected.complete(0);
  REQUIRE(rejected.failures.size() == 1);
  CHECK(rejected.failures.front() == FindSearchError::LIMIT_EXCEEDED);
}

TEST_CASE("owning snapshot overload creates the ordinary managed model without client async scaffolding") {
  Executor executor;
  ApplicationOptions options;

  options.backgroundExecutor = &executor;

  auto app = take(Application::create(options));
  auto scene = take(ManagedScene::create(*app));
  auto probe = std::make_shared<Probe>();
  ui::Ref<FindBar> find;

  take(ui::mount(*scene, ui::findBar(find, source({"aaaa"}, probe), projection(probe), "aa")));
  take(scene->prepare(*app, {50, 8}));
  REQUIRE(executor.jobs.size() == 1);
  executor.run(0);
  take(executor.publish(0));
  take(app->scheduler().dispatch());
  take(scene->prepare(*app, {50, 8}));
  CHECK(take(find.match()).storedCount() == 3);
  CHECK(probe->worker != std::this_thread::get_id());
}

TEST_CASE("checked collection source uses the existing snapshot adapter and preserves original mount errors") {
  Executor executor;
  ApplicationOptions options;

  options.backgroundExecutor = &executor;

  auto app = take(Application::create(options));
  auto scene = take(ManagedScene::create(*app));
  const ModelRow<std::string> rows[]{{{7, 2}, "aaaa"}};
  ui::Ref<FindBar> find;

  take(ui::mount(*scene, ui::findBar(
                           find, RowCollection<std::string>::from(rows),
                           [](const std::string &value) -> std::string_view {
                             return value;
                           },
                           "aa")));
  take(scene->prepare(*app, {50, 8}));
  REQUIRE(executor.jobs.size() == 1);
  executor.run(0);
  take(executor.publish(0));
  take(app->scheduler().dispatch());
  take(scene->prepare(*app, {50, 8}));
  CHECK(take(find.match()).storedCount() == 3);
  CHECK(take(find.match()).rowId() == RowId{7, 2});

  const ModelRow<std::string> duplicate[]{{{1, 1}, "a"}, {{1, 1}, "b"}};
  auto rejected = ui::mount(
    *scene, ui::findBar(RowCollection<std::string>::from(duplicate), [](const std::string &value) -> std::string_view {
      return value;
    }));

  REQUIRE_FALSE(rejected);
  CHECK(std::get<TreeError>(rejected.error().cause) == TreeError::INVALID_ARGUMENT);
}

TEST_CASE("selected callback failure preserves publication and self-removal retires late callbacks") {
  ui::FindBarConfig throwing;

  throwing.selected = [](FindMatch) {
    throw std::runtime_error("selected failure");
  };
  Fixture failed({"aaaa"}, "a", throwing);

  failed.complete(0);
  CHECK(failed.match().storedCount() == 4);

  auto diagnostics = take(failed.scene->diagnostics());

  REQUIRE(diagnostics.errors.size() == 1);
  CHECK(std::get<TreeError>(diagnostics.errors.front().error.cause) == TreeError::CALLBACK_FAILED);

  Fixture *removing{};
  ui::FindBarConfig self;

  self.selected = [&removing](FindMatch) {
    take(removing->scene->remove(removing->find.node()));
  };
  Fixture removed({"aaaa"}, "a", self);

  removing = &removed;
  removed.complete(0);
  CHECK(removed.selected.size() == 1);
  CHECK_FALSE(removed.find.match());
  CHECK(removed.executor.jobs.front().job->stopToken().stop_requested());
}

TEST_CASE("managed pin access remains UI-affine while owning snapshot reads survive scene close") {
  Fixture f({"alpha"}, "alpha");
  auto retained = take(portals::detail::ManagedModelAccess::snapshot(f.model, f.scene->root().id()));
  const auto sceneId = f.scene->root().id();
  const auto model = f.model;
  std::expected<std::shared_ptr<const ModelSnapshot<std::string>>, TreeError> forbidden;

  std::jthread worker([model, sceneId, &forbidden] {
    forbidden = portals::detail::ManagedModelAccess::snapshot(model, sceneId);
  });

  worker.join();
  REQUIRE_FALSE(forbidden);
  CHECK(forbidden.error() == TreeError::WRONG_THREAD);
  take(f.scene->close());

  auto range = take(ModelRange<std::string>::read(retained, {0, 1}, 1));

  CHECK(range.rows().front().value == "alpha");
}

TEST_CASE("default model search covers 100k rows and the public frame owner reads on a worker") {
  const FindSearchOptions options;

  CHECK(options.maxSourceRows == 100000);
  CHECK(options.maxProjectedBytes == 33554432);
  CHECK(options.maxRowBytes == 1048576);
  CHECK(options.maxQueryBytes == 16384);
  CHECK(options.maxMatches == 4096);

  auto probe = std::make_shared<Probe>();
  std::vector<ModelRow<std::string>> rows(0);

  rows.reserve(options.maxSourceRows);

  for (std::size_t index = 0; index < options.maxSourceRows; ++index) {
    rows.push_back({{index + 1, 1}, index + 1 == options.maxSourceRows ? "needle" : "hay"});
  }

  Fixture f({}, "needle");
  auto publisher = take(f.model.publisher());

  take(publisher.publish(std::make_shared<const Rows>(2, std::move(rows), probe)));
  f.dispatch();
  f.prepare();
  REQUIRE(f.executor.jobs.size() == 2);
  f.complete(1);
  REQUIRE(f.match());
  CHECK(f.match().rowId() == RowId{100000, 1});
  CHECK(f.match().text() == "needle");
  CHECK(f.probe->projections == 100000);
  CHECK(probe->reads == (100000 + 255) / 256);

  auto retained = take(portals::detail::ManagedModelAccess::snapshot(f.model, f.scene->root().id()));
  auto pin = retained;
  std::expected<ModelRange<std::string>, TreeError> range = std::unexpected(TreeError::INVALID_ARGUMENT);

  std::jthread worker([&] {
    range = ModelRange<std::string>::read(pin, {99999, 1}, 1);
  });

  worker.join();
  REQUIRE(range);
  CHECK(range->rows().front().value == "needle");
}
} // namespace
