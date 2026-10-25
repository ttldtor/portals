// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "../src/widgets/line_edit_validation.hpp"
#include <doctest.h>
#include <portals/background.hpp>
#include <portals/ui.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace portals;

namespace {
template <class T, class E>
T take(std::expected<T, E> value, std::source_location location = std::source_location::current()) {
  if (!value) {
    MESSAGE(std::format("{}:{}", location.file_name(), location.line()));

    if constexpr (std::same_as<E, ManagedError>) {
      ui::SceneReport report;

      report.failure = value.error();
      ui::reportToStderr(report);
    } else {
      MESSAGE("Error code: " << static_cast<int>(value.error()));
    }
  }

  REQUIRE(value);

  if constexpr (!std::is_void_v<T>) {
    return std::move(*value);
  }
}

class Executor final : public BackgroundExecutor {
public:
  bool full{};
  std::optional<BackgroundError> error;
  std::uint64_t epoch{};
  int attempts{};
  std::vector<std::shared_ptr<BackgroundJob>> jobs;

  std::expected<BackgroundTicket, BackgroundError> submit(std::unique_ptr<BackgroundJob> &job) override {
    ++attempts;

    if (error) {
      return std::unexpected(*error);
    }

    if (full) {
      return std::unexpected(BackgroundError::WOULD_BLOCK);
    }

    jobs.emplace_back(std::move(job));

    return BackgroundTicket(jobs.back());
  }

  void service() noexcept override {
  }

  std::uint64_t capacityEpoch() const noexcept override {
    return epoch;
  }

  void finish(std::size_t index) {
    auto job = jobs.at(index);

    job->run(job->stopToken());
    take(job->publish());
  }
};

struct Fixture {
  Executor executor;
  Application *app{};
  ManagedScene *owner{};
  ui::Ref<FormField> field;
  ui::Ref<Button> other;
  ui::Scene scene;
  std::vector<LineEditResult> results;
  std::function<void(const LineEditResult &)> onResult;
  int edits{};
  int finishes{};

  explicit Fixture(
    EditMode mode = EditMode::IMMEDIATE,
    ui::AsyncFieldValidator work =
      [](std::string_view value, std::stop_token) {
        return value == "bad" ? FieldValidation::invalid("Unavailable") : FieldValidation::valid("Available");
      },
    ui::FieldValidationLimits limits = {}, std::function<FieldValidation(std::string_view)> synchronous = {},
    std::shared_ptr<Clipboard> clipboard = {})
      : scene(ui::scene(ui::vbox(ui::formField(field, "Name", "",
                                               {.validate = std::move(synchronous),
                                                .editor = {.result =
                                                             [this](LineEditResult result) {
                                                               results.push_back(std::move(result));

                                                               if (onResult) {
                                                                 onResult(results.back());
                                                               }

                                                               return std::expected<void, TreeError>{};
                                                             },
                                                           .options = {.editMode = mode},
                                                           .clipboard = std::move(clipboard)},
                                                .validateAsync = std::move(work),
                                                .validationLimits = limits}),
                                 ui::button(other, "Other").onClick([] {
                                 })),
                        {.application = {.scheduler = {.data_capacity = 2}, .backgroundExecutor = &executor}})) {
    scene.configure([this](ManagedScene &managed, Application &application) {
      owner = &managed;
      app = &application;
    });
    take(scene.prepareChecked());
    take(field.focus());
  }

  void cycle() {
    take(app->scheduler().dispatch());
    take(owner->afterDispatch(*app));
  }

  void set(std::string_view value) {
    take((field.text() = value).result());
    cycle();
  }

  void enter() {
    take(app->router().invoke(field.editor().node(), ActionEvent{Action::ACTIVATE}));
    cycle();
  }

  void escape() {
    take(app->router().invoke(field.editor().node(), ActionEvent{Action::CANCEL}));
    cycle();
  }

  FieldStatus status() {
    return take(field.validation()).status;
  }
};
} // namespace

TEST_CASE("async field is additive and synchronous bool validation cannot pretend pending is false") {
  Fixture fixture;
  CHECK(fixture.status() == FieldStatus::UNCHECKED);
  auto checked = fixture.field.validate();
  CHECK_FALSE(checked);
  CHECK(std::get<TreeError>(checked.error().cause) == TreeError::INVALID_ARGUMENT);
  fixture.set("alice");
  CHECK(fixture.status() == FieldStatus::PENDING);
  REQUIRE(fixture.executor.jobs.size() == 1);
  CHECK(fixture.results.back().status == LineEditStatus::PENDING);
  fixture.executor.finish(0);
  fixture.cycle();
  CHECK(fixture.status() == FieldStatus::VALID);
  CHECK(fixture.results.back().status == LineEditStatus::COMMITTED);
  CHECK(fixture.results.back().trigger == LineEditTrigger::ASYNC_VALIDATION);
  CHECK(take(fixture.field.text().get()) == "alice");
  CHECK(fixture.field.editor().canUndo());
}

TEST_CASE("all modes preserve deferred draft, repeated Enter shares one request, completion commits once") {
  for (const auto mode : {EditMode::IMMEDIATE, EditMode::ON_COMPLETION, EditMode::EXPLICIT}) {
    Fixture fixture(mode);
    fixture.set("alice");
    CHECK(fixture.executor.jobs.size() == (mode == EditMode::IMMEDIATE ? 1 : 0));
    fixture.enter();
    fixture.enter();
    CHECK(fixture.status() == FieldStatus::PENDING);
    REQUIRE(fixture.executor.jobs.size() == 1);
    fixture.executor.finish(0);
    fixture.cycle();
    CHECK(fixture.status() == FieldStatus::VALID);
    CHECK(std::ranges::count(fixture.results, LineEditStatus::COMMITTED, &LineEditResult::status) == 1);
    CHECK(fixture.field.editor().canUndo());
    fixture.cycle();
    CHECK(std::ranges::count(fixture.results, LineEditStatus::COMMITTED, &LineEditResult::status) == 1);
  }
}

TEST_CASE("focus loss respects ON_COMPLETION and EXPLICIT without changing their meaning") {
  for (const auto mode : {EditMode::ON_COMPLETION, EditMode::EXPLICIT}) {
    Fixture fixture(mode);
    fixture.set("alice");
    take(fixture.other.focus());
    fixture.cycle();
    CHECK(fixture.executor.jobs.size() == (mode == EditMode::ON_COMPLETION ? 1 : 0));
  }
}

TEST_CASE("completion dispatched by a result callback is retained until the editor transaction returns") {
  Fixture fixture;
  fixture.set("alice");
  fixture.executor.finish(0);
  bool reentered{};
  fixture.onResult = [&](const LineEditResult &result) {
    if (!reentered && result.status == LineEditStatus::PENDING) {
      reentered = true;
      take(fixture.app->scheduler().dispatch());
    }
  };
  fixture.enter();
  fixture.cycle();
  CHECK(reentered);
  CHECK(fixture.status() == FieldStatus::VALID);
  CHECK(fixture.executor.jobs.size() == 1);
  CHECK(std::ranges::count(fixture.results, LineEditStatus::COMMITTED, &LineEditResult::status) == 1);
}

TEST_CASE("A B A edits reject out of order results even when bytes match and Escape retires pending") {
  Fixture fixture;
  fixture.set("A");
  fixture.set("B");
  fixture.set("A");
  REQUIRE(fixture.executor.jobs.size() == 3);
  fixture.executor.finish(0);
  fixture.cycle();
  CHECK(fixture.status() == FieldStatus::PENDING);
  fixture.executor.finish(1);
  fixture.cycle();
  CHECK(fixture.status() == FieldStatus::PENDING);
  fixture.executor.finish(2);
  fixture.cycle();
  CHECK(fixture.status() == FieldStatus::VALID);
  CHECK(std::ranges::count(fixture.results, LineEditStatus::COMMITTED, &LineEditResult::status) == 1);
  fixture.set("late");
  fixture.escape();
  CHECK(take(fixture.field.text().get()) == "A");
  CHECK_FALSE(fixture.field.editor().canUndo());
  fixture.executor.finish(3);
  fixture.cycle();
  CHECK(take(fixture.field.text().get()) == "A");
  CHECK(std::ranges::count(fixture.results, LineEditStatus::COMMITTED, &LineEditResult::status) == 1);
}

TEST_CASE("full worker admission retains newest intent and retries without another key") {
  Fixture fixture;
  fixture.executor.full = true;
  fixture.set("old");
  fixture.set("latest");
  CHECK(fixture.executor.jobs.empty());
  fixture.executor.full = false;
  ++fixture.executor.epoch;
  fixture.cycle();
  REQUIRE(fixture.executor.jobs.size() == 1);
  fixture.executor.finish(0);
  fixture.cycle();
  CHECK(fixture.status() == FieldStatus::VALID);
  CHECK(take(fixture.field.text().get()) == "latest");
}

TEST_CASE("business invalid and typed technical failure preserve draft and allow retry") {
  Fixture fixture;
  fixture.set("bad");
  fixture.executor.finish(0);
  fixture.cycle();
  CHECK(fixture.status() == FieldStatus::INVALID);
  CHECK(take(fixture.field.text().get()) == "bad");
  CHECK(fixture.field.editor().canUndo());
  fixture.executor.error = BackgroundError::CLOSED;
  fixture.set("next");
  fixture.cycle();
  CHECK(fixture.status() == FieldStatus::TECHNICAL_ERROR);
  CHECK(fixture.results.back().error == TreeError::CLOSED);
  CHECK(fixture.results.back().background == BackgroundError::CLOSED);
  fixture.executor.error.reset();
  ++fixture.executor.epoch;
  fixture.enter();
  REQUIRE(fixture.executor.jobs.size() == 2);
  fixture.executor.finish(1);
  fixture.cycle();
  CHECK(fixture.status() == FieldStatus::VALID);
}

TEST_CASE("worker exceptions wrong status malformed and oversized messages remain recoverable") {
  const std::array<ui::AsyncFieldValidator, 4> rules{[](auto, auto) -> std::expected<FieldValidation, TreeError> {
                                                       throw std::runtime_error("worker failure");
                                                     },
                                                     [](auto, auto) {
                                                       return FieldValidation::pending();
                                                     },
                                                     [](auto, auto) {
                                                       return FieldValidation::valid(std::string(1, '\xff'));
                                                     },
                                                     [](auto, auto) {
                                                       return FieldValidation::valid(std::string(8, 'x'));
                                                     }};

  for (const auto &rule : rules) {
    Fixture fixture(EditMode::IMMEDIATE, rule, {.maxCandidateBytes = 16, .maxMessageBytes = 4});
    fixture.set("text");
    fixture.executor.finish(0);
    fixture.cycle();
    CHECK(fixture.status() == FieldStatus::TECHNICAL_ERROR);
    CHECK(fixture.results.back().status == LineEditStatus::FAILED);
    CHECK(fixture.results.back().error.has_value());
    CHECK(take(fixture.field.text().get()) == "text");
    CHECK(fixture.field.editor().canUndo());
  }
}

TEST_CASE("async configuration preserves the fatal policy and cause of a synchronous validator exception") {
  for (const bool afterWorker : {false, true}) {
    bool shouldThrow = !afterWorker;
    Fixture fixture(
      EditMode::IMMEDIATE,
      [](auto, auto) {
        return FieldValidation::valid();
      },
      {},
      [&](auto) -> FieldValidation {
        if (shouldThrow) {
          throw std::runtime_error("synchronous rule failure");
        }
        return FieldValidation::valid();
      });
    auto changed = (fixture.field.text() = "text").result();
    CHECK(changed); // The legacy fluent result acknowledges the draft, not successful model publication.
    if (afterWorker) {
      take(fixture.owner->afterDispatch(*fixture.app));
      REQUIRE(fixture.executor.jobs.size() == 1);
      shouldThrow = true;
      fixture.executor.finish(0);
    } else {
      CHECK(fixture.executor.jobs.empty());
    }
    auto dispatched = take(fixture.app->scheduler().dispatch());
    REQUIRE(fixture.results.size() == (afterWorker ? 2 : 1));
    CHECK(fixture.results.back().error == TreeError::CALLBACK_FAILED);
    CHECK_FALSE(fixture.results.back().recoverable);
    CHECK_FALSE(fixture.results.back().async);
    CHECK_FALSE(fixture.results.back().background);
    CHECK(fixture.results.back().operation == nullptr);
    CHECK(dispatched.stopped);
  }
}

TEST_CASE("completion eligibility failure never inherits the rejected worker message cause") {
  Fixture fixture(EditMode::IMMEDIATE, [](auto, auto) {
    return FieldValidation::valid(std::string(1, '\xff'));
  });
  fixture.set("text");
  fixture.executor.finish(0);
  take(fixture.app->tree().setEnabled(fixture.field.editor().node(), false));
  auto dispatched = take(fixture.app->scheduler().dispatch());
  REQUIRE(fixture.results.size() == 2);
  const auto &result = fixture.results.back();
  CHECK(result.error == TreeError::WOULD_BLOCK);
  CHECK(result.recoverable);
  CHECK_FALSE(result.async);
  CHECK_FALSE(result.background);
  CHECK_FALSE(result.text);
  CHECK(std::string_view(result.operation) == "formField.validateAsync");
  CHECK_FALSE(dispatched.stopped);
}

TEST_CASE("async configuration never decorates an unrelated clipboard receipt") {
  struct FailedClipboard final : Clipboard {
    std::expected<std::string, TreeError> read(std::size_t) override {
      return std::unexpected(TreeError::CLOSED);
    }

    std::expected<void, TreeError> write(std::string_view) override {
      return std::unexpected(TreeError::CLOSED);
    }
  };

  Fixture fixture(
    EditMode::IMMEDIATE,
    [](auto, auto) {
      return FieldValidation::valid();
    },
    {}, {}, std::make_shared<FailedClipboard>());
  take(fixture.app->router().dispatch(KeyEvent{InputKey::CHARACTER, U'v', {.control = true}}));
  REQUIRE(fixture.results.size() == 1);
  const auto &result = fixture.results.back();
  CHECK(result.status == LineEditStatus::CLIPBOARD_FAILED);
  CHECK(result.error == TreeError::CLOSED);
  CHECK_FALSE(result.recoverable);
  CHECK_FALSE(result.async);
  CHECK_FALSE(result.background);
  CHECK(result.operation == nullptr);
  CHECK(fixture.executor.jobs.empty());
  take(fixture.app->scheduler().dispatch());
}

namespace {
// A host hook exists only on the UI thread. The worker owns no UI handle or callback closure.
thread_local std::function<void()> *cancellationHostHook{};
} // namespace

TEST_CASE("stop callback host reentry retains the newest validation intent without cancelling it on the next cycle") {
  auto started = std::make_shared<std::promise<void>>();
  auto start = started->get_future();
  auto release = std::make_shared<std::promise<void>>();
  auto released = release->get_future().share();
  const auto uiThread = std::this_thread::get_id();
  Fixture fixture(EditMode::IMMEDIATE, [=](std::string_view value, std::stop_token stop) {
    if (value == "first") {
      std::stop_callback callback(stop, [uiThread] {
        if (std::this_thread::get_id() == uiThread && cancellationHostHook) {
          (*cancellationHostHook)();
        }
      });
      started->set_value();
      released.wait();
    }
    return FieldValidation::valid();
  });
  fixture.set("first");
  REQUIRE(fixture.executor.jobs.size() == 1);
  auto first = fixture.executor.jobs.front();
  std::jthread worker([first] {
    first->run(first->stopToken());
  });

  struct Cleanup {
    std::shared_ptr<std::promise<void>> release;

    ~Cleanup() {
      cancellationHostHook = nullptr;
      release->set_value();
    }
  } cleanup{release};

  REQUIRE(start.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  int calls{};
  std::function<void()> hook = [&] {
    ++calls;
    take((fixture.field.text() = "latest").result());
  };
  cancellationHostHook = &hook;
  fixture.set("intermediate");
  cancellationHostHook = nullptr;
  CHECK(calls == 1);
  fixture.cycle();
  REQUIRE(fixture.executor.jobs.size() >= 2);
  auto latest = fixture.executor.jobs.back();
  CHECK_FALSE(latest->stopToken().stop_requested());
  if (!latest->stopToken().stop_requested()) {
    latest->run(latest->stopToken());
    take(latest->publish());
  }
  fixture.cycle();
  CHECK(fixture.status() == FieldStatus::VALID);
  CHECK(take(fixture.field.text().get()) == "latest");
}

TEST_CASE("candidate byte limit does not reduce accepted editor text or its undo history") {
  Fixture fixture(EditMode::IMMEDIATE,
                  [](auto, auto) {
                    return FieldValidation::valid();
                  },
                  {.maxCandidateBytes = 2, .maxMessageBytes = 16});
  fixture.set("longer");
  CHECK(take(fixture.field.text().get()) == "longer");
  CHECK(fixture.status() == FieldStatus::TECHNICAL_ERROR);
  CHECK(fixture.executor.jobs.empty());
  CHECK(fixture.field.editor().canUndo());
}

TEST_CASE("keyboard paste Undo Redo and equal programmatic RECORD share the real draft generation boundary") {
  Fixture fixture;
  take(fixture.app->router().dispatch(TextInputEvent{U'a'}));
  fixture.cycle();
  take(fixture.app->router().dispatch(PasteInputEvent{"bc"}));
  fixture.cycle();
  REQUIRE(fixture.executor.jobs.size() == 2);
  CHECK(take(fixture.field.text().get()) == "abc");
  take(fixture.field.editor().undo().result());
  fixture.cycle();
  CHECK(take(fixture.field.text().get()) == "a");
  take(fixture.field.editor().redo().result());
  fixture.cycle();
  CHECK(take(fixture.field.text().get()) == "abc");
  REQUIRE(fixture.executor.jobs.size() == 4);
  fixture.set("abc");
  CHECK(fixture.executor.jobs.size() == 4);
  fixture.executor.finish(1);
  fixture.cycle();
  CHECK(fixture.status() == FieldStatus::PENDING);
  fixture.executor.finish(3);
  fixture.cycle();
  CHECK(fixture.status() == FieldStatus::VALID);
}

TEST_CASE("completed and equal validation presentation never causes idle redraw") {
  Fixture fixture;
  fixture.set("alice");
  fixture.executor.finish(0);
  fixture.cycle();
  take(fixture.app->tree().consumeFrameRequest());

  for (int cycle = 0; cycle < 8; ++cycle) {
    fixture.cycle();
    CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
  }

  take(fixture.field.display(FieldValidation::valid("Available")));
  CHECK(take(fixture.app->tree().consumeFrameRequest()) == Invalidation::NONE);
}

TEST_CASE("rejected result publication retries the prepared immutable result without recomputing") {
  int calls{};
  Fixture fixture(EditMode::IMMEDIATE, [&calls](auto, auto) {
    ++calls;

    return FieldValidation::valid();
  });
  fixture.set("alice");
  auto job = fixture.executor.jobs.at(0);
  job->run(job->stopToken());
  std::vector<Publisher<int>> blockers;
  std::vector<Subscription> subscriptions;

  take(fixture.app->tree().withScope(fixture.field.node(), [&](const UiScope &scope) {
    for (;;) {
      auto connection = take(fixture.app->scheduler().connect<int>(scope, [](auto) {
      }));
      auto sent = connection.publisher.publish(std::make_shared<const int>(1));

      blockers.push_back(std::move(connection.publisher));
      subscriptions.push_back(std::move(connection.subscription));

      if (!sent) {
        REQUIRE(sent.error() == SchedulerError::WOULD_BLOCK);

        break;
      }
    }
  }));
  auto rejected = job->publish();
  CHECK_FALSE(rejected);
  CHECK(rejected.error() == AsyncError::WOULD_BLOCK);
  take(fixture.app->scheduler().dispatch());
  take(job->publish());
  fixture.cycle();
  CHECK(calls == 1);
  CHECK(fixture.status() == FieldStatus::VALID);
}

TEST_CASE("protected async configuration is rejected instead of copying hidden plaintext to workers") {
  for (const auto mode : {LineEditDisplayMode::PASSWORD, LineEditDisplayMode::NO_ECHO}) {
    auto scene = ui::scene(
      ui::formField("Secret", "", {.editor = {.options = {.displayMode = mode}}, .validateAsync = [](auto, auto) {
                                     return FieldValidation::valid();
                                   }}));
    auto prepared = scene.prepareChecked();
    CHECK_FALSE(prepared);
    CHECK(std::get<TreeError>(prepared.error().cause) == TreeError::INVALID_ARGUMENT);
    (void)scene.close();
  }
}

TEST_CASE("real pool executes validation off UI and cancellation callbacks run after routed editing returns") {
  for (const bool closeFromCallback : {false, true}) {
    auto pool = take(BackgroundPool::create({.workers = 1, .pending = 1}));
    auto started = std::make_shared<std::promise<void>>();
    auto start = started->get_future();
    auto release = std::make_shared<std::promise<void>>();
    auto released = release->get_future().share();

    struct ReleaseGuard {
      std::shared_ptr<std::promise<void>> promise;

      ~ReleaseGuard() {
        try {
          promise->set_value();
        } catch (const std::future_error &) {
        }
      }
    } releaseGuard{release};

    auto insideInput = std::make_shared<std::atomic<bool>>(false);
    auto stopInsideInput = std::make_shared<std::atomic<bool>>(false);
    auto stopped = std::make_shared<std::atomic<bool>>(false);
    const auto uiThread = std::this_thread::get_id();
    auto workerThread = std::make_shared<std::thread::id>();
    ui::Ref<FormField> field;
    ManagedScene *owner{};
    Application *app{};
    bool closeOnResult{};
    auto scene =
      ui::scene(ui::formField(field, "Name", "",
                              {.editor = {.result =
                                            [&](LineEditResult result) {
                                              if (closeOnResult && result.status == LineEditStatus::PENDING) {
                                                insideInput->store(true);
                                                take(owner->close());
                                                CHECK_FALSE(stopped->load());
                                                insideInput->store(false);
                                              }

                                              return std::expected<void, TreeError>{};
                                            }},
                               .validateAsync =
                                 [=](auto, std::stop_token stop) {
                                   *workerThread = std::this_thread::get_id();
                                   std::stop_callback callback(stop, [=] {
                                     stopInsideInput->store(insideInput->load());
                                     stopped->store(true);
                                   });

                                   started->set_value();
                                   released.wait();

                                   return FieldValidation::valid();
                                 }}),
                {.application = {.backgroundExecutor = pool.get()}});
    scene.configure([&](ManagedScene &managed, Application &application) {
      owner = &managed;
      app = &application;
    });
    take(scene.prepareChecked());
    take(field.focus());
    take((field.text() = "first").result());
    take(owner->afterDispatch(*app));
    REQUIRE(start.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    if (closeFromCallback) {
      closeOnResult = true;
      take(app->router().invoke(field.editor().node(), {Action::ACTIVATE}));
    } else {
      insideInput->store(true);
      take(app->router().invoke(field.editor().node(), {Action::CANCEL}));
      CHECK_FALSE(stopped->load());
      insideInput->store(false);
      take(owner->afterDispatch(*app));
    }
    CHECK(stopped->load());
    CHECK_FALSE(stopInsideInput->load());
    CHECK(*workerThread != uiThread);
    release->set_value();
    take(scene.close());
    pool->join();
  }
}

TEST_CASE("removal and scene close retire completion without worker-owned UI objects") {
  Fixture fixture;
  fixture.set("late");
  auto job = fixture.executor.jobs.at(0);
  take(fixture.owner->remove(fixture.field.node()));
  fixture.cycle();
  CHECK(job->stopToken().stop_requested());
  job->run(job->stopToken());
  auto published = job->publish();
  CHECK_FALSE(published);
  CHECK(published.error() == AsyncError::CLOSED);
  fixture.cycle();
  CHECK(std::ranges::count(fixture.results, LineEditStatus::COMMITTED, &LineEditResult::status) == 0);
  take(fixture.scene.close());
}

TEST_CASE("self removal during a pending result cancels retained old worker without another input event") {
  Fixture fixture;
  fixture.set("first");
  auto old = fixture.executor.jobs.at(0);
  fixture.onResult = [&](const LineEditResult &result) {
    if (result.status == LineEditStatus::PENDING) {
      take(fixture.owner->remove(fixture.field.node()));
      CHECK_FALSE(old->stopToken().stop_requested());
    }
  };
  fixture.set("second");
  CHECK(old->stopToken().stop_requested());
  CHECK(fixture.executor.jobs.size() == 1);
}

TEST_CASE("scene closure retires its own worker without joining or another key") {
  Fixture fixture;
  fixture.set("first");
  auto old = fixture.executor.jobs.at(0);
  take(fixture.scene.close());
  CHECK(old->stopToken().stop_requested());
}

TEST_CASE("reentrant managed scene close leaves no UI publication or uncancelled own worker") {
  Fixture fixture;
  fixture.set("first");
  auto old = fixture.executor.jobs.at(0);
  fixture.onResult = [&](const LineEditResult &result) {
    if (result.status == LineEditStatus::PENDING) {
      take(fixture.owner->close());
      CHECK_FALSE(old->stopToken().stop_requested());
    }
  };
  fixture.set("second");
  CHECK(old->stopToken().stop_requested());
}

namespace {
struct Gate final : detail::LineEditValidationGate {
  detail::EditorValidationIdentity requested{};
  bool approved{};
  int requests{};
  bool *destroyedWidget{};
  bool *safeDestruction{};
  bool *gateDestroyed{};

  ~Gate() override {
    if (safeDestruction) {
      *safeDestruction = *destroyedWidget;
      *gateDestroyed = true;
    }
  }

  std::expected<detail::EditorValidationDecision, TreeError> check(std::string_view,
                                                                   detail::EditorValidationIdentity identity) override {
    requested = identity;

    if (approved) {
      approved = false;

      return detail::EditorValidationDecision::ACCEPT;
    }

    ++requests;

    return detail::EditorValidationDecision::PENDING;
  }

  void retire() noexcept override {
  }

  void flush(bool) noexcept override {
  }

  void close() noexcept override {
  }

  void hold() noexcept override {
  }

  void finished(LineEditResult &, bool) override {
  }
};

struct EditorFixture {
  std::unique_ptr<Scheduler> scheduler = take(Scheduler::create());
  std::unique_ptr<WidgetTree> tree = take(WidgetTree::create(*scheduler));
  Node<Widget> root = take(tree->root());
  std::unique_ptr<EventRouter> router = take(EventRouter::create(*tree));
  Node<LineEdit> node = take(tree->emplace<LineEdit>(root));
  std::shared_ptr<Property<std::string>> model =
    take(Property<std::string>::create(*tree, root, std::string{}, Invalidation::LAYOUT));
  std::shared_ptr<Gate> gate = std::make_shared<Gate>();
  std::function<bool()> validator;
  int edits{};
  int boundaries{};
  std::vector<LineEditResult> receipts;
  LineEditBinding binding = take(bindLineEditWithEvents(
    *tree, *router, node, model,
    [this](LineEditResult value) {
      receipts.push_back(std::move(value));
    },
    {},
    [this](std::string_view) {
      return !validator || validator();
    },
    {.textEdited =
       [this](const auto &) {
         ++edits;
       },
     .editingFinished =
       [this](const auto &) {
         ++boundaries;
       }}));
  detail::EditorValidationComplete complete = take(detail::LineEditValidationAccess::attach(binding, gate));

  EditorFixture() {
    take(router->setFocus(node));
  }

  void type(char32_t value) {
    take(router->dispatch(TextInputEvent{value}));
  }
};
} // namespace

TEST_CASE("exact model revision blocks validated overwrite including model A B A and keeps history") {
  EditorFixture fixture;
  fixture.type(U'x');
  const auto identity = fixture.gate->requested;
  take(fixture.model->set("B"));
  take(fixture.model->set(""));
  fixture.gate->approved = true;
  auto result = take(fixture.complete(identity, true, {}));
  REQUIRE(result);
  CHECK(result->status == LineEditStatus::CONFLICT);
  CHECK(take(fixture.model->snapshot())->value == "");
  CHECK(fixture.edits == 1);
  CHECK(fixture.boundaries == 0);
}

TEST_CASE("disabled publication and completion cannot emit extra text or finish events") {
  EditorFixture fixture;
  fixture.type(U'x');
  auto identity = fixture.gate->requested;
  take(fixture.router->invoke(fixture.node, {Action::ACTIVATE}));
  CHECK(fixture.edits == 1);
  CHECK(fixture.boundaries == 1);
  fixture.gate->approved = true;
  auto result = take(fixture.complete(identity, true, {}));
  REQUIRE(result);
  CHECK(result->status == LineEditStatus::COMMITTED);
  CHECK(fixture.edits == 1);
  CHECK(fixture.boundaries == 1);
  CHECK(take(fixture.model->snapshot())->value == "x");
  CHECK(fixture.binding.canUndo());
  fixture.type(U'y');
  identity = fixture.gate->requested;
  take(fixture.tree->setEnabled(fixture.node, false));
  result = take(fixture.complete(identity, true, {}));
  REQUIRE(result);
  CHECK(result->error == TreeError::WOULD_BLOCK);
  CHECK(result->recoverable);
  CHECK(take(fixture.model->snapshot())->value == "x");
}

TEST_CASE("successful worker approval still rechecks eligibility changed by a synchronous UI rule") {
  EditorFixture fixture;
  fixture.type(U'x');
  auto identity = fixture.gate->requested;
  fixture.gate->approved = true;
  fixture.validator = [&] {
    take(fixture.tree->setEnabled(fixture.node, false));

    return true;
  };
  auto result = take(fixture.complete(identity, true, {}));
  REQUIRE(result);
  CHECK(result->error == TreeError::WOULD_BLOCK);
  CHECK_FALSE(result->recoverable); // A synchronous rule's side effects still use the ordinary commit path.
  CHECK(take(fixture.model->snapshot())->value.empty());
}

TEST_CASE("retained router State releases final gate after outer tree borrow reclaims removed editor") {
  struct ProbeEdit final : LineEdit {
    bool &destroyed;

    explicit ProbeEdit(bool &flag) : destroyed(flag) {
    }

    ~ProbeEdit() override {
      destroyed = true;
    }
  };

  auto scheduler = take(Scheduler::create());
  auto tree = take(WidgetTree::create(*scheduler));
  auto root = take(tree->root());
  auto router = take(EventRouter::create(*tree));
  bool destroyed{};
  bool safe{};
  bool gateDestroyed{};
  auto derived = take(tree->emplace<ProbeEdit>(root, destroyed));
  Node<LineEdit> node = derived;
  std::shared_ptr<Property<std::string>> model =
    take(Property<std::string>::create(*tree, root, std::string{}, Invalidation::LAYOUT));
  std::optional<LineEditBinding> binding;
  binding.emplace(take(bindLineEdit(*tree, *router, node, model, [&](LineEditResult) {
    take(tree->remove(node));
    binding.reset();
    CHECK_FALSE(destroyed);
  })));
  auto gate = std::make_shared<Gate>();
  gate->destroyedWidget = &destroyed;
  gate->safeDestruction = &safe;
  gate->gateDestroyed = &gateDestroyed;
  (void)take(detail::LineEditValidationAccess::attach(*binding, gate));
  gate.reset();
  take(router->setFocus(node));
  take(router->dispatch(TextInputEvent{U'x'}));
  CHECK(destroyed);
  take(scheduler->dispatch());
  take(router->dispatch(TextInputEvent{U'y'}));
  CHECK(gateDestroyed);
  CHECK(safe);
}
