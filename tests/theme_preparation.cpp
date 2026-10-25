// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/background.hpp>
#include <portals/theme_selection.hpp>

#include "support/managed_text_fixture.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <latch>

namespace {
constexpr RgbColor RED{220, 10, 10};
constexpr RgbColor BLUE{10, 10, 220};
constexpr RgbColor GREEN{10, 220, 10};
constexpr std::size_t ACTIVITY_TEST_NODE_LIMIT = 8;

StyleSheet color(RgbColor value) {
  StyleRule rule;

  rule.declarations.foreground = value;

  return {.rules = {rule}};
}

class ReadyTerminal final : public ApplicationReadyTerminal {
public:
  std::size_t pending{};

  TerminalRead poll(std::stop_token) override {
    return {TerminalStatus::DEADLINE};
  }

  std::expected<CellSize, TerminalError> size() const override {
    return CellSize{30, 4};
  }

  std::expected<void, TerminalError> submit(std::string bytes) override {
    pending = bytes.size();

    return {};
  }

  TerminalWrite flushReady(std::stop_token) override {
    return {TerminalStatus::PROGRESS, std::exchange(pending, 0), 0, {}};
  }

  std::expected<void, TerminalError> close() noexcept override {
    return {};
  }
};

// Deterministic host executor: only the test chooses admission/completion order.
// Work and publication still execute on a real worker, with no UI state captured.
class ControlledExecutor final : public BackgroundExecutor {
public:
  struct Item {
    std::shared_ptr<BackgroundJob> job;
    bool ran{};
  };

  std::vector<Item> items;
  std::size_t capacity{2};
  std::uint64_t epoch{};
  std::optional<BackgroundError> rejected;
  unsigned throwMode{};

  std::expected<BackgroundTicket, BackgroundError> submit(std::unique_ptr<BackgroundJob> &job) override {
    if (throwMode == 1) {
      throw std::bad_alloc();
    }

    if (throwMode == 2) {
      throw std::runtime_error("host submit failure");
    }

    if (rejected) {
      return std::unexpected(*rejected);
    }

    if (items.size() >= capacity) {
      return std::unexpected(BackgroundError::WOULD_BLOCK);
    }

    auto owner = std::shared_ptr<BackgroundJob>(std::move(job));

    items.push_back({owner});

    return BackgroundTicket(owner);
  }

  void service() noexcept override {
  }

  std::uint64_t capacityEpoch() const noexcept override {
    return epoch;
  }

  std::expected<void, AsyncError> finish(std::size_t index) {
    auto &item = items[index];
    std::expected<void, AsyncError> result;

    std::thread worker([&] {
      if (!item.ran) {
        item.job->run(item.job->stopToken());
        item.ran = true;
      }

      result = item.job->stopToken().stop_requested()
                 ? std::expected<void, AsyncError>(std::unexpected(AsyncError::CANCELLED))
                 : item.job->publish();
    });

    worker.join();

    if (result || result.error() == AsyncError::CANCELLED || result.error() == AsyncError::CLOSED) {
      items.erase(items.begin() + static_cast<std::ptrdiff_t>(index));
      ++epoch;
    }

    return result;
  }
};

struct WorkerProbe {
  std::atomic<unsigned> redCalls{}, blueCalls{};
  std::thread::id preparedOn;
  std::latch entered{1};
  std::latch release{1};
  bool holdRed{};
};

struct ThreadCopyFactory {
  std::thread::id ui = std::this_thread::get_id();
  bool noMemory{};

  explicit ThreadCopyFactory(bool allocation) : noMemory(allocation) {
  }

  ThreadCopyFactory(const ThreadCopyFactory &other) : ui(other.ui), noMemory(other.noMemory) {
    if (std::this_thread::get_id() != ui) {
      if (noMemory) {
        throw std::bad_alloc();
      }

      throw std::runtime_error("worker callable copy");
    }
  }

  StyleSheet operator()(std::stop_token) {
    return color(RED);
  }
};

struct PreparationFixture {
  ReadyTerminal terminal;
  ui::Ref<Button> button;
  ui::ThemeSelectionRef selection;
  ManagedScene *managed{};
  Application *app{};
  std::shared_ptr<WorkerProbe> worker = std::make_shared<WorkerProbe>();
  std::optional<ui::Scene> scene;

  explicit PreparationFixture(BackgroundExecutor &executor, std::size_t dataCapacity = 16, bool initial = false,
                              bool holdRed = false) {
    ui::SceneOptions options;

    options.application.backgroundExecutor = &executor;
    options.application.scheduler.data_capacity = dataCapacity;
    options.managed.maxNodes = ACTIVITY_TEST_NODE_LIMIT;
    worker->holdRed = holdRed;
    scene.emplace(ui::scene(ui::button(button, "Subject").style({"Button", "subject", {}}), options));
    scene->configure([this](ManagedScene &owner, Application &application) {
      managed = &owner;
      app = &application;
    });
    *scene | ui::themeSelection(
               selection, {{"sync", "Synchronous",
                            [] {
                              return color(GREEN);
                            }},
                           ui::preparedTheme("red", "Prepared red",
                                             [probe = worker](std::stop_token) {
                                               ++probe->redCalls;
                                               probe->preparedOn = std::this_thread::get_id();

                                               if (probe->holdRed) {
                                                 probe->entered.count_down();
                                                 probe->release.wait();
                                               }

                                               return color(RED);
                                             }),
                           ui::preparedTheme("blue", "Prepared blue",
                                             [probe = worker](std::stop_token) {
                                               ++probe->blueCalls;

                                               return color(BLUE);
                                             }),
                           ui::preparedTheme("bad", "Domain failure",
                                             [](std::stop_token) -> std::expected<StyleSheet, StyleError> {
                                               return std::unexpected(StyleError{TreeError::INVALID_ARGUMENT, 7, 11,
                                                                                 "exact static factory error"});
                                             }),
                           ui::preparedTheme("throw", "Thrown failure",
                                             [](std::stop_token) -> StyleSheet {
                                               throw std::runtime_error("worker failure");
                                             }),
                           ui::preparedTheme("copy-throw", "Callable copy failure", ThreadCopyFactory(false)),
                           ui::preparedTheme("copy-oom", "Callable allocation failure", ThreadCopyFactory(true)),
                           ui::preparedTheme("oversized", "Payload limit", [](std::stop_token) {
                             StyleSheet sheet;

                             sheet.rules.resize(MAX_STYLE_RULES + 1);

                             return sheet;
                           })});

    if (initial) {
      scene->configure([this](ManagedScene &) {
        return selection.preview("red");
      });
    }

    take(scene->prepareChecked());
    take(scene->start(terminal));
  }

  ~PreparationFixture() {
    if (scene) {
      (void)scene->close();
    }
  }

  void step() {
    check(!take(scene->step()).finished, "hosted UI remains live");
  }

  std::optional<RgbColor> foreground() {
    std::optional<RgbColor> value;

    take(app->tree().read(button.node(), [&](const Button &subject) {
      value = subject.computedStyle()->normal.foreground;
    }));

    return value;
  }
};

void initialAndWorkerBoundary() {
  ControlledExecutor executor;
  PreparationFixture f(executor, 16, true);

  f.step();
  check(executor.items.size() == 1 && f.worker->redCalls == 0,
        "mount intent admitted only after background becomes ready");
  check(take(f.selection.snapshot())->preparing, "initial prepared preview is observable pending");
  take(executor.finish(0));
  f.step();
  check(f.foreground() == RED && f.worker->redCalls == 1 && f.worker->preparedOn != std::this_thread::get_id(),
        "factory executes once on worker and UI installs its prepared result");
  take(f.selection.commit());
  check(take(f.selection.snapshot())->lastCommittedId == "red", "prepared preview uses ordinary commit history");
}

void selectorPreparedInitialChoice() {
  for (const bool rejected : {false, true}) {
    ControlledExecutor executor;
    ReadyTerminal terminal;
    ui::Ref<ThemeSelector> selector;
    ui::SceneOptions options;
    unsigned committedCallbacks{};

    options.application.backgroundExecutor = &executor;

    ui::ThemeSelectorCatalog catalog;

    catalog.initialId = "initial";
    catalog.choices.push_back(ui::preparedTheme(
      "initial", "Prepared initial", [rejected](std::stop_token) -> std::expected<StyleSheet, StyleError> {
        if (rejected) {
          return std::unexpected(StyleError{TreeError::INVALID_ARGUMENT, 3, 4, "initial preparation rejected"});
        }

        return color(RED);
      }));

    ui::ThemeSelectorOptions selectorOptions;

    selectorOptions.onCommitted = [&](std::string_view) -> std::expected<void, ManagedError> {
      ++committedCallbacks;

      return {};
    };

    auto scene =
      ui::scene(options, ui::label("Prepared initial catalog"), ui::themeSelector(selector, catalog, selectorOptions));

    take(scene.prepareChecked());
    check(take(selector.snapshot())->preparing && executor.items.size() == 1,
          "selector initialId uses the already available borrowed executor without running the factory on UI");
    take(scene.start(terminal));
    check(!take(scene.step()).finished && executor.items.size() == 1, "first cycle admits prepared initial choice");
    take(executor.finish(0));
    check(!take(scene.step()).finished, "prepared initial failure remains recoverable at runtime");

    auto snapshot = take(selector.snapshot());

    if (rejected) {
      check(snapshot->preparationError && snapshot->status == ui::ThemeSelectionStatus::IDLE &&
              !snapshot->lastCommittedId,
            "failed prepared startup never invents a committed choice");
    } else {
      check(snapshot->status == ui::ThemeSelectionStatus::IDLE && snapshot->lastCommittedId == "initial" &&
              !snapshot->previewedId,
            "prepared initial choice commits only after worker installation");
    }

    check(committedCallbacks == 0, "prepared initial commit preserves no-persistence-callback startup semantics");
    take(scene.quit());
    check(take(scene.step()).finished, "selector fixture stops its own hosted session");
  }
}

void nativeDefaultMountRetainsIntent() {
  ui::ThemeSelectionRef selection;
  auto calls = std::make_shared<std::atomic<unsigned>>(0);
  auto scene = ui::scene(ui::label("Default executor starts at native run"));

  scene | ui::themeSelection(selection, {ui::preparedTheme("default", "Native default", [calls](std::stop_token) {
                               ++*calls;

                               return color(RED);
                             })});
  scene.configure([selection](ManagedScene &) {
    return selection.preview("default");
  });
  take(scene.prepareChecked());

  auto snapshot = take(selection.snapshot());

  check(snapshot->preparing && !snapshot->preparationError && *calls == 0,
        "native default mount retains initial intent without a false UNAVAILABLE or inline factory");
  take(scene.close());
}

void latestIntentAndFullAdmission() {
  ControlledExecutor executor;

  executor.capacity = 1;

  PreparationFixture f(executor);

  take(f.selection.preview("red"));
  check(executor.items.size() == 1, "first attempt accepted");
  auto old = executor.items[0].job;

  for (unsigned i = 0; i != 9; ++i) {
    take(f.selection.preview(i == 8 ? "blue" : "red"));
  }

  check(old->stopToken().stop_requested() && executor.items.size() == 1,
        "full admission cancels prior immediately and retains one latest intent");
  check(take(f.selection.snapshot())->requestedId == "blue" && !f.selection.commit(),
        "commit cannot publish an older visible choice as the newest request");
  check(executor.finish(0).error() == AsyncError::CANCELLED, "cancelled old job never publishes");
  f.step();
  check(executor.items.size() == 1, "capacity readiness retries without another preview/key");
  take(executor.finish(0));
  f.step();
  check(f.foreground() == BLUE && f.worker->blueCalls == 1 && f.worker->redCalls == 1,
        "only admitted old plus newest factory execute; latest intent wins");
}

void oldQueuedResultNeverWins() {
  ControlledExecutor executor;
  PreparationFixture f(executor);

  take(f.selection.preview("red"));
  take(executor.finish(0));
  take(f.selection.preview("blue"));
  take(executor.finish(0));
  f.step();
  check(f.foreground() == BLUE && take(f.selection.snapshot())->previewedId == "blue",
        "already-queued old completion is fenced by exact newer intent");
}

void latchedOldWorkerAndNewPreview() {
  auto pool = take(BackgroundPool::create({2, 1}));
  PreparationFixture f(*pool, 16, false, true);

  struct Release {
    std::shared_ptr<WorkerProbe> probe;

    ~Release() {
      probe->release.count_down();
    }
  } release{f.worker};

  take(f.managed->enableStyleLayers());
  take(f.selection.preview("red"));

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);

  while (!f.worker->entered.try_wait() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }

  check(f.worker->entered.try_wait(), "old worker entered the explicit latch");
  take(f.selection.preview("blue"));

  while (take(f.selection.snapshot())->preparing && std::chrono::steady_clock::now() < deadline) {
    f.step();
    std::this_thread::yield();
  }

  check(f.foreground() == BLUE && f.worker->blueCalls == 1 && f.worker->redCalls == 1,
        "new preview installs while cancelled old factory is still held; cancellation never joins");
  take(f.selection.cancel());
}

void publicationRetriesPreparedResult() {
  ControlledExecutor executor;
  PreparationFixture f(executor, 1);
  auto scope = take(f.app->scheduler().makeScope());
  auto blocker = take(f.app->scheduler().connect<int>(scope, [](auto) {
  }));

  take(blocker.publisher.publish(std::make_shared<const int>(1)));
  take(f.selection.preview("red"));
  check(executor.finish(0).error() == AsyncError::WOULD_BLOCK && f.worker->redCalls == 1,
        "full result lane retains prepared sheet");
  f.step();
  take(executor.finish(0));
  f.step();
  check(f.foreground() == RED && f.worker->redCalls == 1, "publication retry never repeats factory");
}

void exactErrorsAndCancelRestoration() {
  ControlledExecutor executor;
  PreparationFixture f(executor);

  take(f.managed->theme(color(BLUE)));
  take(f.selection.preview("sync"));
  take(f.selection.preview("bad"));
  take(executor.finish(0));
  f.step();

  const auto domainSnapshot = take(f.selection.snapshot());
  auto failure = domainSnapshot->preparationError;
  const auto *style = failure ? std::get_if<StyleError>(&failure->cause) : nullptr;

  check(style && style->line == 7 && style->column == 11 && style->message == "exact static factory error" &&
          !failure->async && f.foreground() == GREEN,
        "domain StyleError survives worker transport exactly");
  take(f.selection.preview("throw"));
  take(executor.finish(0));
  f.step();
  failure = take(f.selection.snapshot())->preparationError;
  check(failure && failure->async == AsyncError::CALLBACK_FAILED && f.foreground() == GREEN,
        "worker throw remains separately classified and prior installed preview survives");
  check(domainSnapshot->requestedId == "bad" && domainSnapshot->preparationError &&
          !domainSnapshot->preparationError->async,
        "retained failure snapshots remain immutable after replacing their intent");

  for (const auto id : {"copy-throw", "copy-oom"}) {
    take(f.selection.preview(id));
    take(executor.finish(0));
    f.step();
    failure = take(f.selection.snapshot())->preparationError;

    check(failure && failure->async ==
                       (std::string_view(id) == "copy-oom" ? AsyncError::NO_MEMORY : AsyncError::CALLBACK_FAILED),
          "worker callable copy failures retain their exact async classification");
  }

  take(f.selection.preview("oversized"));
  take(executor.finish(0));
  f.step();
  failure = take(f.selection.snapshot())->preparationError;
  style = failure ? std::get_if<StyleError>(&failure->cause) : nullptr;
  check(style && style->cause == TreeError::INVALID_ARGUMENT && f.foreground() == GREEN,
        "worker checks the finite result rule budget before UI installation");
  take(f.selection.preview("red"));
  auto pending = executor.items[0].job;

  take(f.selection.cancel());
  check(pending->stopToken().stop_requested() && f.foreground() == BLUE && !take(f.selection.snapshot())->requestedId,
        "cancel retires pending work and restores original theme");
}

void cancellationReentryIsGuarded() {
  ControlledExecutor executor;
  PreparationFixture f(executor);

  take(f.selection.preview("red"));

  std::optional<ManagedError> reentry;
  std::stop_callback callback(executor.items[0].job->stopToken(), [&] {
    auto attempted = f.selection.preview("blue");

    if (!attempted) {
      reentry = attempted.error();
    }
  });

  take(f.selection.cancel());

  const auto *style = reentry ? std::get_if<StyleError>(&reentry->cause) : nullptr;

  check(style && style->cause == TreeError::WOULD_BLOCK && !take(f.selection.snapshot())->requestedId,
        "synchronous stop callback cannot reenter and replace the cancellation transition");
}

void failedIdleCancelIsAllocationFree() {
  ControlledExecutor executor;
  PreparationFixture f(executor);

  take(f.selection.preview("bad"));
  take(executor.finish(0));
  f.step();

  const auto failed = take(f.selection.snapshot());

  allocationTest::remaining = 0;

  auto cancelled = f.selection.cancel();

  allocationTest::remaining = -1;

  take(cancelled);
  check(!take(f.selection.snapshot())->preparationError && failed->preparationError && failed->requestedId == "bad",
        "idle cancel restores its prior immutable snapshot without allocation or mutating retained failure");
}

void executorFailuresStayTyped() {
  for (unsigned mode = 0; mode != 3; ++mode) {
    ControlledExecutor executor;
    PreparationFixture f(executor);

    executor.throwMode = mode;

    if (!mode) {
      executor.rejected = BackgroundError::CLOSED;
    }

    auto rejected = f.selection.preview("red");

    check(!rejected && !take(f.selection.snapshot())->preparing && f.worker->redCalls == 0,
          "terminal admission failure preserves the requested identity without running work");

    if (!mode) {
      check(rejected.error().background == BackgroundError::CLOSED && !rejected.error().async,
            "returned executor error retains its exact BackgroundError");
    } else {
      check(rejected.error().async == (mode == 1 ? AsyncError::NO_MEMORY : AsyncError::CALLBACK_FAILED) &&
              !rejected.error().background,
            "thrown host submission remains an async exception classification");
    }
  }

  ControlledExecutor executor;

  executor.capacity = 0;

  PreparationFixture f(executor);

  take(f.selection.preview("red"));
  executor.throwMode = 2;
  ++executor.epoch;
  f.step();
  check(take(f.selection.snapshot())->preparationError->async == AsyncError::CALLBACK_FAILED,
        "noexcept activity service translates a thrown host submit without terminating");
}

void overridesAndExternalAuthority() {
  ControlledExecutor executor;
  PreparationFixture f(executor);

  take(f.selection.preview("red"));
  auto user = take(f.managed->addLayer(StyleTier::USER, "user", color(GREEN)));

  take(executor.finish(0));
  f.step();
  check(take(f.selection.snapshot())->previewedId == "red" && f.foreground() == GREEN,
        "intervening USER revision is preserved and does not reject THEME installation");
  take(f.managed->removeLayer(user));
  check(f.foreground() == RED, "removing USER layer reveals correctly installed prepared theme");
  take(f.selection.preview("blue"));
  take(f.managed->theme(color(GREEN)));
  take(executor.finish(0));
  f.step();
  check(f.foreground() == GREEN && take(f.selection.snapshot())->preparationError.has_value(),
        "external THEME ownership handoff rejects late old authority");
}

void disposalAndPayloadBudget() {
  ControlledExecutor executor;

  {
    PreparationFixture f(executor);

    take(f.selection.preview("red"));
    take(executor.finish(0));
    take(f.scene->quit());
    check(take(f.scene->step()).finished, "quit retires the active hosted scene");
    check(!f.selection.snapshot(), "scene close retires queued completion registration");
    f.scene.reset();
  }

  {
    PreparationFixture f(executor);

    take(f.selection.preview("red"));
    auto job = executor.items[0].job;

    f.scene.reset();
    check(job->stopToken().stop_requested(), "scene disposal cancels accepted job without worker join");
    check(executor.finish(0).error() == AsyncError::CANCELLED, "disposed scene cannot receive worker completion");
  }
}
} // namespace

int main() try {
  initialAndWorkerBoundary();
  selectorPreparedInitialChoice();
  nativeDefaultMountRetainsIntent();
  latestIntentAndFullAdmission();
  oldQueuedResultNeverWins();
  latchedOldWorkerAndNewPreview();
  publicationRetriesPreparedResult();
  exactErrorsAndCancelRestoration();
  cancellationReentryIsGuarded();
  failedIdleCancelIsAllocationFree();
  executorFailuresStayTyped();
  overridesAndExternalAuthority();
  disposalAndPayloadBudget();

  return 0;
} catch (const std::exception &failure) {
  std::cerr << failure.what() << '\n';

  return 1;
}
