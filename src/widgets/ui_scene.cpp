// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_style.hpp>

#include <array>
#include <cstdio>

namespace portals::ui {
namespace {
constexpr const char *phaseName(ManagedPhase phase) noexcept {
  switch (phase) {
  case ManagedPhase::MOUNT:
    return "MOUNT";
  case ManagedPhase::STATE:
    return "STATE";
  case ManagedPhase::BIND:
    return "BIND";
  case ManagedPhase::STYLE:
    return "STYLE";
  case ManagedPhase::PREPARE:
    return "PREPARE";
  case ManagedPhase::INPUT:
    return "INPUT";
  case ManagedPhase::RUN:
    return "RUN";
  case ManagedPhase::CLEANUP:
    return "CLEANUP";
  case ManagedPhase::TEXT:
    return "TEXT";
  }

  return "UNKNOWN";
}

constexpr const char *treeErrorName(TreeError cause) noexcept {
  switch (cause) {
  case TreeError::INVALID_ARGUMENT:
    return "INVALID_ARGUMENT";
  case TreeError::WRONG_THREAD:
    return "WRONG_THREAD";
  case TreeError::WRONG_SESSION:
    return "WRONG_SESSION";
  case TreeError::STALE_NODE:
    return "STALE_NODE";
  case TreeError::CLOSED:
    return "CLOSED";
  case TreeError::WOULD_BLOCK:
    return "WOULD_BLOCK";
  case TreeError::NO_MEMORY:
    return "NO_MEMORY";
  case TreeError::IDENTITY_EXHAUSTED:
    return "IDENTITY_EXHAUSTED";
  case TreeError::NODE_LIMIT:
    return "NODE_LIMIT";
  case TreeError::DEPTH_LIMIT:
    return "DEPTH_LIMIT";
  case TreeError::CALLBACK_FAILED:
    return "CALLBACK_FAILED";
  case TreeError::INVALID_GEOMETRY:
    return "INVALID_GEOMETRY";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(ApplicationPhase value) noexcept {
  switch (value) {
  case ApplicationPhase::CREATE:
    return "CREATE";
  case ApplicationPhase::OPEN:
    return "OPEN";
  case ApplicationPhase::INPUT:
    return "INPUT";
  case ApplicationPhase::DISPATCH:
    return "DISPATCH";
  case ApplicationPhase::PREPARE:
    return "PREPARE";
  case ApplicationPhase::RENDER:
    return "RENDER";
  case ApplicationPhase::SUBMIT:
    return "SUBMIT";
  case ApplicationPhase::FLUSH:
    return "FLUSH";
  case ApplicationPhase::STOP:
    return "STOP";
  case ApplicationPhase::CLOSE:
    return "CLOSE";
  case ApplicationPhase::CONTROL_LATENCY:
    return "CONTROL_LATENCY";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(ApplicationFailure value) noexcept {
  switch (value) {
  case ApplicationFailure::INVALID_OPTIONS:
    return "INVALID_OPTIONS";
  case ApplicationFailure::ALREADY_RUN:
    return "ALREADY_RUN";
  case ApplicationFailure::NO_MEMORY:
    return "NO_MEMORY";
  case ApplicationFailure::CALLBACK_FAILED:
    return "CALLBACK_FAILED";
  case ApplicationFailure::OBSERVER_FAILED:
    return "OBSERVER_FAILED";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(TerminalErrorKind value) noexcept {
  switch (value) {
  case TerminalErrorKind::INVALID_OPTIONS:
    return "INVALID_OPTIONS";
  case TerminalErrorKind::UNSUPPORTED:
    return "UNSUPPORTED";
  case TerminalErrorKind::SYSTEM:
    return "SYSTEM";
  case TerminalErrorKind::CLOSED:
    return "CLOSED";
  case TerminalErrorKind::OUTPUT_PENDING:
    return "OUTPUT_PENDING";
  case TerminalErrorKind::OUTPUT_LIMIT:
    return "OUTPUT_LIMIT";
  case TerminalErrorKind::INPUT_DECODE:
    return "INPUT_DECODE";
  case TerminalErrorKind::OUTPUT_UNCERTAIN:
    return "OUTPUT_UNCERTAIN";
  case TerminalErrorKind::INPUT_LIMIT:
    return "INPUT_LIMIT";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(SchedulerError value) noexcept {
  switch (value) {
  case SchedulerError::INVALID_ARGUMENT:
    return "INVALID_ARGUMENT";
  case SchedulerError::WRONG_THREAD:
    return "WRONG_THREAD";
  case SchedulerError::WRONG_SCHEDULER:
    return "WRONG_SCHEDULER";
  case SchedulerError::CLOSED:
    return "CLOSED";
  case SchedulerError::WOULD_BLOCK:
    return "WOULD_BLOCK";
  case SchedulerError::NO_MEMORY:
    return "NO_MEMORY";
  case SchedulerError::REENTRANT_DISPATCH:
    return "REENTRANT_DISPATCH";
  case SchedulerError::IDENTITY_EXHAUSTED:
    return "IDENTITY_EXHAUSTED";
  case SchedulerError::CALLBACK_FAILED:
    return "CALLBACK_FAILED";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(InputError value) noexcept {
  switch (value) {
  case InputError::INVALID_OPTIONS:
    return "INVALID_OPTIONS";
  case InputError::BACKWARD_TIME:
    return "BACKWARD_TIME";
  case InputError::CLOSED:
    return "CLOSED";
  case InputError::FAILED:
    return "FAILED";
  case InputError::OFFSET_OVERFLOW:
    return "OFFSET_OVERFLOW";
  case InputError::INVALID_WINDOW:
    return "INVALID_WINDOW";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(RenderErrorKind value) noexcept {
  switch (value) {
  case RenderErrorKind::COMMAND_LIMIT:
    return "COMMAND_LIMIT";
  case RenderErrorKind::TEXT_BYTE_LIMIT:
    return "TEXT_BYTE_LIMIT";
  case RenderErrorKind::WORK_LIMIT:
    return "WORK_LIMIT";
  case RenderErrorKind::POLICY_MISMATCH:
    return "POLICY_MISMATCH";
  case RenderErrorKind::BUFFER_ERROR:
    return "BUFFER_ERROR";
  case RenderErrorKind::ALLOCATION_FAILURE:
    return "ALLOCATION_FAILURE";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(FrameError value) noexcept {
  switch (value) {
  case FrameError::PENDING:
    return "PENDING";
  case FrameError::POISONED:
    return "POISONED";
  case FrameError::FRAME_LIMIT:
    return "FRAME_LIMIT";
  case FrameError::OUTPUT_LIMIT:
    return "OUTPUT_LIMIT";
  case FrameError::ALLOCATION_FAILURE:
    return "ALLOCATION_FAILURE";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(CellBufferError value) noexcept {
  switch (value) {
  case CellBufferError::INVALID_LIMITS:
    return "INVALID_LIMITS";
  case CellBufferError::INVALID_POLICY:
    return "INVALID_POLICY";
  case CellBufferError::COLUMN_LIMIT:
    return "COLUMN_LIMIT";
  case CellBufferError::ROW_LIMIT:
    return "ROW_LIMIT";
  case CellBufferError::CELL_LIMIT:
    return "CELL_LIMIT";
  case CellBufferError::OUT_OF_BOUNDS:
    return "OUT_OF_BOUNDS";
  case CellBufferError::INVALID_GRAPHEME:
    return "INVALID_GRAPHEME";
  case CellBufferError::POLICY_MISMATCH:
    return "POLICY_MISMATCH";
  case CellBufferError::WIDE_AT_EDGE:
    return "WIDE_AT_EDGE";
  case CellBufferError::GLYPH_LIMIT:
    return "GLYPH_LIMIT";
  case CellBufferError::GLYPH_BYTE_LIMIT:
    return "GLYPH_BYTE_LIMIT";
  case CellBufferError::STYLE_LIMIT:
    return "STYLE_LIMIT";
  case CellBufferError::ALLOCATION_FAILURE:
    return "ALLOCATION_FAILURE";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(TextErrorKind value) noexcept {
  switch (value) {
  case TextErrorKind::INVALID_UTF8:
    return "INVALID_UTF8";
  case TextErrorKind::INPUT_LIMIT:
    return "INPUT_LIMIT";
  case TextErrorKind::CLUSTER_LIMIT:
    return "CLUSTER_LIMIT";
  case TextErrorKind::GRAPHEME_LIMIT:
    return "GRAPHEME_LIMIT";
  case TextErrorKind::CELL_LIMIT:
    return "CELL_LIMIT";
  case TextErrorKind::INVALID_POLICY:
    return "INVALID_POLICY";
  case TextErrorKind::NOT_BOUNDARY:
    return "NOT_BOUNDARY";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(ShortcutErrorKind value) noexcept {
  switch (value) {
  case ShortcutErrorKind::INVALID_OPTIONS:
    return "INVALID_OPTIONS";
  case ShortcutErrorKind::SOURCE_LIMIT:
    return "SOURCE_LIMIT";
  case ShortcutErrorKind::ALTERNATIVE_LIMIT:
    return "ALTERNATIVE_LIMIT";
  case ShortcutErrorKind::STROKE_LIMIT:
    return "STROKE_LIMIT";
  case ShortcutErrorKind::EMPTY_ALTERNATIVE:
    return "EMPTY_ALTERNATIVE";
  case ShortcutErrorKind::EMPTY_TOKEN:
    return "EMPTY_TOKEN";
  case ShortcutErrorKind::UNKNOWN_KEY:
    return "UNKNOWN_KEY";
  case ShortcutErrorKind::DUPLICATE_MODIFIER:
    return "DUPLICATE_MODIFIER";
  case ShortcutErrorKind::EXPECTED_KEY:
    return "EXPECTED_KEY";
  case ShortcutErrorKind::UNSUPPORTED_SEQUENCE:
    return "UNSUPPORTED_SEQUENCE";
  case ShortcutErrorKind::INVALID_CHORD:
    return "INVALID_CHORD";
  case ShortcutErrorKind::NO_MEMORY:
    return "NO_MEMORY";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(TextResourceError value) noexcept {
  switch (value) {
  case TextResourceError::MISSING_RESOURCE:
    return "MISSING_RESOURCE";
  case TextResourceError::RESOLVER_REQUIRED:
    return "RESOLVER_REQUIRED";
  case TextResourceError::RESOLVER_FAILED:
    return "RESOLVER_FAILED";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(PssErrorCode value) noexcept {
  switch (value) {
  case PssErrorCode::NONE:
    return "NONE";
  case PssErrorCode::INPUT_LIMIT:
    return "INPUT_LIMIT";
  case PssErrorCode::RULE_LIMIT:
    return "RULE_LIMIT";
  case PssErrorCode::CLASS_LIMIT:
    return "CLASS_LIMIT";
  case PssErrorCode::DECLARATION_LIMIT:
    return "DECLARATION_LIMIT";
  case PssErrorCode::TEXT_LIMIT:
    return "TEXT_LIMIT";
  case PssErrorCode::DOCUMENT_LIMIT:
    return "DOCUMENT_LIMIT";
  case PssErrorCode::SOURCE_NAME_LIMIT:
    return "SOURCE_NAME_LIMIT";
  case PssErrorCode::INVALID_UTF8:
    return "INVALID_UTF8";
  case PssErrorCode::NUL_BYTE:
    return "NUL_BYTE";
  case PssErrorCode::EXPECTED_SELECTOR:
    return "EXPECTED_SELECTOR";
  case PssErrorCode::EXPECTED_IDENTIFIER:
    return "EXPECTED_IDENTIFIER";
  case PssErrorCode::EXPECTED_PUNCTUATION:
    return "EXPECTED_PUNCTUATION";
  case PssErrorCode::UNTERMINATED_COMMENT:
    return "UNTERMINATED_COMMENT";
  case PssErrorCode::DUPLICATE_ID:
    return "DUPLICATE_ID";
  case PssErrorCode::DUPLICATE_STATE:
    return "DUPLICATE_STATE";
  case PssErrorCode::UNKNOWN_STATE:
    return "UNKNOWN_STATE";
  case PssErrorCode::UNKNOWN_PROPERTY:
    return "UNKNOWN_PROPERTY";
  case PssErrorCode::INVALID_VALUE:
    return "INVALID_VALUE";
  case PssErrorCode::INVALID_WIDTH:
    return "INVALID_WIDTH";
  case PssErrorCode::ALLOCATION:
    return "ALLOCATION";
  }

  return "UNKNOWN";
}

constexpr const char *categoryName(PssFileOperation value) noexcept {
  switch (value) {
  case PssFileOperation::OPEN:
    return "OPEN";
  case PssFileOperation::READ:
    return "READ";
  case PssFileOperation::INPUT_LIMIT:
    return "INPUT_LIMIT";
  }

  return "UNKNOWN";
}

ManagedError error(ManagedPhase phase, TreeError cause, NodeId node = {}) {
  return {phase, node, cause, {}, {}};
}

TreeError cleanupCause(const ManagedError &failure) {
  if (const auto *cause = std::get_if<TreeError>(&failure.cause)) {
    return *cause;
  }

  return TreeError::CALLBACK_FAILED;
}

void reportTerminal(const TerminalError &cause) noexcept {
  std::fprintf(stderr, "TerminalError(kind=%s, native=%d, restoration=%d)", categoryName(cause.kind), cause.native_code,
               cause.restoration_code);
}

void reportApplication(const ApplicationError &cause) noexcept {
  std::fprintf(stderr, "ApplicationError(phase=%s, cause=", categoryName(cause.phase));
  std::visit(
    [](const auto &value) {
      if constexpr (std::same_as<std::decay_t<decltype(value)>, TerminalError>) {
        reportTerminal(value);
      } else if constexpr (std::same_as<std::decay_t<decltype(value)>, RenderError>) {
        std::fprintf(stderr, "RenderError(kind=%s, buffer=%s)", categoryName(value.kind),
                     categoryName(value.buffer_error));
      } else if constexpr (std::same_as<std::decay_t<decltype(value)>, TreeError>) {
        std::fprintf(stderr, "TreeError(%s)", treeErrorName(value));
      } else {
        std::fprintf(stderr, "%s", categoryName(value));
      }
    },
    cause.cause);
  std::fputc(')', stderr);

  if (cause.restoration) {
    std::fputs(" restoration=", stderr);
    reportTerminal(*cause.restoration);
  }
}

void reportFailure(const ManagedError &failure, std::source_location origin) noexcept {
  std::fprintf(stderr, "Portals: scene failed (phase=%s) at %s:%u: ", phaseName(failure.phase), origin.file_name(),
               origin.line());
  std::visit(
    [](const auto &cause) {
      using Cause = std::decay_t<decltype(cause)>;

      if constexpr (std::same_as<Cause, TreeError>) {
        std::fprintf(stderr, "TreeError(%s)", treeErrorName(cause));
      } else if constexpr (std::same_as<Cause, StyleError>) {
        std::fprintf(stderr, "StyleError(cause=%s, line=%zu, column=%zu)", treeErrorName(cause.cause), cause.line,
                     cause.column);
      } else {
        reportApplication(cause);
      }
    },
    failure.cause);

  if (failure.styleSource) {
    std::fputs(" source=", stderr);
    std::fwrite(failure.styleSource->data(), 1, failure.styleSource->size(), stderr);
  }

  if (failure.pss) {
    const auto &pss = *failure.pss;

    std::fprintf(stderr, " pss=(code=%s, offset=%zu, line=%zu, column=%zu, subject=", categoryName(pss.code),
                 pss.location.offset, pss.location.line, pss.location.column);
    std::fwrite(pss.subject.data(), 1, std::min(pss.subjectSize, pss.subject.size()), stderr);
    std::fputs(", message=", stderr);
    std::fwrite(pss.message.data(), 1, pss.message.size(), stderr);
    std::fputc(')', stderr);
  }

  if (failure.file) {
    const auto &file = *failure.file;

    std::fprintf(stderr, " file=(operation=%s, category=%s, value=%d)", categoryName(file.operation),
                 file.cause.category().name(), file.cause.value());
  }

  if (failure.application) {
    std::fputs(" runner=", stderr);
    reportApplication(*failure.application);
  }

  if (failure.cleanup) {
    std::fprintf(stderr, " cleanup=TreeError(%s)", treeErrorName(*failure.cleanup));
  }

  if (failure.text) {
    std::fprintf(stderr, " text=(kind=%s, offset=%zu)", categoryName(failure.text->kind), failure.text->byte_offset);
  }

  if (failure.resource) {
    std::fprintf(stderr, " resource=%s", categoryName(*failure.resource));
  }

  if (failure.shortcut) {
    std::fprintf(stderr, " shortcut=(cause=%s, offset=%zu)", categoryName(failure.shortcut->cause),
                 failure.shortcut->offset);
  }

  std::fputc('\n', stderr);
}
} // namespace

void reportToStderr(const SceneReport &report) noexcept {
  if (report.failure) {
    reportFailure(*report.failure, report.origin);
  } else {
    std::fprintf(stderr, "Portals: recoverable diagnostics: retained=%zu, dropped=%zu; inspect diagnostics()\n",
                 report.retainedErrors, report.droppedErrors);
  }
}

struct Scene::State {
  enum class Operation {
    NONE,
    PREPARE,
    CONFIGURE,
    RUN,
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
  std::source_location recipeOrigin;
  std::unique_ptr<Application> application;
  std::unique_ptr<ManagedScene> managed;
  std::vector<Pending> pending = std::vector<Pending>(0);
  std::unique_ptr<StyleSheet> theme;
  std::shared_ptr<ManagedErrorHandler> observer;
  std::shared_ptr<SceneReportHandler> reportSink;
  std::optional<ManagedError> failure;
  std::source_location failureOrigin;
  ManagedDiagnostic fallback;
  Operation active{Operation::NONE};
  bool prepared{}, completed{}, managedClosed{}, notified{}, fallbackObserverFailed{};
  bool publishingFailure{};
  bool quitRequested{}, finalReported{};

  struct StyleBinding {
    std::weak_ptr<const void> identity;
    StyleLayerHandle handle;
  };

  std::shared_ptr<detail::SceneAnchor> anchor = std::make_shared<detail::SceneAnchor>();
  std::vector<StyleBinding> styles = std::vector<StyleBinding>(0);
  std::vector<std::shared_ptr<detail::LiveBinding>> live = std::vector<std::shared_ptr<detail::LiveBinding>>(0);

  std::expected<void, ManagedError> requestQuit() {
    if (active == Operation::NOTIFY || active == Operation::REPORT || active == Operation::CLOSE) {
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
    Guard guard(*this, Operation::NOTIFY);
    auto callback = observer;

    // Latch an exact managed primary before user observer reentry can introduce a boundary error.
    if (!failure && guard.previous == Operation::RUN && detail::SceneAccess::notifyingRunFailure(*managed)) {
      fail(diagnostic.error, "scene.run", diagnostic.origin, false);
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
    if (!failure || notified || !observer || active == Operation::NOTIFY) {
      return;
    }

    Guard guard(*this, Operation::NOTIFY);
    auto callback = observer;
    fallback.error = *failure;
    fallback.origin = failureOrigin;
    notified = true;

    try {
      (*callback)(fallback);
    } catch (...) {
      fallbackObserverFailed = true;
    }
  }

  void fail(ManagedError cause, std::string_view operation, std::source_location origin, bool report = true) {
    // Reporting is read-only: rejected final-sink reentry cannot alter a completed outcome or notify reactions.
    if (failure || active == Operation::REPORT) {
      return;
    }

    failure = std::move(cause);
    failureOrigin = origin;

    if (!report) {
      return;
    }

    if (managed && !managedClosed) {
      try {
        std::array<ManagedDiagnosticContext, 1> context;

        context[0].operation = operation;
        context[0].origin = origin;
        publishingFailure = true;
        (void)managed->reportError(*failure, context, origin);
        publishingFailure = false;
      } catch (const std::bad_alloc &) {
        publishingFailure = true;
        (void)managed->reportError(*failure, {}, origin);
        publishingFailure = false;
      }
    } else {
      notifyFallback();
    }
  }

  std::expected<void, ManagedError> cleanup(std::source_location origin) {
    completed = true;

    // Revoke observing publication and actual live controllers before managed input captures retire.
    if (anchor) {
      anchor->state = nullptr;
      anchor->events->lifetime.reset();
    }

    anchor.reset();
    live.clear();
    styles.clear();

    auto result = managed ? managed->close() : std::expected<void, ManagedError>{};

    if (!result) {
      const auto cause = cleanupCause(result.error());

      if (failure) {
        failure->cleanup = cause;
      } else {
        fail(result.error(), "scene.close", origin);
      }
    } else {
      managedClosed = true;
    }

    // Publish completion before captures retire: destructors can reenter the facade.
    recipe = {};
    pending.clear();
    theme.reset();

    return result;
  }

  void invoke(Configuration &callback, std::source_location origin) {
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

Scene::Scene(Configuration recipe, SceneOptions options, std::source_location origin)
    : owner_(std::this_thread::get_id()), state_(std::make_unique<State>()) {
  state_->options = options;
  state_->recipe = std::move(recipe);
  state_->recipeOrigin = origin;
  state_->anchor->state = state_.get();
  state_->anchor->quit = [](void *raw, std::source_location) {
    return static_cast<State *>(raw)->requestQuit();
  };
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

  State::Guard guard(*state_, State::Operation::CLOSE);

  (void)state_->cleanup(std::source_location::current());
  state_->observer.reset();
  state_->reportSink.reset();

  // Detach each public-facing owner before its user captures retire, while all State fields live.
  auto managed = std::move(state_->managed);

  managed.reset();

  auto application = std::move(state_->application);

  application.reset();
  state_->options.application.scheduler.diagnostics.reset();
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

  auto &state = *state_;

  if (state.active != State::Operation::NONE) {
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
            auto mounted = state.recipe(*state.managed);

            if (!mounted) {
              state.fail(mounted.error(), "scene.mount", state.recipeOrigin);
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

  if (state.active != State::Operation::NONE && !(runtime && state.active == State::Operation::RUN)) {
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
  auto &state = *state_;

  if (state.prepared) {
    if (runtime && state.active == State::Operation::RUN) {
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

  if (!anchor || !anchor->state) {
    return std::unexpected(error(ManagedPhase::RUN, TreeError::CLOSED));
  }

  return anchor->quit(anchor->state, origin);
}

void SceneRef::report(const ManagedError &failure, std::source_location origin) const {
  if (owner_ != std::this_thread::get_id()) {
    return;
  }

  const auto anchor = anchor_.lock();

  if (anchor && anchor->state) {
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
  if (owner_ != std::this_thread::get_id() || !state_) {
    return *this;
  }

  auto &state = *state_;

  if (state.active != State::Operation::NONE) {
    state.fail(error(ManagedPhase::BIND, TreeError::WOULD_BLOCK, state.node()), "scene.onError", origin);

    return *this;
  }

  State::Guard guard(state, State::Operation::CONFIGURE);

  try {
    auto replacement = observer ? std::make_shared<ManagedErrorHandler>(std::move(observer)) : nullptr;

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
  if (owner_ != std::this_thread::get_id() || !state_) {
    return *this;
  }

  auto &state = *state_;

  if (state.active != State::Operation::NONE) {
    state.fail(error(ManagedPhase::BIND, TreeError::WOULD_BLOCK, state.node()), "scene.reportOnError", origin);

    return *this;
  }

  State::Guard guard(state, State::Operation::CONFIGURE);

  try {
    auto replacement = sink ? std::make_shared<SceneReportHandler>(std::move(sink)) : nullptr;

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
      state.active == State::Operation::CLOSE) {
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

int Scene::runApp(std::stop_token stop, std::source_location origin) {
  return runAppImpl(nullptr, stop, origin);
}

int Scene::runApp(ApplicationTerminal &terminal, std::stop_token stop, std::source_location origin) {
  return runAppImpl(&terminal, stop, origin);
}

int Scene::runAppImpl(ApplicationTerminal *terminal, std::stop_token stop, std::source_location origin) {
  // An unadmitted facade cannot prove that another owner's terminal has been restored.
  if (owner_ != std::this_thread::get_id() || !state_) {
    return 1;
  }

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

  if (report.failure || report.retainedErrors || report.droppedErrors) {
    // Keep the callable alive and forbid facade mutation/destruction throughout delivery and fallback.
    State::Guard guard(state, State::Operation::REPORT);
    auto sink = state.reportSink;

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

  return result ? 0 : 1;
}

std::expected<ApplicationStats, ManagedError> Scene::run(ApplicationTerminal &terminal, std::stop_token stop,
                                                         std::source_location origin) {
  return runImpl(&terminal, stop, origin);
}

std::expected<ApplicationStats, ManagedError> Scene::runImpl(ApplicationTerminal *terminal, std::stop_token stop,
                                                             std::source_location origin) {
  auto prepared = prepareChecked(origin);

  if (!prepared) {
    return std::unexpected(prepared.error());
  }

  auto &state = *state_;
  State::Guard guard(state, State::Operation::RUN);

  auto result = terminal ? state.managed->run(*terminal, stop) : state.managed->run(stop);

  if (!result) {
    if (state.failure) {
      if (result.error().application) {
        state.failure->application = result.error().application;
      }

      if (result.error().cleanup) {
        state.failure->cleanup = result.error().cleanup;
      }
    } else {
      state.fail(result.error(), "scene.run", origin, false);
      state.notified = detail::SceneAccess::runFailureNotified(*state.managed);
    }
  }

  // Native open may fail before Application calls scene.stop; explicit close is mandatory.
  (void)state.cleanup(origin);
  state.notifyFallback();

  return state.failure ? std::unexpected(*state.failure) : result;
}

std::expected<void, ManagedError> Scene::close(std::source_location origin) {
  if (owner_ != std::this_thread::get_id()) {
    return std::unexpected(error(ManagedPhase::CLEANUP, TreeError::WRONG_THREAD));
  }

  if (!state_) {
    return std::unexpected(error(ManagedPhase::CLEANUP, TreeError::CLOSED));
  }

  auto &state = *state_;

  if (state.active != State::Operation::NONE) {
    auto cause = error(ManagedPhase::CLEANUP, TreeError::WOULD_BLOCK, state.node());

    state.fail(cause, "scene.close", origin);

    return std::unexpected(cause);
  }

  State::Guard guard(state, State::Operation::CLOSE);

  return state.cleanup(origin);
}
} // namespace portals::ui
