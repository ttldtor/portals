// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "async_scenario.hpp"

namespace async_example {
/** Equivalent explicit callback state machine: one scoped timer/data registration at a time. */
struct CallbackState : std::enable_shared_from_this<CallbackState> {
  Harness &h;
  Subscription wait;
  Subscription sentinel;
  UiTaskOutcome outcome;
  bool alive{true};

  explicit CallbackState(Harness &value) : h(value) {
  }

  struct ScopeNotice {
    std::weak_ptr<CallbackState> state;
    bool armed{};

    ~ScopeNotice() {
      if (armed) {
        if (auto owner = state.lock()) {
          owner->stop(true);
        }
      }
    }
  };

  std::weak_ptr<ScopeNotice> notice;

  void initialize() {
    auto lifetime = std::make_shared<ScopeNotice>();
    lifetime->state = weak_from_this();
    take(h.tree->withScope(h.button, [&](const UiScope &scope) {
      auto connection = take(h.scheduler->connect<int>(scope, [lifetime](auto) {
      }));
      sentinel = std::move(connection.subscription);
    }));
    notice = lifetime;
    lifetime->armed = true;
  }

  void stop(bool close) {
    if (close) {
      alive = false;
    }

    if (outcome.status == UiTaskStatus::RUNNING) {
      outcome.status = UiTaskStatus::CANCELLED;
      outcome.error = close ? AsyncError::CLOSED : AsyncError::CANCELLED;
    }

    take(wait.disconnect());

    if (close) {
      if (auto token = notice.lock()) {
        token->armed = false;
      }

      take(sentinel.disconnect());
    }
  }

  void failed(AsyncError error) {
    outcome.status = UiTaskStatus::FAILED;
    outcome.error = error;
    h.view->apply(std::unexpected(error));
    take(wait.disconnect());
  }

  std::expected<void, AsyncError> start(UiTaskRestart restart) {
    if (!alive) {
      return std::unexpected(AsyncError::CLOSED);
    }

    if (outcome.status == UiTaskStatus::RUNNING) {
      if (restart == UiTaskRestart::REJECT_RUNNING) {
        return std::unexpected(AsyncError::WOULD_BLOCK);
      }

      stop(false);
    }

    ++outcome.generation;
    outcome.status = UiTaskStatus::RUNNING;
    outcome.error.reset();
    auto generation = outcome.generation;
    std::expected<Subscription, SchedulerError> timer = std::unexpected(SchedulerError::CLOSED);
    auto scoped = h.tree->withScope(h.button, [&](const UiScope &scope) {
      timer = h.scheduler->at(scope, h.now + 10, [weak = weak_from_this(), generation] {
        if (auto state = weak.lock(); state && state->alive && state->outcome.generation == generation) {
          state->receive(generation);
        }
      });
    });

    if (!scoped || !timer) {
      failed(!scoped ? mapError(scoped.error()) : mapError(timer.error()));

      return {};
    }

    wait = std::move(*timer);

    return {};
  }

  void receive(std::uint64_t generation) {
    take(wait.disconnect());
    std::expected<SchedulerConnection<Result>, SchedulerError> connection = std::unexpected(SchedulerError::CLOSED);
    auto scoped = h.tree->withScope(h.button, [&](const UiScope &scope) {
      connection = h.scheduler->connect<Result>(
        scope, [weak = weak_from_this(), generation](std::shared_ptr<const Result> result) {
          auto state = weak.lock();

          if (!state || !state->alive || state->outcome.generation != generation ||
              state->outcome.status != UiTaskStatus::RUNNING) {
            return;
          }

          take(state->wait.disconnect());
          state->outcome.status = *result ? UiTaskStatus::SUCCEEDED : UiTaskStatus::FAILED;
          state->outcome.error = *result ? std::optional<AsyncError>{} : result->error();
          state->h.view->apply(*result);
        });
    });

    if (!scoped || !connection) {
      failed(!scoped ? mapError(scoped.error()) : mapError(connection.error()));

      return;
    }

    wait = std::move(connection->subscription);

    try {
      h.provider->start([publisher = connection->publisher](Result result) -> std::expected<PublishResult, AsyncError> {
        try {
          auto sent = publisher.publish(std::make_shared<const Result>(std::move(result)));

          if (!sent) {
            return std::unexpected(mapError(sent.error()));
          }

          return *sent;
        } catch (const std::bad_alloc &) {
          return std::unexpected(AsyncError::NO_MEMORY);
        }
      });
    } catch (const std::bad_alloc &) {
      failed(AsyncError::NO_MEMORY);
    } catch (...) {
      failed(AsyncError::CALLBACK_FAILED);
    }
  }
};

struct CallbackForm {
  Harness &h;
  UiTaskRestart restart{UiTaskRestart::REJECT_RUNNING};
  std::shared_ptr<CallbackState> owner;
  ActionBinding buttonBinding;

  explicit CallbackForm(Harness &value) : h(value), owner(std::make_shared<CallbackState>(h)) {
    owner->initialize();
    buttonBinding = take(bindButton(*h.router, h.button, [this] {
      auto started = owner->start(restart);

      if (!started) {
        h.view->trace.push_back(errorName(started.error()));
      }
    }));
  }

  ~CallbackForm() {
    close();
  }

  UiTaskOutcome outcome() const {
    return owner->outcome;
  }

  void close() {
    buttonBinding.disconnect();
    owner->stop(true);
  }
};
} // namespace async_example

int main() {
  return async_example::run<async_example::CallbackForm>();
}
