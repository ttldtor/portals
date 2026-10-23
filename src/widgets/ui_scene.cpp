// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_style.hpp>

#include "dialog_state.hpp"

#include <array>
#include <cstdio>
#include <limits>

namespace portals::ui {

namespace {
ManagedError error(ManagedPhase phase, TreeError cause, NodeId node = {}) {
  return {phase, node, cause, {}, {}};
}

TreeError cleanupCause(const ManagedError &failure) {
  if (const auto *cause = std::get_if<TreeError>(&failure.cause)) {
    return *cause;
  }

  return TreeError::CALLBACK_FAILED;
}

std::size_t saturatingCount(std::size_t first, std::size_t second) noexcept {
  constexpr auto MAX_COUNT = std::numeric_limits<std::size_t>::max();

  return second > MAX_COUNT - first ? MAX_COUNT : first + second;
}

} // namespace

thread_local detail::FactoryCleanup *detail::FactoryCleanupScope::current_{};

detail::FactoryCleanupScope::FactoryCleanupScope(FactoryCleanup *collector) noexcept : previous_(current_) {
  current_ = collector;
}

detail::FactoryCleanupScope::~FactoryCleanupScope() {
  current_ = previous_;
}

void detail::FactoryCleanupScope::retain(const ManagedError &failure) noexcept {
  if (!current_) {
    return;
  }

  if (!current_->failure) {
    current_->failure = failure;
  } else {
    current_->droppedErrors = saturatingCount(current_->droppedErrors, 1);
  }
}

bool detail::FactoryCleanupScope::capturing() noexcept {
  return current_ != nullptr;
}

std::size_t detail::FactoryCleanup::attach(ManagedError &primary) const noexcept {
  auto dropped = droppedErrors;

  if (!failure) {
    return dropped;
  }

  if (!primary.cleanup) {
    primary.cleanup = cleanupCause(*failure);
  } else {
    dropped = saturatingCount(dropped, 1);
  }

  // The primary and one cleanup cause fit the established envelope; count any further independent cause.
  if (failure->cleanup) {
    dropped = saturatingCount(dropped, 1);
  }

  if (!primary.application) {
    if (failure->application) {
      primary.application = failure->application;
    } else if (const auto *application = std::get_if<ApplicationError>(&failure->cause)) {
      primary.application = *application;
    }
  }

  return dropped;
}

struct Scene::State {
  enum class Operation {
    NONE,
    PREFLIGHT,
    PREPARE,
    CONFIGURE,
    LOOKUP,
    RUN,
    STOP,
    CLOSE,
    NOTIFY,
    REPORT
  };

  struct Guard {
    State &state;
    Operation previous;

    Guard(State &value, Operation operation) : state(value), previous(value.active) {
      state.active = operation;
    }

    ~Guard() {
      state.active = previous;
    }
  };

  struct Pending {
    Configuration callback;
    std::source_location origin;
  };

  SceneOptions options;
  Configuration recipe;
  std::function<ApplicationOptions()> applicationOptions;
  std::source_location applicationOptionsOrigin;
  Stop stopCallback;
  std::source_location stopOrigin;
  std::source_location recipeOrigin;
  std::unique_ptr<Application> application;
  std::unique_ptr<ManagedScene> managed;
  std::vector<Pending> pending = std::vector<Pending>(0);
  std::unique_ptr<StyleSheet> theme;
  std::shared_ptr<ManagedErrorHandler> observer;
  std::shared_ptr<SceneReportHandler> reportSink;
  std::optional<ManagedError> failure;
  std::optional<ManagedError> stopFailure;
  std::source_location failureOrigin;
  ManagedDiagnostic fallback;
  Operation active{Operation::NONE};
  bool prepared{}, completed{}, managedClosed{}, notified{}, fallbackObserverFailed{};
  bool publishingFailure{};
  bool quitRequested{}, finalReported{};
  std::optional<int> preflightStatus;
  bool stopped{};
  bool sessionStarted{}, sessionActive{};
  std::optional<std::expected<ApplicationStep, ManagedError>> sessionResult;
  bool shortFailure{}, shortDeliveryPending{};
  bool primaryRecorded{};
  const void *primaryShortAttempt{};

  struct NamedRequest {
    NamedRequest() = default;

    // Copies remain fallible inside the admitted boundary, including vector reallocation.
    NamedRequest(const detail::LookupSource &request, const detail::LookupPublisher &publication)
        : source(request), publish(publication) {
    }

    ~NamedRequest() = default;

    detail::LookupSource source;
    detail::LookupPublisher publish;
  };

  std::vector<NamedRequest> lookups = std::vector<NamedRequest>(0);

  std::expected<void, ManagedError> resolve(const detail::LookupSource &source, detail::LookupPublisher &publish) {
    auto result = [&]() -> std::expected<void, ManagedError> {
      try {
        auto found = source.observation ? detail::NamedAccess::observe(*managed, source.observation)
                                        : detail::NamedAccess::find(*managed, source.name);

        return publish(*managed, found);
      } catch (const std::bad_alloc &) {
        return std::unexpected(error(ManagedPhase::BIND, TreeError::NO_MEMORY));
      } catch (...) {
        return std::unexpected(error(ManagedPhase::BIND, TreeError::CALLBACK_FAILED));
      }
    }();

    if (!result) {
      if (!prepared) {
        // Latch the startup cause before observer reentry; one publication owns its notification.
        fail(result.error(), "scene.get", source.origin, false);
        publishingFailure = true;
      }

      detail::NamedAccess::report(*managed, result.error(), source);
      publishingFailure = false;
    }

    return result;
  }

  struct StyleBinding {
    std::weak_ptr<const void> identity;
    StyleLayerHandle handle;
  };

  std::shared_ptr<detail::SceneAnchor> anchor = std::make_shared<detail::SceneAnchor>();
  std::vector<StyleBinding> styles = std::vector<StyleBinding>(0);
  std::vector<std::shared_ptr<detail::LiveBinding>> live = std::vector<std::shared_ptr<detail::LiveBinding>>(0);

  void deliverReport(const SceneReport &report, std::shared_ptr<SceneReportHandler> overrideSink = {}) noexcept {
    Guard guard(*this, Operation::REPORT);
    auto sink = overrideSink ? std::move(overrideSink) : reportSink;

    try {
      if (sink) {
        (*sink)(report);
      } else {
        reportToStderr(report);
      }
    } catch (...) {
      reportToStderr(report);
    }
  }

  void applyApplicationOptions() {
    if (!applicationOptions) {
      return;
    }

    auto provider = std::move(applicationOptions);

    try {
      options.application = provider();
    } catch (const std::bad_alloc &) {
      fail(error(ManagedPhase::PREPARE, TreeError::NO_MEMORY, node()), "scene.applicationOptions",
           applicationOptionsOrigin);
    } catch (...) {
      fail(error(ManagedPhase::PREPARE, TreeError::CALLBACK_FAILED, node()), "scene.applicationOptions",
           applicationOptionsOrigin);
    }

    // Retire owning captures while PREPARE still excludes lifecycle reentry.
    provider = {};
  }

  static std::expected<void, ManagedError> lookupBridge(void *raw, const detail::LookupSource &source,
                                                        detail::LookupPublisher publish) {
    auto &state = *static_cast<State *>(raw);

    if (state.failure) {
      return std::unexpected(*state.failure);
    }

    if (state.completed || state.active == State::Operation::REPORT || state.active == State::Operation::STOP ||
        state.active == State::Operation::CLOSE || state.active == State::Operation::NOTIFY ||
        state.active == State::Operation::PREPARE || state.active == State::Operation::LOOKUP) {
      return std::unexpected(error(ManagedPhase::BIND, TreeError::WOULD_BLOCK));
    }

    State::Guard guard(state, State::Operation::LOOKUP);

    try {
      if (state.prepared) {
        return state.resolve(source, publish);
      }

      if (state.lookups.size() >= state.options.maxConfigurations) {
        state.fail(error(ManagedPhase::BIND, TreeError::NODE_LIMIT), "scene.get", source.origin);

        return std::unexpected(*state.failure);
      }

      state.lookups.emplace_back(source, publish);

      return {};
    } catch (const std::bad_alloc &) {
      state.fail(error(ManagedPhase::BIND, TreeError::NO_MEMORY), "scene.get", source.origin);

      return std::unexpected(*state.failure);
    }
  }

  std::expected<void, ManagedError> requestQuit() {
    if (active == Operation::NOTIFY || active == Operation::REPORT || active == Operation::STOP ||
        active == Operation::CLOSE) {
      return std::unexpected(error(ManagedPhase::RUN, TreeError::WOULD_BLOCK, node()));
    }

    if (completed) {
      return std::unexpected(error(ManagedPhase::RUN, TreeError::CLOSED, node()));
    }

    quitRequested = true;

    if (prepared && active != Operation::PREPARE) {
      application->requestStop();
    }

    return {};
  }

  NodeId node() const {
    return managed ? managed->root().id() : NodeId{};
  }

  void observe(const ManagedDiagnostic &diagnostic) {
    detail::CurrentSceneScope mask;
    Guard guard(*this, Operation::NOTIFY);
    auto callback = observer;
    detail::CurrentSceneScope scope(anchor);

    // Latch an exact managed primary before user observer reentry can introduce a boundary error.
    if (!failure && guard.previous == Operation::RUN && detail::SceneAccess::notifyingRunFailure(*managed)) {
      fail(diagnostic.error, "scene.run", diagnostic.origin, false);
      primaryRecorded = detail::SceneAccess::primaryRecorded(*managed);
      notified = true;
    }

    if (publishingFailure) {
      notified = true;
    }

    if (callback) {
      (*callback)(diagnostic);
    }
  }

  std::expected<void, ManagedError> installObserver() {
    return managed->onError(observer ? ManagedErrorHandler{[this](const ManagedDiagnostic &diagnostic) {
      observe(diagnostic);
    }}
                                     : ManagedErrorHandler{});
  }

  void notifyFallback() noexcept {
    detail::CurrentSceneScope mask;
    if (!failure || notified || primaryRecorded || !observer || active == Operation::NOTIFY) {
      return;
    }

    Guard guard(*this, Operation::NOTIFY);
    auto callback = observer;
    detail::CurrentSceneScope scope(anchor);
    fallback.error = *failure;
    fallback.origin = failureOrigin;
    notified = true;

    try {
      (*callback)(fallback);
    } catch (...) {
      fallbackObserverFailed = true;
    }
  }

  void fail(ManagedError cause, const char *operation, std::source_location origin, bool report = true) {
    // Reporting is read-only: rejected final-sink reentry cannot alter a completed outcome or notify reactions.
    if (failure || active == Operation::REPORT || active == Operation::STOP) {
      return;
    }

    if (!cause.operation) {
      cause.operation = operation;
      cause.operationOrigin = origin;
    }

    if (!cause.execution) {
      cause.execution = operation;
    }

    failure = std::move(cause);
    failureOrigin = failure->operationOrigin.value_or(origin);

    if (!report) {
      return;
    }

    if (managed && detail::SceneAccess::shortReported(*managed, *failure)) {
      return;
    }

    publishFailure(operation, origin);
  }

  void publishFailure(const char *operation, std::source_location origin) {
    if (managed && !managedClosed) {
      bool entered{};

      try {
        std::array<ManagedDiagnosticContext, 1> context;

        context[0].operation = operation;
        context[0].origin = origin;
        publishingFailure = true;
        entered = managed->reportError(*failure, context, failureOrigin).has_value();
        publishingFailure = false;
      } catch (const std::bad_alloc &) {
        publishingFailure = true;
        entered = managed->reportError(*failure, {}, failureOrigin).has_value();
        publishingFailure = false;
      }

      if (!entered) {
        notifyFallback();
      }
    } else {
      notifyFallback();
    }
  }

  static void setupEvent(void *publication, detail::SetupFailureEvent event, ManagedError *cause,
                         const void *attempt) noexcept {
    auto &anchor = *static_cast<detail::SceneAnchor *>(publication);

    if (anchor.owner != std::this_thread::get_id() || !anchor.state) {
      return;
    }

    auto &state = *static_cast<State *>(anchor.state);

    switch (event) {
    case detail::SetupFailureEvent::LATCH:
      if (!state.failure && !state.completed && state.active != Operation::REPORT) {
        state.fail(*cause, cause->execution, cause->operationOrigin.value_or(std::source_location{}), false);
        const bool entered = state.managed && detail::SceneAccess::shortReported(*state.managed, *cause);

        state.shortFailure = true;
        state.shortDeliveryPending = !entered;
        state.primaryShortAttempt = entered ? attempt : nullptr;
      }
      break;
    case detail::SetupFailureEvent::NOTIFY:
      if (attempt && state.primaryShortAttempt == attempt) {
        // Notification identity is synchronous, separate from whether bounded history retained the error.
        state.notified = true;
      }
      break;
    case detail::SetupFailureEvent::END_ATTEMPT:
      if (attempt && state.primaryShortAttempt == attempt) {
        state.primaryShortAttempt = nullptr;
      }
      break;
    case detail::SetupFailureEvent::END_SCOPE:
      state.primaryShortAttempt = nullptr;

      if (state.shortDeliveryPending) {
        state.shortDeliveryPending = false;
        state.publishFailure(state.failure->execution, state.failureOrigin);
      }

      if (state.shortFailure) {
        state.notifyFallback();
      }
      break;
    }
  }

  std::expected<void, ManagedError> stop() {
    detail::SetupFailureScope setupMask({}, nullptr, nullptr);
    detail::CurrentSceneScope currentMask;

    if (stopped) {
      return {};
    }

    stopped = true;

    Guard guard(*this, Operation::STOP);
    auto callback = std::move(stopCallback);
    std::expected<void, ManagedError> result;

    try {
      if (callback) {
        result = callback();
      }
    } catch (const std::bad_alloc &) {
      result = std::unexpected(error(ManagedPhase::CLEANUP, TreeError::NO_MEMORY, node()));
    } catch (...) {
      result = std::unexpected(error(ManagedPhase::CLEANUP, TreeError::CALLBACK_FAILED, node()));
    }

    if (!result) {
      if (!result.error().operation) {
        result.error().operation = "scene.stop";
        result.error().operationOrigin = stopOrigin;
      }

      result.error().execution = "scene.stop";
    }

    return result;
  }

  std::expected<void, ManagedError> cleanup(std::source_location origin) {
    detail::SetupFailureScope cleanupScope({}, nullptr, nullptr);

    if (sessionActive) {
      // Configuration can fail between host callbacks. Finish the borrowed application
      // before revoking publication or retiring its scene, just as the native run does.
      Guard guard(*this, Operation::RUN);
      detail::CurrentSceneScope current(anchor);

      application->requestStop();

      auto finished = finishStep(managed->step(), origin);

      return finished ? std::expected<void, ManagedError>{} : std::unexpected(finished.error());
    }

    completed = true;

    // Revoke observing publication and actual live controllers before managed input captures retire.
    if (anchor) {
      anchor->state = nullptr;
      anchor->events->lifetime.reset();
    }

    anchor.reset();
    live.clear();
    styles.clear();

    auto result = managed ? managed->close() : stop();

    if (!result) {
      const auto cause = cleanupCause(result.error());

      if (failure) {
        if (!managed) {
          // Pre-owner failures have no managed collector; retain one exact independent shutdown cause.
          stopFailure = result.error();
        }

        failure->cleanup = cause;
      } else {
        fail(result.error(), "scene.close", origin);
      }
    } else {
      managedClosed = true;
    }

    if (managed) {
      detail::SceneAccess::retainCloseFailure(*managed);
    }

    // Publish completion before captures retire: destructors can reenter the facade.
    recipe = {};
    applicationOptions = {};
    pending.clear();
    lookups.clear();
    theme.reset();

    return result;
  }

  std::expected<ApplicationStats, ManagedError> finishRun(std::expected<ApplicationStats, ManagedError> result,
                                                          std::source_location origin) {
    if (!result) {
      if (failure) {
        if (result.error().application) {
          failure->application = result.error().application;
        }

        if (result.error().cleanup) {
          failure->cleanup = result.error().cleanup;
        }
      } else {
        fail(result.error(), "scene.run", origin, false);
        primaryRecorded = detail::SceneAccess::primaryRecorded(*managed);
        notified = detail::SceneAccess::runFailureNotified(*managed);
      }
    }

    // Native open can fail before scene.stop; both drivers require this final cleanup.
    (void)cleanup(origin);
    notifyFallback();

    return failure ? std::unexpected(*failure) : result;
  }

  std::expected<ApplicationStep, ManagedError> finishStep(std::expected<ApplicationStep, ManagedError> result,
                                                          std::source_location origin) {
    if (result && !result->finished) {
      return *result;
    }

    sessionActive = false;

    auto finished = finishRun(
      result ? std::expected<ApplicationStats, ManagedError>(result->stats) : std::unexpected(result.error()), origin);

    if (finished) {
      sessionResult.emplace(ApplicationStep{true, *finished});
    } else {
      sessionResult.emplace(std::unexpected(finished.error()));
    }

    return *sessionResult;
  }

  void invoke(Configuration &callback, std::source_location origin) {
    detail::SetupFailureScope scope(active == Operation::RUN ? std::weak_ptr<void>{} : anchor,
                                    active == Operation::RUN ? nullptr : setupEvent, "scene.configure");
    detail::CurrentSceneScope current(anchor);

    try {
      auto result = callback(*managed);

      if (!result) {
        fail(result.error(), "scene.configure", origin);
      }
    } catch (const std::bad_alloc &) {
      fail(error(ManagedPhase::BIND, TreeError::NO_MEMORY, node()), "scene.configure", origin);
    } catch (...) {
      fail(error(ManagedPhase::BIND, TreeError::CALLBACK_FAILED, node()), "scene.configure", origin);
    }
  }
};

Application &Scene::admittedApplication(ManagedScene &scene) noexcept {
  return detail::SceneAccess::application(scene);
}

std::expected<Node<OverlayHost>, ManagedError> detail::Access::layerHost(ManagedScene &scene, Node<Widget> root) {
  auto allowed = check(scene, root);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  const auto &host = scene.dialogs_;

  if (!host || host->retired || !host->ready || host->root != root) {
    return std::unexpected(detail::error(root, TreeError::INVALID_ARGUMENT));
  }

  return host->root;
}

std::expected<void, ManagedError> detail::Access::checkLayer(ManagedScene &scene, Node<Widget> node) {
  auto allowed = check(scene, node);

  if (!allowed) {
    return allowed;
  }

  bool exact{};
  auto read = scene.app_.tree().read(node, [&](const Widget &widget) {
    exact = typeid(widget) == typeid(Dialog);
  });

  if (!read) {
    return std::unexpected(detail::error(node, read.error()));
  }

  if (!exact || !scene.findDialog(node)) {
    return std::unexpected(detail::error(node, TreeError::INVALID_ARGUMENT));
  }

  return {};
}

detail::SetupFailureScope Scene::setupScope(const char *execution) noexcept {
  if (owner_ != std::this_thread::get_id() || !state_) {
    return detail::SetupFailureScope({}, nullptr, execution);
  }

  return detail::SetupFailureScope(state_->anchor, State::setupEvent, execution);
}

Scene::Scene(Configuration recipe, SceneOptions options, std::source_location origin)
    : owner_(std::this_thread::get_id()), state_(std::make_unique<State>()) {
  state_->options = options;
  state_->recipe = std::move(recipe);
  state_->recipeOrigin = origin;
  state_->anchor->state = state_.get();
  state_->anchor->lookup = &State::lookupBridge;
  state_->anchor->quit = [](void *raw, std::source_location) {
    return static_cast<State *>(raw)->requestQuit();
  };
}

DynamicRef Scene::get(std::string_view name, std::source_location origin) {
  return DynamicRef({ref().anchor_, owner_, std::string(name), origin});
}

std::expected<void, ManagedError> detail::requestLookup(const LookupSource &source, LookupPublisher publish) {
  if (source.owner != std::this_thread::get_id()) {
    return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::WRONG_THREAD, {}, {}});
  }

  const auto anchor = source.anchor.lock();

  if (!anchor || !anchor->state || !anchor->lookup) {
    return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::CLOSED, {}, {}});
  }

  return anchor->lookup(anchor->state, source, std::move(publish));
}

Scene::Scene(Scene &&other) noexcept : owner_(other.owner_) {
  if (owner_ != std::this_thread::get_id() || (other.state_ && other.state_->active != State::Operation::NONE)) {
    std::terminate();
  }

  state_ = std::move(other.state_);
}

Scene::~Scene() {
  if (!state_) {
    return;
  }

  if (owner_ != std::this_thread::get_id() || state_->active != State::Operation::NONE) {
    std::terminate();
  }

  const bool collectCleanup = detail::FactoryCleanupScope::capturing();
  std::optional<ManagedError> abandonedFailure;

  {
    detail::FactoryCleanupScope cleanupMask;
    detail::SetupFailureScope lifecycleScope({}, nullptr, nullptr);

    if (state_->sessionActive) {
      State::Guard guard(*state_, State::Operation::RUN);
      detail::CurrentSceneScope current(state_->anchor);

      state_->application->requestStop();
      (void)state_->finishStep(state_->managed->step(), std::source_location::current());
    }

    {
      State::Guard guard(*state_, State::Operation::CLOSE);

      const auto cleanup = state_->cleanup(std::source_location::current());

      if (collectCleanup && !cleanup && !state_->finalReported) {
        abandonedFailure = cleanup.error();
      }

      state_->observer.reset();
      state_->reportSink.reset();

      // Detach each public-facing owner before its user captures retire, while all State fields live.
      auto managed = std::move(state_->managed);

      managed.reset();

      auto application = std::move(state_->application);

      application.reset();
      state_->options.application.scheduler.diagnostics.reset();
    }

    // Retire passive diagnostic identities under the same mask, after guarded user services are gone.
    state_.reset();
  }

  if (abandonedFailure) {
    detail::FactoryCleanupScope::retain(*abandonedFailure);
  }
}

std::expected<void, ManagedError> Scene::checked() const {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(error(ManagedPhase::PREPARE, TreeError::WRONG_THREAD));
  }

  if (!state_) {
    return std::unexpected(error(ManagedPhase::PREPARE, TreeError::CLOSED));
  }

  return state_->failure ? std::unexpected(*state_->failure) : std::expected<void, ManagedError>{};
}

std::expected<void, ManagedError> Scene::factoryResultChecked() const {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(error(ManagedPhase::PREPARE, TreeError::WRONG_THREAD));
  }

  if (!state_) {
    return std::unexpected(error(ManagedPhase::PREPARE, TreeError::CLOSED));
  }

  if (state_->active != State::Operation::NONE || state_->sessionActive) {
    return std::unexpected(error(ManagedPhase::PREPARE, TreeError::WOULD_BLOCK));
  }

  if (state_->completed || state_->finalReported) {
    return std::unexpected(state_->failure.value_or(error(ManagedPhase::PREPARE, TreeError::CLOSED)));
  }

  return {};
}

std::size_t Scene::applyFactoryCleanup(const detail::FactoryCleanup &cleanup, std::source_location origin) {
  if (!cleanup.failure || owner_ != std::this_thread::get_id() || !state_) {
    return cleanup.droppedErrors;
  }

  auto &state = *state_;
  auto dropped = cleanup.droppedErrors;

  if (state.failure) {
    dropped = cleanup.attach(*state.failure);
  } else {
    state.fail(*cleanup.failure, "scene.factory", origin);
  }

  // Preserve the whole first envelope, not just its cause: normal cleanup may fill or overwrite its slot.
  auto retained = *state.failure;

  if (state.active == State::Operation::NONE && !state.completed) {
    State::Guard guard(state, State::Operation::CONFIGURE);
    const auto closed = state.cleanup(origin);

    if (!closed) {
      detail::FactoryCleanup closing;

      closing.failure = closed.error();
      dropped = saturatingCount(dropped, closing.attach(retained));
    }

    state.failure = std::move(retained);
  }

  return dropped;
}

bool Scene::deliverFactoryReport(const SceneReport &report, std::shared_ptr<SceneReportHandler> sink) {
  if (owner_ != std::this_thread::get_id() || !state_ || state_->active != State::Operation::NONE) {
    return false;
  }

  state_->deliverReport(report, std::move(sink));

  return true;
}

std::expected<std::reference_wrapper<ManagedScene>, ManagedError> Scene::managed() const {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(error(ManagedPhase::PREPARE, TreeError::WRONG_THREAD));
  }

  if (!state_ || !state_->prepared || state_->completed) {
    return std::unexpected(error(ManagedPhase::PREPARE, TreeError::CLOSED));
  }

  return std::ref(*state_->managed);
}

std::expected<ManagedDiagnosticBatch, TreeError> Scene::diagnostics() const {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(TreeError::WRONG_THREAD);
  }

  if (!state_) {
    return std::unexpected(TreeError::CLOSED);
  }

  auto &state = *state_;

  try {
    // Older MSVC introduces a noexcept snapshot move when these prvalues use a conditional expression.
    auto result = [&]() -> std::expected<ManagedDiagnosticBatch, TreeError> {
      if (state.managed) {
        return state.managed->diagnostics();
      }

      return std::expected<ManagedDiagnosticBatch, TreeError>(std::in_place);
    }();

    if (!result) {
      return std::unexpected(result.error());
    }

    if (!state.managed && state.failure) {
      ManagedDiagnostic diagnostic;

      diagnostic.error = *state.failure;
      diagnostic.origin = state.failureOrigin;
      diagnostic.observerFailed = state.fallbackObserverFailed;
      result->errors.push_back(diagnostic);

      if (state.stopFailure) {
        if (result->errors.size() < state.options.managed.diagnostics.maxErrors) {
          ManagedDiagnostic shutdown;

          shutdown.error = *state.stopFailure;
          shutdown.origin = shutdown.error.operationOrigin.value_or(state.stopOrigin);
          result->errors.push_back(shutdown);
        } else {
          ++result->droppedErrors;
        }
      }
    }

    if (state.fallbackObserverFailed) {
      ++result->observerFailures;
    }

    result->closed = state.completed;

    // Copy into expected directly: MSVC Debug's noexcept vector move may allocate a proxy.
    return std::expected<ManagedDiagnosticBatch, TreeError>(std::in_place, *result);
  } catch (const std::bad_alloc &) {
    return std::unexpected(TreeError::NO_MEMORY);
  }
}

Scene &Scene::prepare(std::source_location origin) & {
  (void)prepareChecked(origin);

  return *this;
}

std::expected<void, ManagedError> Scene::prepareChecked(std::source_location origin) {
  if (owner_ != std::this_thread::get_id() || !state_) {
    return checked();
  }

  detail::SetupFailureScope lifecycleScope({}, nullptr, nullptr);
  auto &state = *state_;

  if (state.active != State::Operation::NONE || state.sessionActive) {
    auto cause = error(ManagedPhase::PREPARE, TreeError::WOULD_BLOCK, state.node());

    state.fail(cause, "scene.prepare", origin);

    return std::unexpected(cause);
  }

  if (state.failure) {
    return std::unexpected(*state.failure);
  }

  if (state.completed) {
    return std::unexpected(error(ManagedPhase::PREPARE, TreeError::CLOSED, state.node()));
  }

  if (state.prepared) {
    return {};
  }

  State::Guard guard(state, State::Operation::PREPARE);

  try {
    if (!state.options.maxConfigurations || !state.recipe) {
      state.fail(error(ManagedPhase::PREPARE, TreeError::INVALID_ARGUMENT), "scene.prepare", origin);
    } else {
      state.applyApplicationOptions();

      if (state.failure) {
        (void)state.cleanup(origin);
        state.notifyFallback();

        return std::unexpected(*state.failure);
      }

      auto application = Application::create(state.options.application, state.options.clock);

      if (!application) {
        state.fail({ManagedPhase::PREPARE, {}, application.error(), {}, {}}, "scene.application", origin);
      } else {
        state.application = std::move(*application);

        auto managed = ManagedScene::create(*state.application, state.options.managed);

        if (!managed) {
          state.fail(managed.error(), "scene.owner", origin);
        } else {
          state.managed = std::move(*managed);
          detail::SceneAccess::beforeClose(*state.managed, &state, [](void *context) {
            return static_cast<State *>(context)->stop();
          });
          detail::SceneAccess::currentOwner(*state.managed, state.anchor);
          state.anchor->events->scene = state.managed.get();
          state.anchor->events->lifetime = style::ct::detail::Access::lifetime(*state.managed);
          state.anchor->events->node = state.managed->root();

          auto observer = state.installObserver();

          if (!observer) {
            state.fail(observer.error(), "scene.onError", origin);
          }

          if (!state.failure && state.theme) {
            auto applied = state.managed->theme(std::move(*state.theme));

            if (!applied) {
              state.fail(applied.error(), "scene.theme", origin);
            }
          }

          if (!state.failure) {
            auto mounted = [&] {
              detail::CurrentSceneScope current(state.anchor);

              return state.recipe(*state.managed);
            }();

            if (!mounted) {
              state.fail(mounted.error(), "scene.mount", state.recipeOrigin);
            }
          }

          if (!state.failure) {
            for (auto current = state.lookups.begin(); current != state.lookups.end(); ++current) {
              auto resolved = state.resolve(current->source, current->publish);

              if (!resolved) {
                // A dynamic observation and its typed conversion may queue the same name.
                // Preserve the primary failure in those unpublished slots without reporting twice.
                for (auto remaining = std::next(current); remaining != state.lookups.end(); ++remaining) {
                  if (remaining->source.name == current->source.name) {
                    (void)remaining->publish(*state.managed, std::unexpected(resolved.error()));
                  }
                }

                break;
              }
            }
          }

          // Ref publication belongs to ui::mount; all deferred configuration starts afterwards.
          if (!state.failure) {
            state.prepared = true;

            for (auto &pending : state.pending) {
              state.invoke(pending.callback, pending.origin);

              if (state.failure) {
                break;
              }
            }
            if (!state.failure) {
              detail::SetupFailureScope setup(state.anchor, State::setupEvent, "scene.themeSelector");
              detail::CurrentSceneScope current(state.anchor);
              auto attached = detail::SceneAccess::attachThemeSelector(*state.managed, &state.failure);

              if (!state.failure && !attached) {
                state.fail(attached.error(), "scene.themeSelector", origin);
              }
            }
          }
        }
      }
    }
  } catch (const std::bad_alloc &) {
    state.fail(error(ManagedPhase::MOUNT, TreeError::NO_MEMORY, state.node()), "scene.mount", state.recipeOrigin);
  } catch (...) {
    state.fail(error(ManagedPhase::MOUNT, TreeError::CALLBACK_FAILED, state.node()), "scene.mount", state.recipeOrigin);
  }

  if (state.failure) {
    (void)state.cleanup(origin);
    state.notifyFallback();
  } else {
    state.recipe = {};
    state.pending.clear();
    state.lookups.clear();
    state.theme.reset();

    if (state.failure) {
      (void)state.cleanup(origin);
    } else if (state.quitRequested) {
      state.application->requestStop();
    }
  }

  return checked();
}

bool Scene::admitConfiguration(bool runtime, std::source_location origin) {
  if (owner_ != std::this_thread::get_id() || !state_) {
    return false;
  }

  auto &state = *state_;

  if ((state.active != State::Operation::NONE && !(runtime && state.active == State::Operation::RUN)) ||
      (state.sessionActive && !runtime)) {
    state.fail(error(ManagedPhase::BIND, TreeError::WOULD_BLOCK, state.node()), "scene.configure", origin);

    return false;
  }

  if (state.failure) {
    return false;
  }

  if (state.completed) {
    state.fail(error(ManagedPhase::BIND, TreeError::CLOSED, state.node()), "scene.configure", origin);

    return false;
  }

  return true;
}

void Scene::configurationException(TreeError cause, std::source_location origin) {
  if (owner_ == std::this_thread::get_id() && state_) {
    state_->fail(error(ManagedPhase::BIND, cause, state_->node()), "scene.configure", origin);

    if (state_->prepared && state_->active == State::Operation::NONE) {
      State::Guard guard(*state_, State::Operation::CONFIGURE);

      (void)state_->cleanup(origin);
    }
  }
}

void Scene::shortcutConfigurationFailure(ShortcutError cause, std::source_location origin) {
  if (owner_ == std::this_thread::get_id() && state_) {
    const auto treeCause =
      cause.cause == ShortcutErrorKind::NO_MEMORY ? TreeError::NO_MEMORY : TreeError::INVALID_ARGUMENT;
    auto failure = error(ManagedPhase::BIND, treeCause, state_->node());

    failure.shortcut = cause;
    state_->fail(std::move(failure), "scene.onKey", origin);

    if (state_->prepared && state_->active == State::Operation::NONE) {
      State::Guard guard(*state_, State::Operation::CONFIGURE);

      (void)state_->cleanup(origin);
    }
  }
}

void Scene::addConfiguration(Configuration callback, bool runtime, std::source_location origin) {
  detail::SetupFailureScope configurationScope({}, nullptr, nullptr);
  auto &state = *state_;

  if (state.prepared) {
    if (runtime && (state.active == State::Operation::RUN || state.sessionActive)) {
      State::Guard guard(state, State::Operation::RUN);

      state.invoke(callback, origin);
    } else {
      State::Guard guard(state, State::Operation::CONFIGURE);

      state.invoke(callback, origin);
      callback = {};

      if (state.failure) {
        (void)state.cleanup(origin);
      }
    }
  } else if (state.pending.size() >= state.options.maxConfigurations) {
    state.fail(error(ManagedPhase::BIND, TreeError::NODE_LIMIT), "scene.configure", origin);
  } else {
    state.pending.push_back({std::move(callback), origin});
  }
}

void Scene::setStop(Stop callback, std::source_location origin) {
  detail::SetupFailureScope setupMask({}, nullptr, nullptr);
  detail::CurrentSceneScope currentMask;
  auto &state = *state_;
  State::Guard guard(state, State::Operation::CONFIGURE);

  if (state.stopped) {
    state.fail(error(ManagedPhase::CLEANUP, TreeError::CLOSED, state.node()), "scene.onStop", origin);

    return;
  }

  state.stopOrigin = origin;
  state.stopCallback = std::move(callback);
}

Scene &Scene::theme(StyleSheet sheet, std::source_location origin) & {
  applyTheme(sheet, origin);

  return *this;
}

Scene &Scene::styleLayers(StyleLayerOptions options, std::source_location origin) & {
  if (!admitConfiguration(true, origin)) {
    return *this;
  }

  try {
    addConfiguration(
      [options](ManagedScene &scene) {
        return scene.enableStyleLayers(options);
      },
      true, origin);
  } catch (const std::bad_alloc &) {
    configurationException(TreeError::NO_MEMORY, origin);
  } catch (...) {
    configurationException(TreeError::CALLBACK_FAILED, origin);
  }

  return *this;
}

SceneRef Scene::ref() const {
  if (owner_ != std::this_thread::get_id() || !state_) {
    return {std::weak_ptr<detail::SceneAnchor>{}, owner_};
  }

  return {state_->anchor, owner_};
}

std::expected<void, ManagedError> SceneRef::quit(std::source_location origin) const {
  if (owner_ != std::thread::id{} && owner_ != std::this_thread::get_id()) {
    return std::unexpected(error(ManagedPhase::RUN, TreeError::WRONG_THREAD));
  }

  const auto anchor = anchor_.lock();

  if (anchor && anchor->owner != std::this_thread::get_id()) {
    return std::unexpected(error(ManagedPhase::RUN, TreeError::WRONG_THREAD));
  }

  if (!anchor || !anchor->state) {
    return std::unexpected(error(ManagedPhase::RUN, TreeError::CLOSED));
  }

  return anchor->quit(anchor->state, origin);
}

void quit(std::source_location origin) {
  // Use the same weak snapshot for checked admission and any safe existing-channel report.
  QuitAction{detail::SceneAccess::currentRef(), origin}();
}

void SceneRef::report(const ManagedError &failure, std::source_location origin) const {
  if (owner_ != std::this_thread::get_id()) {
    return;
  }

  const auto anchor = anchor_.lock();

  if (anchor && anchor->owner == std::this_thread::get_id() && anchor->state) {
    detail::reportEventError(anchor->events, failure, origin);
  }
}

void Scene::setupFailure(ManagedError failure, std::source_location origin) {
  if (owner_ != std::this_thread::get_id() || !state_) {
    return;
  }

  auto &state = *state_;

  state.fail(std::move(failure), "scene.setup", origin);

  if (state.active == State::Operation::NONE && !state.completed) {
    State::Guard guard(state, State::Operation::CONFIGURE);

    (void)state.cleanup(origin);
  }
}

Scene &Scene::style(const StyleStage &stage, std::source_location origin) & {
  if (!admitConfiguration(true, origin)) {
    return *this;
  }

  auto &state = *state_;

  {
    State::Guard guard(state,
                       state.active == State::Operation::RUN ? State::Operation::RUN : State::Operation::CONFIGURE);

    try {
      auto *stable = &state;

      addConfiguration(
        [stable, stage](ManagedScene &scene) -> std::expected<void, ManagedError> {
          try {
            // Keep reusable stage handles without owning retired documents or expired registry rows.
            std::erase_if(stable->styles, [](const State::StyleBinding &binding) {
              return binding.identity.expired();
            });

            for (const auto &binding : stable->styles) {
              if (binding.identity.lock() == stage.payload_) {
                return stage.apply(scene, &binding.handle).transform([](const StyleLayerHandle &) {
                });
              }
            }

            // Reserve facade identity storage before the existing engine can commit any layer.
            stable->styles.reserve(stable->styles.size() + 1);

            if (stable->failure) {
              return std::unexpected(*stable->failure);
            }

            auto installed = stage.apply(scene, nullptr);

            if (!installed) {
              return std::unexpected(installed.error());
            }

            stable->styles.push_back({stage.payload_, *installed});

            return {};
          } catch (const std::bad_alloc &) {
            return std::unexpected(stage.boundaryFailure(TreeError::NO_MEMORY));
          } catch (...) {
            return std::unexpected(stage.boundaryFailure(TreeError::CALLBACK_FAILED));
          }
        },
        true, origin);
    } catch (const std::bad_alloc &) {
      state.fail(stage.boundaryFailure(TreeError::NO_MEMORY), "scene.style", origin);
    } catch (...) {
      state.fail(stage.boundaryFailure(TreeError::CALLBACK_FAILED), "scene.style", origin);
    }
  }

  if (state.failure && state.active == State::Operation::NONE && !state.completed) {
    State::Guard guard(state, State::Operation::CONFIGURE);

    (void)state.cleanup(origin);
  }

  return *this;
}

void Scene::applyLiveBinding(std::shared_ptr<detail::LiveFactory> factory, std::source_location origin) {
  if (!admitConfiguration(true, origin)) {
    return;
  }

  auto &state = *state_;

  {
    State::Guard guard(state,
                       state.active == State::Operation::RUN ? State::Operation::RUN : State::Operation::CONFIGURE);

    try {
      auto *stable = &state;

      addConfiguration(
        [stable, factory](ManagedScene &scene) -> std::expected<void, ManagedError> {
          try {
            auto created = factory->create(stable->anchor);

            if (!created) {
              return std::unexpected(created.error());
            }

            if (stable->failure) {
              return std::unexpected(*stable->failure);
            }

            auto binding = *created;
            const bool retained = std::ranges::find(stable->live, binding) != stable->live.end();

            // First attachment reserves scene retention before attachTheme. Reuse allocates no controller/slot.
            if (!retained) {
              stable->live.push_back(binding);
            }

            if (stable->failure) {
              if (!retained) {
                stable->live.pop_back();
              }

              return std::unexpected(*stable->failure);
            }

            auto attached = binding->attach(scene, stable->anchor);

            if ((!attached || !*attached) && !retained) {
              stable->live.pop_back();
            }

            return attached ? std::expected<void, ManagedError>{} : std::unexpected(attached.error());
          } catch (const std::bad_alloc &) {
            return std::unexpected(error(ManagedPhase::STYLE, TreeError::NO_MEMORY, scene.root().id()));
          } catch (...) {
            return std::unexpected(error(ManagedPhase::STYLE, TreeError::CALLBACK_FAILED, scene.root().id()));
          }
        },
        true, origin);
    } catch (const std::bad_alloc &) {
      state.fail(error(ManagedPhase::STYLE, TreeError::NO_MEMORY, state.node()), "scene.liveTheme", origin);
    } catch (...) {
      state.fail(error(ManagedPhase::STYLE, TreeError::CALLBACK_FAILED, state.node()), "scene.liveTheme", origin);
    }
  }

  if (state.failure && state.active == State::Operation::NONE && !state.completed) {
    State::Guard guard(state, State::Operation::CONFIGURE);

    (void)state.cleanup(origin);
  }
}

Scene &Scene::styleLayer(StyleTier tier, std::string name, StyleSheet sheet, std::source_location origin) & {
  applyStyleLayer(tier, name, sheet, origin);

  return *this;
}

void Scene::applyStyleLayer(StyleTier tier, const std::string &name, const StyleSheet &sheet,
                            std::source_location origin) {
  if (!admitConfiguration(true, origin)) {
    return;
  }

  try {
    // Checked-container transfers may allocate even in noexcept closure moves on MSVC.
    // Copy under this boundary, then retain only a non-throwing owner in the callback.
    const auto payload = std::make_shared<const std::pair<std::string, StyleSheet>>(name, sheet);

    addConfiguration(
      [tier, payload](ManagedScene &scene) {
        return scene.addLayer(tier, payload->first, payload->second).transform([](const StyleLayerHandle &) {
        });
      },
      true, origin);
  } catch (const std::bad_alloc &) {
    configurationException(TreeError::NO_MEMORY, origin);
  } catch (...) {
    configurationException(TreeError::CALLBACK_FAILED, origin);
  }
}

void Scene::applyTheme(const StyleSheet &sheet, std::source_location origin) {
  if (!admitConfiguration(true, origin)) {
    return;
  }

  auto &state = *state_;

  try {
    if (!state.prepared) {
      auto replacement = std::make_unique<StyleSheet>(sheet);

      state.theme.swap(replacement);
    } else {
      auto applied = state.managed->theme(sheet);

      if (!applied) {
        state.fail(applied.error(), "scene.theme", origin);
      }
    }
  } catch (const std::bad_alloc &) {
    state.fail(error(ManagedPhase::STYLE, TreeError::NO_MEMORY, state.node()), "scene.theme", origin);
  } catch (...) {
    state.fail(error(ManagedPhase::STYLE, TreeError::CALLBACK_FAILED, state.node()), "scene.theme", origin);
  }

  if (state.failure && state.active == State::Operation::NONE) {
    State::Guard guard(state, State::Operation::CONFIGURE);

    (void)state.cleanup(origin);
  }
}

Scene &Scene::onError(ManagedErrorHandler observer, std::source_location origin) & {
  detail::SetupFailureScope observerScope({}, nullptr, nullptr);
  ManagedErrorHandler incomingObserver;

  // Empty the parameter so rejected or failed transfers retire its target before the scope.
  incomingObserver.swap(observer);

  if (owner_ != std::this_thread::get_id() || !state_) {
    return *this;
  }

  auto &state = *state_;

  if (state.active != State::Operation::NONE || state.sessionActive) {
    state.fail(error(ManagedPhase::BIND, TreeError::WOULD_BLOCK, state.node()), "scene.onError", origin);

    return *this;
  }

  State::Guard guard(state, State::Operation::CONFIGURE);

  try {
    auto replacement = incomingObserver ? std::make_shared<ManagedErrorHandler>(std::move(incomingObserver)) : nullptr;

    state.observer.swap(replacement);

    if (state.managed && !state.managedClosed) {
      auto installed = state.installObserver();

      if (!installed) {
        state.fail(installed.error(), "scene.onError", origin);
      }
    }

    state.notifyFallback();
  } catch (const std::bad_alloc &) {
    state.fail(error(ManagedPhase::BIND, TreeError::NO_MEMORY, state.node()), "scene.onError", origin);
  } catch (...) {
    state.fail(error(ManagedPhase::BIND, TreeError::CALLBACK_FAILED, state.node()), "scene.onError", origin);
  }

  if (state.failure && !state.completed) {
    (void)state.cleanup(origin);
  }

  return *this;
}

Scene &Scene::reportOnError(SceneReportHandler sink, std::source_location origin) & {
  detail::SetupFailureScope reportingScope({}, nullptr, nullptr);
  SceneReportHandler incomingSink;

  // std::function::swap leaves the parameter empty without allocating.
  incomingSink.swap(sink);

  if (owner_ != std::this_thread::get_id() || !state_) {
    return *this;
  }

  auto &state = *state_;

  if (state.active != State::Operation::NONE || state.sessionActive) {
    state.fail(error(ManagedPhase::BIND, TreeError::WOULD_BLOCK, state.node()), "scene.reportOnError", origin);

    return *this;
  }

  State::Guard guard(state, State::Operation::CONFIGURE);

  try {
    auto replacement = incomingSink ? std::make_shared<SceneReportHandler>(std::move(incomingSink)) : nullptr;

    state.reportSink.swap(replacement);
  } catch (const std::bad_alloc &) {
    state.fail(error(ManagedPhase::BIND, TreeError::NO_MEMORY, state.node()), "scene.reportOnError", origin);
  } catch (...) {
    state.fail(error(ManagedPhase::BIND, TreeError::CALLBACK_FAILED, state.node()), "scene.reportOnError", origin);
  }

  return *this;
}

std::expected<ApplicationStats, ManagedError> Scene::run(std::stop_token stop, std::source_location origin) {
  return runImpl(nullptr, stop, origin);
}

std::expected<void, ManagedError> Scene::quit(std::source_location origin) {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(error(ManagedPhase::RUN, TreeError::WRONG_THREAD));
  }

  if (!state_) {
    return std::unexpected(error(ManagedPhase::RUN, TreeError::CLOSED));
  }

  auto &state = *state_;

  if (state.active == State::Operation::NOTIFY || state.active == State::Operation::REPORT ||
      state.active == State::Operation::STOP || state.active == State::Operation::CLOSE) {
    auto cause = error(ManagedPhase::RUN, TreeError::WOULD_BLOCK, state.node());

    state.fail(cause, "scene.quit", origin);

    return std::unexpected(cause);
  }

  if (state.completed) {
    return std::unexpected(error(ManagedPhase::RUN, TreeError::CLOSED, state.node()));
  }

  state.quitRequested = true;

  if (state.prepared && state.active != State::Operation::PREPARE) {
    state.application->requestStop();
  }

  return {};
}

Scene &Scene::applicationOptions(std::function<ApplicationOptions()> provider, std::source_location origin) & {
  if (!admitConfiguration(false, origin)) {
    return *this;
  }

  auto &state = *state_;
  State::Guard guard(state, State::Operation::CONFIGURE);

  if (state.prepared) {
    state.fail(error(ManagedPhase::PREPARE, TreeError::WOULD_BLOCK, state.node()), "scene.applicationOptions", origin);
  } else {
    state.applicationOptions.swap(provider);
    state.applicationOptionsOrigin = origin;
  }

  // Replaced callback captures retire before the admission guard is released.
  provider = {};

  return *this;
}

int Scene::runApp(int argc, const char *const *argv, CommandLineParser parser, std::stop_token stop,
                  std::source_location origin) {
  detail::FactoryCleanupScope factoryMask;
  const auto status = runAppPreflight(nullptr, argc, argv, parser, stop, origin);

  parser = nullptr;

  return status;
}

int Scene::runApp(ApplicationTerminal &terminal, int argc, const char *const *argv, CommandLineParser parser,
                  std::stop_token stop, std::source_location origin) {
  detail::FactoryCleanupScope factoryMask;
  const auto status = runAppPreflight(&terminal, argc, argv, parser, stop, origin);

  parser = nullptr;

  return status;
}

int Scene::runAppPreflight(ApplicationTerminal *terminal, int argc, const char *const *argv, CommandLineParser &parser,
                           std::stop_token stop, std::source_location origin) {
  detail::FactoryCleanupScope factoryMask;

  if (owner_ != std::this_thread::get_id() || !state_) {
    return 1;
  }

  auto &state = *state_;

  if (state.prepared && !state.completed && state.active == State::Operation::NONE) {
    state.fail(error(ManagedPhase::PREPARE, TreeError::WOULD_BLOCK, state.node()), "scene.preflight", origin);
  }

  if (state.active != State::Operation::NONE || state.failure || state.completed || state.finalReported) {
    return runAppImpl(terminal, stop, origin);
  }

  std::expected<LaunchAction, CommandLineError> action;

  {
    State::Guard guard(state, State::Operation::PREFLIGHT);
    detail::SetupFailureScope lifecycleScope({}, nullptr, nullptr);
    detail::CurrentSceneScope currentMask;

    action = detail::commandLinePreflight(argc, argv, parser);
  }

  // A failed CLI result remains primary even if capture retirement independently failed.
  if (state.failure && action) {
    return runAppImpl(terminal, stop, origin);
  }

  if (action && action->disposition == LaunchDisposition::CONTINUE) {
    return runAppImpl(terminal, stop, origin);
  }

  state.preflightStatus = action ? 0 : 2;

  {
    State::Guard guard(state, State::Operation::CLOSE);

    (void)state.cleanup(origin);
    state.notifyFallback();
  }

  state.finalReported = true;

  SceneReport report;

  report.origin = origin;
  report.failure = state.failure;

  if (!action) {
    report.commandLineFailure = std::move(action.error());
  } else if (!action->output.empty()) {
    std::fwrite(action->output.data(), 1, action->output.size(), stdout);
  }

  if (report.commandLineFailure || report.failure) {
    state.deliverReport(report);
  }

  if (state.failure && action) {
    state.preflightStatus = 1;
  }

  return *state.preflightStatus;
}

int Scene::runApp(std::stop_token stop, std::source_location origin) {
  return runAppImpl(nullptr, stop, origin);
}

int Scene::runApp(ApplicationTerminal &terminal, std::stop_token stop, std::source_location origin) {
  return runAppImpl(&terminal, stop, origin);
}

int Scene::runAppReported(const RunAppOptions &options, std::shared_ptr<SceneReportHandler> sink,
                          std::source_location origin, std::size_t droppedErrors) {
  if (owner_ == std::this_thread::get_id() && state_ && state_->active == State::Operation::NONE &&
      !state_->sessionActive && sink) {
    State::Guard guard(*state_, State::Operation::CONFIGURE);

    // The upper callable was pinned before factory entry. Transfer it without allocating; replaced
    // captures retire under admission, and the new sink stays owned through final Scene destruction.
    state_->reportSink.swap(sink);
    sink.reset();
  }

  return runAppImpl(options.terminal, options.stop, origin, droppedErrors);
}

int Scene::runAppImpl(ApplicationTerminal *terminal, std::stop_token stop, std::source_location origin,
                      std::size_t droppedErrors) {
  detail::FactoryCleanupScope factoryMask;

  // An unadmitted facade cannot prove that another owner's terminal has been restored.
  if (owner_ != std::this_thread::get_id() || !state_) {
    return 1;
  }

  if (state_->completed && state_->preflightStatus && state_->active == State::Operation::NONE) {
    return *state_->preflightStatus;
  }

  detail::SetupFailureScope lifecycleScope({}, nullptr, nullptr);
  const auto result = runImpl(terminal, stop, origin);
  auto &state = *state_;

  // A nested runApp has not restored the terminal; leave all reporting to the admitted outer run.
  if (state.active != State::Operation::NONE || state.finalReported) {
    return result ? 0 : 1;
  }

  if (!state.completed) {
    State::Guard guard(state, State::Operation::CLOSE);

    (void)state.cleanup(origin);
    state.notifyFallback();
  }

  state.finalReported = true;

  SceneReport report;

  report.origin = state.failure ? state.failureOrigin : origin;

  if (!result) {
    report.failure = state.failure ? *state.failure : result.error();
  } else if (state.managed) {
    const auto [retained, dropped] = detail::SceneAccess::reportCounts(*state.managed);

    report.retainedErrors = retained;
    report.droppedErrors = dropped;
  }

  report.droppedErrors = saturatingCount(report.droppedErrors, droppedErrors);

  if (report.failure || report.retainedErrors || report.droppedErrors) {
    // Keep the callable alive and forbid facade mutation/destruction throughout delivery and fallback.
    state.deliverReport(report);
  }

  return result ? 0 : 1;
}

std::expected<ApplicationStats, ManagedError> Scene::run(ApplicationTerminal &terminal, std::stop_token stop,
                                                         std::source_location origin) {
  return runImpl(&terminal, stop, origin);
}

std::expected<ApplicationStats, ManagedError> Scene::runImpl(ApplicationTerminal *terminal, std::stop_token stop,
                                                             std::source_location origin) {
  detail::FactoryCleanupScope factoryMask;

  if (owner_ == std::this_thread::get_id() && state_ && state_->sessionActive) {
    return std::unexpected(error(ManagedPhase::RUN, TreeError::WOULD_BLOCK, state_->node()));
  }

  detail::SetupFailureScope lifecycleScope({}, nullptr, nullptr);
  auto prepared = prepareChecked(origin);

  if (!prepared) {
    return std::unexpected(prepared.error());
  }

  auto &state = *state_;
  State::Guard guard(state, State::Operation::RUN);

  auto result = [&] {
    detail::CurrentSceneScope current(state.anchor);

    return terminal ? state.managed->run(*terminal, stop) : state.managed->run(stop);
  }();

  return state.finishRun(std::move(result), origin);
}

std::expected<void, ManagedError> Scene::start(ApplicationReadyTerminal &terminal, std::stop_token stop,
                                               std::source_location origin) {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(error(ManagedPhase::RUN, TreeError::WRONG_THREAD));
  }

  if (!state_) {
    return std::unexpected(error(ManagedPhase::RUN, TreeError::CLOSED));
  }

  if (state_->active != State::Operation::NONE || state_->sessionActive) {
    return std::unexpected(error(ManagedPhase::RUN, TreeError::WOULD_BLOCK, state_->node()));
  }

  detail::SetupFailureScope lifecycleScope({}, nullptr, nullptr);
  auto prepared = prepareChecked(origin);

  if (!prepared) {
    return prepared;
  }

  auto &state = *state_;
  State::Guard guard(state, State::Operation::RUN);
  detail::CurrentSceneScope current(state.anchor);

  state.sessionStarted = state.sessionActive = true;

  auto result = state.managed->start(terminal, stop);

  if (!result || state.stopped) {
    auto finished = state.finishStep(result ? state.managed->step() : std::unexpected(result.error()), origin);

    if (!finished) {
      return std::unexpected(finished.error());
    }
  }

  return {};
}

std::expected<ApplicationStep, ManagedError> Scene::step(std::source_location origin) {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(error(ManagedPhase::RUN, TreeError::WRONG_THREAD));
  }

  if (!state_) {
    return std::unexpected(error(ManagedPhase::RUN, TreeError::CLOSED));
  }

  auto &state = *state_;

  if (state.active != State::Operation::NONE) {
    return std::unexpected(error(ManagedPhase::RUN, TreeError::WOULD_BLOCK, state.node()));
  }

  if (state.sessionResult) {
    return *state.sessionResult;
  }

  if (!state.sessionStarted) {
    return std::unexpected(
      error(ManagedPhase::RUN, state.completed ? TreeError::CLOSED : TreeError::INVALID_ARGUMENT, state.node()));
  }

  detail::SetupFailureScope lifecycleScope({}, nullptr, nullptr);
  State::Guard guard(state, State::Operation::RUN);
  detail::CurrentSceneScope current(state.anchor);

  return state.finishStep(state.managed->step(), origin);
}

std::expected<void, ManagedError> Scene::close(std::source_location origin) {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(error(ManagedPhase::CLEANUP, TreeError::WRONG_THREAD));
  }

  if (!state_) {
    return std::unexpected(error(ManagedPhase::CLEANUP, TreeError::CLOSED));
  }

  detail::SetupFailureScope lifecycleScope({}, nullptr, nullptr);
  auto &state = *state_;

  if (state.sessionActive) {
    return std::unexpected(error(ManagedPhase::CLEANUP, TreeError::WOULD_BLOCK, state.node()));
  }

  if (state.active != State::Operation::NONE) {
    auto cause = error(ManagedPhase::CLEANUP, TreeError::WOULD_BLOCK, state.node());

    state.fail(cause, "scene.close", origin);

    return std::unexpected(cause);
  }

  State::Guard guard(state, State::Operation::CLOSE);

  return state.cleanup(origin);
}
} // namespace portals::ui
