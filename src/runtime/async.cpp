// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/async.hpp>

namespace portals {
namespace detail {
struct AsyncScopeLifetime;

struct AsyncControl {
  AsyncControl(Scheduler &executor, WidgetTree &value, Node<Widget> node)
      : scheduler(executor), tree(value), context(node) {
  }

  Scheduler &scheduler;
  WidgetTree &tree;
  Node<Widget> context;
  const std::thread::id owner = std::this_thread::get_id();
  std::coroutine_handle<UiTask::promise_type> frame{};
  std::weak_ptr<AsyncWait> wait;
  Subscription sentinel;
  Publisher<int> scopeWake;
  std::weak_ptr<AsyncScopeLifetime> scopeLifetime;
  UiTaskOutcome outcome;
  AsyncHooks hooks;
  std::uint64_t generation{};
  bool alive{true};
  bool running{};
  void stop(AsyncError error, bool close) noexcept;
};

struct AsyncScopeLifetime {
  std::weak_ptr<AsyncControl> control;
  bool armed{};

  ~AsyncScopeLifetime() {
    if (armed) {
      if (auto state = control.lock()) {
        state->stop(AsyncError::CLOSED, true);
      }
    }
  }
};

AsyncInvocation asyncInvocation(const std::weak_ptr<AsyncControl> &control) noexcept {
  auto state = control.lock();
  if (!state || state->owner != std::this_thread::get_id() || !state->alive) {
    return {};
  }
  return {state, state->generation, state->wait, state->scopeWake};
}

bool asyncInvocationCurrent(const AsyncInvocation &identity) noexcept {
  auto state = identity.control.lock();
  auto wait = identity.wait.lock();
  return state && state->owner == std::this_thread::get_id() && state->alive && wait &&
         state->generation == identity.generation && state->wait.lock() == wait;
}

void asyncCancelInvocation(const AsyncInvocation &identity) noexcept {
  if (asyncInvocationCurrent(identity)) {
    auto state = identity.control.lock();
    state->stop(AsyncError::CLOSED, false);
  }
}

AsyncError asyncError(SchedulerError error) noexcept {
  switch (error) {
  case SchedulerError::WRONG_THREAD:
    return AsyncError::WRONG_THREAD;
  case SchedulerError::WOULD_BLOCK:
  case SchedulerError::REENTRANT_DISPATCH:
    return AsyncError::WOULD_BLOCK;
  case SchedulerError::NO_MEMORY:
    return AsyncError::NO_MEMORY;
  case SchedulerError::IDENTITY_EXHAUSTED:
    return AsyncError::IDENTITY_EXHAUSTED;
  case SchedulerError::CALLBACK_FAILED:
    return AsyncError::CALLBACK_FAILED;
  case SchedulerError::CLOSED:
    return AsyncError::CLOSED;
  default:
    return AsyncError::INVALID_ARGUMENT;
  }
}

AsyncError asyncError(TreeError error) noexcept {
  switch (error) {
  case TreeError::WRONG_THREAD:
    return AsyncError::WRONG_THREAD;
  case TreeError::WOULD_BLOCK:
  case TreeError::NODE_LIMIT:
    return AsyncError::WOULD_BLOCK;
  case TreeError::NO_MEMORY:
    return AsyncError::NO_MEMORY;
  case TreeError::IDENTITY_EXHAUSTED:
    return AsyncError::IDENTITY_EXHAUSTED;
  case TreeError::CALLBACK_FAILED:
    return AsyncError::CALLBACK_FAILED;
  case TreeError::STALE_NODE:
  case TreeError::CLOSED:
    return AsyncError::CLOSED;
  default:
    return AsyncError::INVALID_ARGUMENT;
  }
}

std::expected<void, AsyncError> asyncCheck(const std::shared_ptr<AsyncControl> &state) {
  if (!state) {
    return std::unexpected(AsyncError::CLOSED);
  }

  if (state->owner != std::this_thread::get_id()) {
    return std::unexpected(AsyncError::WRONG_THREAD);
  }

  if (!state->alive) {
    return std::unexpected(AsyncError::CLOSED);
  }

  auto allowed = state->tree.withScope(state->context, [](const UiScope &) {
  });

  if (!allowed) {
    return std::unexpected(asyncError(allowed.error()));
  }

  return {};
}

std::expected<void, AsyncError> asyncInstallWait(const std::shared_ptr<AsyncControl> &state, std::uint64_t generation,
                                                 std::shared_ptr<AsyncWait> wait) {
  auto allowed = asyncCheck(state);

  if (!allowed) {
    return allowed;
  }

  if (generation != state->generation || state->outcome.status != UiTaskStatus::RUNNING) {
    return std::unexpected(AsyncError::CANCELLED);
  }

  if (!state->wait.expired()) {
    return std::unexpected(AsyncError::WOULD_BLOCK);
  }

  state->wait = wait;

  return {};
}

void asyncFinishWait(const std::shared_ptr<AsyncControl> &state, const AsyncWait *wait) noexcept {
  auto current = state->wait.lock();

  if (current.get() == wait) {
    state->wait.reset();
  }
}

Scheduler &asyncScheduler(const std::shared_ptr<AsyncControl> &state) noexcept {
  return state->scheduler;
}

WidgetTree &asyncTree(const std::shared_ptr<AsyncControl> &state) noexcept {
  return state->tree;
}

Node<Widget> asyncNode(const std::shared_ptr<AsyncControl> &state) noexcept {
  return state->context;
}

void AsyncControl::stop(AsyncError error, bool close) noexcept {
  // Commit cancellation and detach owners before arbitrary capture/parameter destructors run.
  if (generation != std::numeric_limits<std::uint64_t>::max()) {
    ++generation;
  } else {
    close = true;
  }

  if (outcome.status == UiTaskStatus::RUNNING) {
    outcome.status = UiTaskStatus::CANCELLED;
    outcome.error = error;
  }

  if (close) {
    alive = false;
  }

  auto retiredWait = wait.lock();
  wait.reset();
  auto retiredFrame = running ? std::coroutine_handle<UiTask::promise_type>{} : std::exchange(frame, {});
  auto retiredHooks = running ? hooks : std::exchange(hooks, {});
  Subscription retiredSentinel;

  if (close) {
    if (auto lifetime = scopeLifetime.lock()) {
      lifetime->armed = false;
    }

    scopeLifetime.reset();
    retiredSentinel = std::move(sentinel);
  }

  if (retiredHooks.cancel) {
    retiredHooks.cancel(retiredHooks.owner.get());
  }

  if (retiredWait) {
    retiredWait->cancel(error);
  }

  (void)retiredSentinel.disconnect();

  if (retiredFrame) {
    retiredFrame.destroy();
  }
  // In particular, captured coroutine lambdas outlive every frame parameter destructor.
}

void asyncResume(const std::shared_ptr<AsyncControl> &state, std::uint64_t generation) {
  if (!state->alive || state->running || state->generation != generation || !state->frame) {
    return;
  }

  auto frame = state->frame;
  state->running = true;
  const bool entered =
    state->hooks.resume ? state->hooks.resume(state->hooks.owner.get(), frame) : (frame.resume(), true);
  state->running = false;

  if (!entered) {
    state->stop(AsyncError::CLOSED, true);

    return;
  }

  if (state->frame != frame) {
    std::terminate();
  }

  if (!frame.done() && state->alive && state->generation == generation) {
    return;
  }

  if (frame.done() && state->alive && state->generation == generation) {
    auto result = frame.promise().result;

    state->outcome.status = result ? UiTaskStatus::SUCCEEDED : UiTaskStatus::FAILED;
    state->outcome.error = result ? std::optional<AsyncError>{} : result.error();
  }

  state->frame = {};
  auto retiredHooks = std::exchange(state->hooks, {});
  const auto outcome = state->outcome;
  auto retiredWait = state->wait.lock();
  state->wait.reset();

  if (retiredHooks.cancel) {
    retiredHooks.cancel(retiredHooks.owner.get());
  }

  if (retiredWait) {
    retiredWait->cancel(AsyncError::CANCELLED);
  }

  // No mutable state writes after destruction: a parameter destructor may start a newer task.
  frame.destroy();

  if (outcome.status == UiTaskStatus::FAILED && retiredHooks.complete) {
    retiredHooks.complete(retiredHooks.owner.get(), outcome);
  }
}
} // namespace detail

UiTask UiTask::promise_type::get_return_object() noexcept {
  return UiTask(Handle::from_promise(*this));
}

void UiTask::promise_type::unhandled_exception() noexcept {
  try {
    throw;
  } catch (const std::bad_alloc &) {
    result = std::unexpected(AsyncError::NO_MEMORY);
  } catch (...) {
    result = std::unexpected(AsyncError::CALLBACK_FAILED);
  }
}

UiTask::~UiTask() {
  if (frame_) {
    if (frame_.promise().owner != std::this_thread::get_id()) {
      std::terminate();
    }

    frame_.destroy();
  }
}

UiTask::UiTask(UiTask &&other) noexcept : frame_(std::exchange(other.frame_, {})) {
}

UiTask &UiTask::operator=(UiTask &&other) noexcept {
  if (this == &other) {
    return *this;
  }

  auto old = std::exchange(frame_, std::exchange(other.frame_, {}));

  if (old) {
    if (old.promise().owner != std::this_thread::get_id()) {
      std::terminate();
    }

    old.destroy();
  }

  return *this;
}

struct AsyncTimer::Wait final : detail::AsyncWait {
  Subscription subscription;
  std::expected<void, AsyncError> result = std::unexpected(AsyncError::CANCELLED);
  bool settled{};

  void cancel(AsyncError error) noexcept override {
    if (!settled) {
      result = std::unexpected(error);
      settled = true;
    }

    (void)subscription.disconnect();
  }
};

bool AsyncTimer::await_suspend(std::coroutine_handle<UiTask::promise_type> frame) {
  if (rejected_) {
    return false;
  }

  auto state = control_.lock();
  auto checked = detail::asyncCheck(state);

  if (!checked) {
    fallback_ = checked.error();
    return false;
  }

  if (frame.promise().control.lock() != state) {
    fallback_ = AsyncError::INVALID_ARGUMENT;
    return false;
  }

  try {
    wait_ = std::make_shared<Wait>();
    auto installed = detail::asyncInstallWait(state, frame.promise().generation, wait_);

    if (!installed) {
      wait_->cancel(installed.error());
      return false;
    }

    std::expected<Subscription, SchedulerError> subscription = std::unexpected(SchedulerError::CLOSED);
    auto allowed = detail::asyncTree(state).withScope(detail::asyncNode(state), [&](const UiScope &scope) {
      subscription = detail::asyncScheduler(state).at(
        scope, deadline_,
        [weak = std::weak_ptr(wait_), control = std::weak_ptr(state), generation = frame.promise().generation] {
          auto wait = weak.lock();
          auto retained = control.lock();

          if (!wait || !retained || wait->settled) {
            return;
          }

          wait->result = {};
          wait->settled = true;
          wait->cancel(AsyncError::CANCELLED);
          detail::asyncResume(retained, generation);
        });
    });

    if (!allowed || !subscription) {
      wait_->cancel(!allowed ? detail::asyncError(allowed.error()) : detail::asyncError(subscription.error()));

      return false;
    }

    wait_->subscription = std::move(*subscription);

    return true;
  } catch (const std::bad_alloc &) {
    fallback_ = AsyncError::NO_MEMORY;
  } catch (...) {
    fallback_ = AsyncError::CALLBACK_FAILED;
  }

  if (wait_) {
    wait_->cancel(fallback_);
  }

  return false;
}

std::expected<void, AsyncError> AsyncTimer::await_resume() noexcept {
  if (auto state = control_.lock()) {
    detail::asyncFinishWait(state, wait_.get());
  }

  return wait_ ? wait_->result : std::expected<void, AsyncError>(std::unexpected(fallback_));
}

std::expected<std::unique_ptr<UiAsync>, AsyncError> UiAsync::create(Scheduler &scheduler, WidgetTree &tree,
                                                                    Node<Widget> context) {
  auto allowed = tree.withScope(context, [](const UiScope &) {
  });

  if (!allowed) {
    return std::unexpected(detail::asyncError(allowed.error()));
  }

  try {
    auto state = std::make_shared<detail::AsyncControl>(scheduler, tree, context);
    auto lifetime = std::make_shared<detail::AsyncScopeLifetime>();

    lifetime->control = state;

    std::expected<SchedulerConnection<int>, SchedulerError> sentinel = std::unexpected(SchedulerError::CLOSED);

    allowed = tree.withScope(context, [&](const UiScope &scope) {
      sentinel = scheduler.connect<int>(scope, [lifetime](std::shared_ptr<const int>) {
      });
    });

    if (!allowed || !sentinel) {
      return std::unexpected(!allowed ? detail::asyncError(allowed.error()) : detail::asyncError(sentinel.error()));
    }

    state->sentinel = std::move(sentinel->subscription);
    state->scopeWake = sentinel->publisher;
    state->scopeLifetime = lifetime;
    lifetime->armed = true;

    return std::unique_ptr<UiAsync>(new UiAsync(std::move(state)));
  } catch (const std::bad_alloc &) {
    return std::unexpected(AsyncError::NO_MEMORY);
  }
}

UiAsync::~UiAsync() {
  if (state_->owner != std::this_thread::get_id()) {
    std::terminate();
  }

  auto retained = std::move(state_);
  retained->stop(AsyncError::CLOSED, true);
}

UiAsyncContext UiAsync::context() const {
  if (state_->owner != std::this_thread::get_id()) {
    return {};
  }

  return UiAsyncContext(state_);
}

std::expected<void, AsyncError> UiAsync::start(UiTask &&task, UiTaskRestart restart) {
  return startOwned(std::move(task), restart, {});
}

std::expected<void, AsyncError> UiAsync::startOwned(UiTask &&task, UiTaskRestart restart, detail::AsyncHooks hooks) {
  auto state = state_;
  auto allowed = detail::asyncCheck(state);

  if (!allowed) {
    return allowed;
  }

  if (!task.frame_ || (restart != UiTaskRestart::REJECT_RUNNING && restart != UiTaskRestart::CANCEL_PREVIOUS)) {
    return std::unexpected(AsyncError::INVALID_ARGUMENT);
  }

  if (task.frame_.promise().owner != state->owner) {
    return std::unexpected(AsyncError::WRONG_THREAD);
  }

  if (state->running || (state->frame && restart == UiTaskRestart::REJECT_RUNNING)) {
    return std::unexpected(AsyncError::WOULD_BLOCK);
  }

  if (state->generation == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(AsyncError::IDENTITY_EXHAUSTED);
  }

  if (state->frame) {
    auto previous = state->generation;
    state->stop(AsyncError::CANCELLED, false);
    allowed = detail::asyncCheck(state);

    if (!allowed) {
      return allowed;
    }

    if (state->generation != previous + 1) {
      return std::unexpected(AsyncError::CANCELLED);
    }

    // Retirement invokes arbitrary frame-parameter destructors. The caller-owned incoming
    // task may have been emptied or replaced without changing this owner's generation.
    if (!task.frame_) {
      return std::unexpected(AsyncError::INVALID_ARGUMENT);
    }

    if (task.frame_.promise().owner != state->owner) {
      return std::unexpected(AsyncError::WRONG_THREAD);
    }
  }

  if (state->generation == std::numeric_limits<std::uint64_t>::max()) {
    return std::unexpected(AsyncError::IDENTITY_EXHAUSTED);
  }

  auto generation = ++state->generation;
  auto frame = std::exchange(task.frame_, {});

  frame.promise().control = state;
  frame.promise().generation = generation;
  state->frame = frame;
  state->hooks = std::move(hooks);
  state->outcome = {UiTaskStatus::RUNNING, {}, generation};
  detail::asyncResume(state, generation);

  if (!state->alive) {
    return std::unexpected(AsyncError::CLOSED);
  }

  if (state->generation != generation) {
    return std::unexpected(AsyncError::CANCELLED);
  }

  return {};
}

std::expected<void, AsyncError> UiAsync::cancel() {
  auto state = state_;

  if (state->owner != std::this_thread::get_id()) {
    return std::unexpected(AsyncError::WRONG_THREAD);
  }

  state->stop(AsyncError::CANCELLED, false);

  return {};
}

std::expected<void, AsyncError> UiAsync::close() {
  auto state = state_;

  if (state->owner != std::this_thread::get_id()) {
    return std::unexpected(AsyncError::WRONG_THREAD);
  }

  if (state->alive) {
    state->stop(AsyncError::CLOSED, true);
  }

  return {};
}

std::expected<UiTaskOutcome, AsyncError> UiAsync::outcome() const {
  auto state = state_;

  if (state->owner != std::this_thread::get_id()) {
    return std::unexpected(AsyncError::WRONG_THREAD);
  }

  return state->outcome;
}
} // namespace portals
