// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_scene.hpp>

#include <array>

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
} // namespace

struct Scene::State {
  enum class Operation {
    NONE,
    PREPARE,
    CONFIGURE,
    RUN,
    CLOSE,
    NOTIFY
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
  std::optional<ManagedError> failure;
  std::source_location failureOrigin;
  ManagedDiagnostic fallback;
  Operation active{Operation::NONE};
  bool prepared{}, completed{}, managedClosed{}, notified{}, fallbackObserverFailed{};
  bool publishingFailure{};

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
    if (failure) {
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

std::expected<ApplicationStats, ManagedError> Scene::run(std::stop_token stop, std::source_location origin) {
  return runImpl(nullptr, stop, origin);
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
