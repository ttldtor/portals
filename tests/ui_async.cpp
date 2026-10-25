// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>
#include <portals/ui.hpp>
#include <portals/ui_async.hpp>

#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace portals;

template <class T, class E> T take(std::expected<T, E> value) {
  REQUIRE(value);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

struct Fixture {
  SchedulerTime now{};
  ManagedScene *managed{};
  Application *application{};
  ui::Ref<Button> button;
  ui::Scene scene;

  explicit Fixture(BackgroundExecutor *executor = nullptr)
      : scene(ui::scene(ui::vbox(ui::button(button, "Start")), {.application = {.backgroundExecutor = executor},
                                                                .clock = {[](void *value) noexcept {
                                                                            return *static_cast<SchedulerTime *>(value);
                                                                          },
                                                                          &now}})) {
    scene.configure([this](ManagedScene &owner, Application &app) {
      managed = &owner;
      application = &app;
    });
  }

  void prepare() {
    take(scene.prepareChecked());
  }

  void dispatch() {
    take(application->scheduler().dispatch());
    take(managed->afterDispatch(*application));
  }
};

UiTask countLater(ui::AsyncContext context, std::shared_ptr<int> count) {
  auto waited = co_await context.until(10);

  if (!waited) {
    co_return std::unexpected(waited.error());
  }

  ++*count;

  co_return {};
}

class TestExecutor final : public BackgroundExecutor {
public:
  bool ready{};
  bool throws{};
  std::optional<BackgroundError> failure;
  std::uint64_t epoch{};
  int attempts{};
  std::shared_ptr<BackgroundJob> job;

  std::expected<BackgroundTicket, BackgroundError> submit(std::unique_ptr<BackgroundJob> &incoming) override {
    ++attempts;

    if (!ready) {
      return std::unexpected(BackgroundError::WOULD_BLOCK);
    }

    if (throws) {
      throw std::runtime_error("host admission failed");
    }

    if (failure) {
      return std::unexpected(*failure);
    }

    job = std::move(incoming);

    return BackgroundTicket(job);
  }

  void service() noexcept override {
  }

  std::uint64_t capacityEpoch() const noexcept override {
    return epoch;
  }
};

struct LifetimeTrace {
  int next{};
  int frame{};
  int closure{};
  std::vector<int> destroyed;
  bool masked{true};
  bool restarted{};
  std::function<void()> restart;
};

struct TracedValue {
  std::shared_ptr<LifetimeTrace> trace;
  int id;
  bool closure{};

  TracedValue(std::shared_ptr<LifetimeTrace> value, bool capture)
      : trace(std::move(value)), id(++trace->next), closure(capture) {
  }

  TracedValue(const TracedValue &other) : trace(other.trace), id(++trace->next), closure(other.closure) {
  }

  ~TracedValue() {
    trace->destroyed.push_back(id);

    if (closure && id == trace->closure) {
      trace->masked = trace->masked && ui::detail::CurrentSceneScope::snapshot().expired();
    }

    if (!closure && id == trace->frame && trace->restart && !trace->restarted) {
      trace->restarted = true;
      trace->restart();
    }
  }
};

struct ThrowingCopy {
  std::shared_ptr<bool> fail;

  explicit ThrowingCopy(std::shared_ptr<bool> value) : fail(std::move(value)) {
  }

  ThrowingCopy(const ThrowingCopy &other) : fail(other.fail) {
    if (*fail) {
      throw std::bad_alloc();
    }
  }

  ThrowingCopy(ThrowingCopy &&) = default;

  UiTask operator()(ui::AsyncContext) {
    co_return {};
  }
};

struct PreparedProbe final : ui::detail::SceneActivity {
  Fixture &fixture;
  int services{};
  int preparations{};

  explicit PreparedProbe(Fixture &value) : fixture(value) {
  }

  bool active() const noexcept override {
    return true;
  }

  void service() noexcept override {
    ++services;
  }

  void prepared() noexcept override {
    ++preparations;

    const auto text = fixture.button.text().get();
    const auto prepare = fixture.managed->prepare(*fixture.application, {30, 4});
    const auto dispatch = fixture.managed->afterDispatch(*fixture.application);
    auto nested = ui::detail::SceneAccess::admitActivity(*fixture.managed, std::make_shared<PreparedProbe>(fixture));

    CHECK(text);
    CHECK_FALSE(prepare);
    CHECK_FALSE(dispatch);
    CHECK_FALSE(nested);

    if (text && !prepare && !dispatch && !nested) {
      CHECK(*text == "Start");
      CHECK(prepare.error() == TreeError::WOULD_BLOCK);
      CHECK(dispatch.error() == TreeError::WOULD_BLOCK);
      CHECK(std::get<TreeError>(nested.error().cause) == TreeError::WOULD_BLOCK);
    }
  }

  void cancel() noexcept override {
  }
};

class NativeProbeTerminal final : public ApplicationTerminal {
public:
  explicit NativeProbeTerminal(std::future<void> ready) : ready_(std::move(ready)) {
  }

  bool closed{};

  TerminalRead read(TerminalDeadline deadline, std::stop_token stop) override {
    if (stop.stop_requested()) {
      return {TerminalStatus::STOPPED};
    }

    if (!started_) {
      started_ = true;

      return {TerminalStatus::PROGRESS, "\x1bOQ"};
    }

    if (std::chrono::steady_clock::now() >= cutoff_) {
      return {TerminalStatus::END_OF_INPUT};
    }

    (void)ready_.wait_until(deadline);

    return {TerminalStatus::DEADLINE};
  }

  std::expected<CellSize, TerminalError> size() const override {
    return CellSize{40, 4};
  }

  std::expected<void, TerminalError> submit(std::string bytes) override {
    pending_ = bytes.size();

    return {};
  }

  TerminalWrite flush(TerminalDeadline, std::stop_token) override {
    return {TerminalStatus::PROGRESS, std::exchange(pending_, 0), 0, {}};
  }

  std::expected<void, TerminalError> close() noexcept override {
    closed = true;

    return {};
  }

private:
  std::future<void> ready_;
  const TerminalDeadline cutoff_ = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool started_{};
  std::size_t pending_{};
};
} // namespace

TEST_CASE("owning captured coroutine and free function resume through the existing scheduler") {
  Fixture fixture;
  auto calls = std::make_shared<int>();

  fixture.scene | ui::onClick(fixture.button, ui::async([calls](ui::AsyncContext context) -> UiTask {
                                auto waited = co_await context.until(10);

                                if (!waited) {
                                  co_return std::unexpected(waited.error());
                                }

                                ++*calls;

                                co_return {};
                              }));
  fixture.prepare();
  REQUIRE(fixture.managed->requiresBackgroundExecutor());
  take(fixture.button.activate().result());
  CHECK(*calls == 0);
  CHECK(ui::detail::CurrentSceneScope::snapshot().expired());
  fixture.now = 10;
  fixture.dispatch();
  CHECK(*calls == 1);
  CHECK(ui::detail::CurrentSceneScope::snapshot().expired());
}

TEST_CASE("default repeat preserves accepted work and reports exact recoverable admission") {
  Fixture fixture;
  auto calls = std::make_shared<int>();

  fixture.scene | ui::onClick(fixture.button, ui::async(countLater, calls));
  fixture.prepare();
  take(fixture.button.activate().result());
  // activate reports route admission; the rejected start is an exact recoverable diagnostic.
  take(fixture.button.activate().result());
  fixture.now = 10;
  fixture.dispatch();
  CHECK(*calls == 1);

  auto errors = take(fixture.managed->diagnostics());

  REQUIRE(errors.errors.size() == 1);
  CHECK(errors.errors[0].error.async == AsyncError::WOULD_BLOCK);
}

TEST_CASE("explicit replacement cancels the previous free function") {
  Fixture fixture;
  auto calls = std::make_shared<int>();

  fixture.scene | ui::onClick(fixture.button, ui::async(UiTaskRestart::CANCEL_PREVIOUS, countLater, calls));
  fixture.prepare();
  take(fixture.button.activate().result());
  take(fixture.button.activate().result());
  fixture.now = 10;
  fixture.dispatch();
  CHECK(*calls == 1);
  CHECK(take(fixture.managed->diagnostics()).errors.empty());
}

TEST_CASE("node removal suppresses suspended user code and coroutine errors are exact") {
  Fixture fixture;
  auto calls = std::make_shared<int>();

  fixture.scene | ui::onClick(fixture.button, ui::async(countLater, calls));
  fixture.prepare();
  take(fixture.button.activate().result());
  take(fixture.application->tree().remove(fixture.button.node()));
  fixture.now = 10;
  fixture.dispatch();
  CHECK(*calls == 0);
}

TEST_CASE("body exceptions enter the existing collector once") {
  Fixture fixture;

  fixture.scene | ui::onClick(fixture.button, ui::async([](ui::AsyncContext) -> UiTask {
                                throw std::runtime_error("body failed");

                                co_return {};
                              }));
  fixture.prepare();
  take(fixture.button.activate().result());

  auto errors = take(fixture.managed->diagnostics());

  REQUIRE(errors.errors.size() == 1);
  CHECK(errors.errors[0].error.async == AsyncError::CALLBACK_FAILED);
}

TEST_CASE("captured coroutine closure outlives complete frame destruction") {
  auto trace = std::make_shared<LifetimeTrace>();
  Fixture fixture;
  TracedValue capture(trace, true);
  TracedValue argument(trace, false);

  fixture.scene |
    ui::onClick(fixture.button, ui::async(
                                  [capture, trace](ui::AsyncContext context, TracedValue frame) -> UiTask {
                                    trace->frame = frame.id;
                                    trace->closure = capture.id;

                                    auto waited = co_await context.until(10);

                                    co_return waited;
                                  },
                                  argument));
  fixture.prepare();
  take(fixture.button.activate().result());

  SUBCASE("completion") {
    fixture.now = 10;
    fixture.dispatch();
  }

  SUBCASE("node removal") {
    take(fixture.application->tree().remove(fixture.button.node()));
  }

  SUBCASE("frame parameter destructor starts a new generation") {
    trace->restart = [&fixture] {
      take(fixture.button.activate().result());
    };
    fixture.now = 10;
    fixture.dispatch();
    fixture.dispatch();
    CHECK(trace->restarted);
    trace->restart = {};
  }

  const auto frame = std::ranges::find(trace->destroyed, trace->frame);
  const auto closure = std::ranges::find(trace->destroyed, trace->closure);

  REQUIRE(frame != trace->destroyed.end());
  REQUIRE(closure != trace->destroyed.end());
  CHECK(frame < closure);
  CHECK(trace->masked);
}

TEST_CASE("self-removal during resume defers the closure until body returns") {
  Fixture fixture;
  auto trace = std::make_shared<LifetimeTrace>();
  TracedValue capture(trace, true);

  fixture.scene | ui::onClick(fixture.button, ui::async([capture, trace, &fixture](ui::AsyncContext context) -> UiTask {
                                trace->closure = capture.id;

                                auto waited = co_await context.until(10);

                                if (!waited) {
                                  co_return waited;
                                }

                                take(fixture.application->tree().remove(fixture.button.node()));
                                CHECK(std::ranges::find(trace->destroyed, capture.id) == trace->destroyed.end());

                                co_return {};
                              }));
  fixture.prepare();
  take(fixture.button.activate().result());
  fixture.now = 10;
  fixture.dispatch();
  CHECK(std::ranges::find(trace->destroyed, trace->closure) != trace->destroyed.end());
  CHECK(trace->masked);
}

TEST_CASE("independent scene resumes publish only their own current context") {
  Fixture first;
  Fixture second;
  std::weak_ptr<ui::detail::SceneAnchor> firstAnchor;
  std::weak_ptr<ui::detail::SceneAnchor> secondAnchor;
  int calls{};

  const auto handler = [&calls](std::weak_ptr<ui::detail::SceneAnchor> &observed) {
    return ui::async([&calls, &observed](ui::AsyncContext context) -> UiTask {
      observed = ui::detail::CurrentSceneScope::snapshot();

      auto waited = co_await context.until(10);

      if (!waited) {
        co_return waited;
      }

      auto current = ui::detail::CurrentSceneScope::snapshot();

      CHECK_FALSE(current.expired());
      CHECK_FALSE(current.owner_before(observed));
      CHECK_FALSE(observed.owner_before(current));
      ++calls;

      co_return {};
    });
  };

  first.scene | ui::onClick(first.button, handler(firstAnchor));
  second.scene | ui::onClick(second.button, handler(secondAnchor));
  first.prepare();
  second.prepare();
  take(first.button.activate().result());
  take(second.button.activate().result());
  CHECK(firstAnchor.owner_before(secondAnchor) != secondAnchor.owner_before(firstAnchor));
  CHECK(ui::detail::CurrentSceneScope::snapshot().expired());
  second.now = 10;
  second.dispatch();
  CHECK(ui::detail::CurrentSceneScope::snapshot().expired());
  first.now = 10;
  first.dispatch();
  CHECK(calls == 2);
  CHECK(ui::detail::CurrentSceneScope::snapshot().expired());
}

TEST_CASE("shortcut and exact key bindings use the same coroutine adapter") {
  Fixture fixture;
  auto calls = std::make_shared<int>();

  fixture.scene | ui::onKey("F2", ui::async(countLater, calls)) |
    ui::onKey(ui::key(InputKey::F3), ui::async(countLater, calls));
  fixture.prepare();
  take(fixture.managed->input(*fixture.application, ui::key(InputKey::F2)));
  take(fixture.managed->input(*fixture.application, ui::key(InputKey::F3)));
  fixture.now = 10;
  fixture.dispatch();
  CHECK(*calls == 2);
}

TEST_CASE("full background admission retries on the existing application cycle without input") {
  TestExecutor executor;
  Fixture fixture(&executor);
  auto result = std::make_shared<int>();
  auto worker = std::make_shared<std::thread::id>();
  const auto uiThread = std::this_thread::get_id();

  fixture.scene | ui::onClick(fixture.button, ui::async([result, worker, uiThread](ui::AsyncContext context) -> UiTask {
                                auto value = co_await context.background<int>(
                                  std::make_shared<const int>(7), [worker](const int &input, std::stop_token) {
                                    *worker = std::this_thread::get_id();

                                    return std::make_shared<const int>(input * 2);
                                  });

                                if (!value) {
                                  co_return std::unexpected(value.error());
                                }

                                CHECK(std::this_thread::get_id() == uiThread);
                                *result = **value;

                                co_return {};
                              }));
  fixture.prepare();
  take(fixture.button.activate().result());
  CHECK(executor.attempts == 1);
  fixture.dispatch();
  CHECK(executor.attempts == 1);
  CHECK_FALSE(executor.job);
  executor.ready = true;
  ++executor.epoch;
  fixture.dispatch();
  REQUIRE(executor.job);
  CHECK(executor.attempts == 2);

  std::expected<void, AsyncError> sent;
  std::jthread producer([job = executor.job, &sent] {
    job->run(job->stopToken());
    sent = job->publish();
  });

  producer.join();
  REQUIRE(sent);
  CHECK(*result == 0);
  fixture.dispatch();
  CHECK(*result == 14);
  CHECK(*worker != uiThread);
}

TEST_CASE("throwing host retry is a typed coroutine failure rather than noexcept termination") {
  TestExecutor executor;
  Fixture fixture(&executor);

  fixture.scene | ui::onClick(fixture.button, ui::async([](ui::AsyncContext context) -> UiTask {
                                auto value = co_await context.background<int>(std::make_shared<const int>(1),
                                                                              [](const int &, std::stop_token) {
                                                                                return std::make_shared<const int>(2);
                                                                              });

                                co_return value ? std::expected<void, AsyncError>{} : std::unexpected(value.error());
                              }));
  fixture.prepare();
  take(fixture.button.activate().result());
  executor.ready = true;
  executor.throws = true;
  ++executor.epoch;
  fixture.dispatch();
  fixture.dispatch();

  const auto errors = take(fixture.managed->diagnostics());

  REQUIRE(errors.errors.size() == 2);
  CHECK(errors.errors[0].error.async == AsyncError::CALLBACK_FAILED);
  CHECK(std::string_view(errors.errors[0].error.operation) == "ui.async.background");
  CHECK_FALSE(errors.errors[0].error.background);
  CHECK(errors.errors[1].error.async == AsyncError::CALLBACK_FAILED);
  CHECK(std::string_view(errors.errors[1].error.operation) == "ui.async");
  CHECK_FALSE(errors.errors[1].error.background);
}

TEST_CASE("handled background admission error does not become a later task error cause") {
  TestExecutor executor;
  Fixture fixture(&executor);
  auto handled = std::make_shared<bool>();
  auto later = AsyncError::INVALID_ARGUMENT;

  SUBCASE("different later error") {
    later = AsyncError::INVALID_ARGUMENT;
  }

  SUBCASE("same later error is still unrelated") {
    later = AsyncError::CLOSED;
  }

  executor.ready = true;
  executor.failure = BackgroundError::CLOSED;
  fixture.scene | ui::onClick(fixture.button, ui::async([handled, later](ui::AsyncContext context) -> UiTask {
                                auto value = co_await context.background<int>(
                                  std::make_shared<const int>(1), [](const int &input, std::stop_token) {
                                    return std::make_shared<const int>(input);
                                  });

                                REQUIRE_FALSE(value);
                                CHECK(value.error() == AsyncError::CLOSED);
                                *handled = true;
                                auto waited = co_await context.after(std::chrono::milliseconds(10));

                                if (!waited) {
                                  co_return std::unexpected(waited.error());
                                }

                                co_return std::unexpected(later);
                              }));
  fixture.prepare();
  take(fixture.button.activate().result());
  fixture.dispatch();
  REQUIRE(*handled);
  fixture.now = 10;
  fixture.dispatch();
  const auto errors = take(fixture.managed->diagnostics());

  REQUIRE_FALSE(errors.errors.empty());
  REQUIRE(errors.errors.size() == 2);
  CHECK(errors.errors[0].error.async == AsyncError::CLOSED);
  CHECK(errors.errors[0].error.background == BackgroundError::CLOSED);
  CHECK(std::string_view(errors.errors[0].error.operation) == "ui.async.background");
  CHECK(errors.errors.back().error.async == later);
  CHECK_FALSE(errors.errors.back().error.background);
  CHECK(std::string_view(errors.errors.back().error.operation) == "ui.async");
}

TEST_CASE("terminal background admission retires work before later capacity service") {
  TestExecutor executor;
  Fixture fixture(&executor);
  auto handled = std::make_shared<bool>();
  auto workers = std::make_shared<int>();
  auto expected = AsyncError::CLOSED;
  bool deferred{};

  SUBCASE("immediate closed") {
    executor.ready = true;
    executor.failure = BackgroundError::CLOSED;
  }

  SUBCASE("immediate host exception") {
    executor.ready = true;
    executor.throws = true;
    expected = AsyncError::CALLBACK_FAILED;
  }

  SUBCASE("deferred no memory") {
    deferred = true;
    executor.failure = BackgroundError::NO_MEMORY;
    expected = AsyncError::NO_MEMORY;
  }

  SUBCASE("deferred host exception") {
    deferred = true;
    executor.throws = true;
    expected = AsyncError::CALLBACK_FAILED;
  }

  fixture.scene |
    ui::onClick(fixture.button, ui::async([handled, workers, expected](ui::AsyncContext context) -> UiTask {
                  auto value = co_await context.background<int>(std::make_shared<const int>(1),
                                                                [workers](const int &input, std::stop_token) {
                                                                  ++*workers;

                                                                  return std::make_shared<const int>(input);
                                                                });

                  REQUIRE_FALSE(value);
                  CHECK(value.error() == expected);
                  *handled = true;
                  auto waited = co_await context.after(std::chrono::milliseconds(100));

                  co_return waited ? std::expected<void, AsyncError>{} : std::unexpected(waited.error());
                }));
  fixture.prepare();
  take(fixture.button.activate().result());

  if (deferred) {
    executor.ready = true;
    ++executor.epoch;
    fixture.dispatch();
  }

  fixture.dispatch();
  REQUIRE(*handled);
  const auto attempts = executor.attempts;

  executor.failure.reset();
  executor.throws = false;
  ++executor.epoch;
  fixture.dispatch();
  ++executor.epoch;
  fixture.dispatch();

  CHECK(executor.attempts == attempts);
  CHECK_FALSE(executor.job);
  CHECK(*workers == 0);
  CHECK(fixture.now == 0);
}

TEST_CASE("background operation observer removal suppresses the awaiting user continuation") {
  TestExecutor executor;
  Fixture fixture(&executor);
  auto resumed = std::make_shared<bool>();
  int observed{};

  executor.ready = true;
  executor.failure = BackgroundError::CLOSED;
  fixture.scene | ui::onClick(fixture.button, ui::async([resumed](ui::AsyncContext context) -> UiTask {
                                auto value = co_await context.background<int>(
                                  std::make_shared<const int>(1), [](const int &input, std::stop_token) {
                                    return std::make_shared<const int>(input);
                                  });

                                *resumed = true;

                                co_return value ? std::expected<void, AsyncError>{} : std::unexpected(value.error());
                              }));
  fixture.prepare();
  take(fixture.managed->onError([&](const ManagedDiagnostic &diagnostic) {
    ++observed;
    CHECK(std::string_view(diagnostic.error.operation) == "ui.async.background");
    CHECK(diagnostic.error.background == BackgroundError::CLOSED);
    take(fixture.application->tree().remove(fixture.button.node()));
  }));
  take(fixture.button.activate().result());
  CHECK(observed == 0);
  fixture.dispatch();
  CHECK(observed == 1);
  CHECK_FALSE(*resumed);
  ++executor.epoch;
  fixture.dispatch();
  CHECK(executor.attempts == 1);
  CHECK_FALSE(executor.job);
  CHECK(take(fixture.managed->diagnostics()).errors.size() == 1);
}

TEST_CASE("invocation capture allocation failure preserves its exact async cause") {
  Fixture fixture;
  auto fail = std::make_shared<bool>();

  fixture.scene | ui::onClick(fixture.button, ui::async(ThrowingCopy(fail)));
  fixture.prepare();
  *fail = true;
  take(fixture.button.activate().result());
  *fail = false;

  const auto errors = take(fixture.managed->diagnostics());

  REQUIRE(errors.errors.size() == 1);
  CHECK(errors.errors[0].error.async == AsyncError::NO_MEMORY);
}

TEST_CASE("active preparation runs outside the preparation guard and rejects service reentry") {
  Fixture fixture;

  fixture.prepare();

  auto activity = std::make_shared<PreparedProbe>(fixture);

  take(ui::detail::SceneAccess::admitActivity(*fixture.managed, activity));
  take(fixture.managed->prepare(*fixture.application, {30, 4}));
  CHECK(activity->preparations == 1);
  CHECK(activity->services == 0);
  fixture.dispatch();
  CHECK(activity->services == 1);
}

TEST_CASE("relative timer preserves deferred delivery and exact preflight errors") {
  using namespace std::chrono_literals;

  Fixture fixture;
  auto context = std::make_shared<std::optional<ui::AsyncContext>>();
  auto foreignTimer = std::make_shared<std::optional<AsyncTimer>>();
  auto calls = std::make_shared<int>();
  std::chrono::milliseconds delay{10};
  std::optional<AsyncError> expected;
  bool foreign{};
  bool empty{};

  SUBCASE("positive") {
  }

  SUBCASE("zero is deferred") {
    delay = 0ms;
  }

  SUBCASE("negative") {
    delay = -1ms;
    expected = AsyncError::INVALID_ARGUMENT;
  }

  SUBCASE("deadline overflow") {
    fixture.now = std::numeric_limits<SchedulerTime>::max() - 5;
    expected = AsyncError::INVALID_ARGUMENT;
  }

  SUBCASE("empty context") {
    empty = true;
    expected = AsyncError::CLOSED;
  }

  SUBCASE("foreign context") {
    foreign = true;
    expected = AsyncError::WRONG_THREAD;
  }

  fixture.scene | ui::onClick(fixture.button, ui::async([context, foreignTimer, calls, delay,
                                                         empty](ui::AsyncContext current) -> UiTask {
                                *context = current;

                                auto waited = co_await (empty ? ui::AsyncContext{}.after(delay) : current.after(delay));

                                if (!waited) {
                                  co_return waited;
                                }

                                if (*foreignTimer) {
                                  co_return co_await std::move(**foreignTimer);
                                }

                                ++*calls;

                                co_return {};
                              }));
  fixture.prepare();
  take(fixture.button.activate().result());
  CHECK(*calls == 0);

  if (foreign) {
    std::jthread producer([context, foreignTimer] {
      foreignTimer->emplace((*context)->after(0ms));
    });

    producer.join();
  }

  if (!expected || foreign) {
    fixture.now = 10;
    fixture.dispatch();
  }

  const auto errors = take(fixture.managed->diagnostics());

  if (expected) {
    REQUIRE(errors.errors.size() == 1);
    CHECK(errors.errors[0].error.async == expected);
    CHECK(*calls == 0);
  } else {
    CHECK(errors.errors.empty());
    CHECK(*calls == 1);

    Fixture closed;

    closed.scene | ui::onClick(closed.button, ui::async([old = **context](ui::AsyncContext) -> UiTask {
                                 co_return co_await old.after(0ms);
                               }));
    closed.prepare();
    take(closed.button.activate().result());

    const auto expired = take(closed.managed->diagnostics());

    REQUIRE(expired.errors.size() == 1);
    CHECK(expired.errors[0].error.async == AsyncError::CLOSED);
  }
}

TEST_CASE("background domain errors remain typed data and executor errors retain both exact causes") {
  enum class DomainError {
    MISSING
  };
  using DomainResult = std::expected<int, DomainError>;

  TestExecutor executor;
  Fixture fixture(&executor);
  auto seen = std::make_shared<std::optional<DomainError>>();

  executor.ready = true;

  SUBCASE("domain result") {
    fixture.scene |
      ui::onClick(fixture.button, ui::async([seen](ui::AsyncContext context) -> UiTask {
                    auto value = co_await context.background<DomainResult>(
                      std::make_shared<const int>(1), [](const int &, std::stop_token) {
                        return std::make_shared<const DomainResult>(std::unexpected(DomainError::MISSING));
                      });

                    if (!value) {
                      co_return std::unexpected(value.error());
                    }

                    const auto &domain = **value;

                    if (!domain) {
                      *seen = domain.error();
                    }

                    co_return {};
                  }));
    fixture.prepare();
    take(fixture.button.activate().result());
    REQUIRE(executor.job);

    std::expected<void, AsyncError> sent;
    std::jthread producer([job = executor.job, &sent] {
      job->run(job->stopToken());
      sent = job->publish();
    });

    producer.join();
    REQUIRE(sent);
    fixture.dispatch();
    CHECK(*seen == DomainError::MISSING);
    CHECK(take(fixture.managed->diagnostics()).errors.empty());
  }

  SUBCASE("executor closed") {
    executor.failure = BackgroundError::CLOSED;
    fixture.scene | ui::onClick(fixture.button, ui::async([](ui::AsyncContext context) -> UiTask {
                                  auto value = co_await context.background<int>(
                                    std::make_shared<const int>(1), [](const int &input, std::stop_token) {
                                      return std::make_shared<const int>(input);
                                    });

                                  co_return value ? std::expected<void, AsyncError>{} : std::unexpected(value.error());
                                }));
    fixture.prepare();
    take(fixture.button.activate().result());
    fixture.dispatch();

    const auto errors = take(fixture.managed->diagnostics());

    REQUIRE(errors.errors.size() == 2);
    CHECK(errors.errors[0].error.async == AsyncError::CLOSED);
    CHECK(errors.errors[0].error.background == BackgroundError::CLOSED);
    CHECK(std::string_view(errors.errors[0].error.operation) == "ui.async.background");
    CHECK(errors.errors[1].error.async == AsyncError::CLOSED);
    CHECK_FALSE(errors.errors[1].error.background);
    CHECK(std::string_view(errors.errors[1].error.operation) == "ui.async");
  }
}

TEST_CASE("ordinary runApp owns the background pool and resumes the same native key path") {
  auto entered = std::make_shared<std::promise<void>>();
  auto worker = std::make_shared<std::thread::id>();
  NativeProbeTerminal terminal(entered->get_future());
  int applied{};

  const auto status = ui::runApp(
    [entered, worker, &applied] {
      return ui::scene(ui::label("F2 computes, then closes")) |
             ui::onKey("F2", ui::async([entered, worker, &applied](ui::AsyncContext context) -> UiTask {
                         auto value = co_await context.background<int>(
                           std::make_shared<const int>(7), [entered, worker](const int &input, std::stop_token) {
                             *worker = std::this_thread::get_id();
                             entered->set_value();

                             return std::make_shared<const int>(input * 2);
                           });

                         if (!value) {
                           co_return std::unexpected(value.error());
                         }

                         applied = **value;
                         ui::quit();

                         co_return {};
                       }));
    },
    {.terminal = &terminal});

  CHECK(status == 0);
  CHECK(terminal.closed);
  CHECK(applied == 14);
  CHECK(*worker != std::thread::id{});
  CHECK(*worker != std::this_thread::get_id());
}
