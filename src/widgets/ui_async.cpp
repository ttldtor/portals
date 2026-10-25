// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui_async.hpp>

namespace portals::ui::detail {
namespace {
TreeError treeAsyncError(AsyncError error) noexcept {
  switch (error) {
  case AsyncError::WRONG_THREAD:
    return TreeError::WRONG_THREAD;
  case AsyncError::CLOSED:
  case AsyncError::CANCELLED:
    return TreeError::CLOSED;
  case AsyncError::WOULD_BLOCK:
    return TreeError::WOULD_BLOCK;
  case AsyncError::NO_MEMORY:
    return TreeError::NO_MEMORY;
  case AsyncError::IDENTITY_EXHAUSTED:
    return TreeError::IDENTITY_EXHAUSTED;
  case AsyncError::CALLBACK_FAILED:
    return TreeError::CALLBACK_FAILED;
  default:
    return TreeError::INVALID_ARGUMENT;
  }
}
} // namespace

ManagedError asyncFailure(const AsyncSite &site, AsyncError error, std::optional<BackgroundError> background) noexcept {
  ManagedError failure{ManagedPhase::INPUT, site.node.id(), treeAsyncError(error), {}, {}};

  failure.async = error;
  failure.background = background;
  failure.operation = "ui.async";
  failure.operationOrigin = site.origin;

  return failure;
}

AsyncError backgroundAsyncError(BackgroundError error) noexcept {
  switch (error) {
  case BackgroundError::NO_MEMORY:
    return AsyncError::NO_MEMORY;
  case BackgroundError::WOULD_BLOCK:
    return AsyncError::WOULD_BLOCK;
  case BackgroundError::CLOSED:
  case BackgroundError::UNAVAILABLE:
    return AsyncError::CLOSED;
  case BackgroundError::START_FAILED:
    return AsyncError::CALLBACK_FAILED;
  default:
    return AsyncError::INVALID_ARGUMENT;
  }
}

std::expected<AsyncSite, ManagedError> AsyncAccess::site(ManagedScene &scene, Node<Widget> node,
                                                         std::source_location origin) {
  auto allowed = scene.boundary(ManagedPhase::BIND, node);

  if (!allowed) {
    return std::unexpected(allowed.error());
  }

  auto background = SceneAccess::requireBackground(scene);

  if (!background) {
    return std::unexpected(background.error());
  }

  return AsyncSite{&scene, &scene.app_, node, scene.lifetime_, scene.facade_, origin};
}

std::expected<Node<Button>, ManagedError> AsyncAccess::node(ManagedScene &scene, const Ref<Button> &reference) {
  auto owner = eventScene(reference.target());

  if (!owner) {
    return std::unexpected(owner.error());
  }

  if (*owner != &scene) {
    return std::unexpected(ManagedError{ManagedPhase::BIND, {}, TreeError::INVALID_ARGUMENT, {}, {}});
  }

  return reference.node();
}

std::expected<void, AsyncError> AsyncAccess::start(UiAsync &owner, UiTask &&task, UiTaskRestart restart,
                                                   portals::detail::AsyncHooks hooks) {
  return owner.startOwned(std::move(task), restart, std::move(hooks));
}

EditorCommandResult AsyncAccess::result(const AsyncSite &site, std::expected<void, ManagedError> result) {
  if (result) {
    return EditorCommandResult({}, {}, false);
  }

  const auto error = treeAsyncError(result.error().async.value_or(AsyncError::CALLBACK_FAILED));
  bool reported{};

  if (!site.lifetime.expired()) {
    reported = site.scene->reportError(result.error(), {}, site.origin).has_value();
  }

  return EditorCommandResult(std::unexpected(error), site.lifetime, reported);
}

AsyncTimer AsyncAccess::rejectedTimer(AsyncError error) noexcept {
  return AsyncTimer(error);
}

AsyncInvocation::~AsyncInvocation() {
  cancel();
}

bool AsyncInvocation::active() const noexcept {
  return active_;
}

bool AsyncInvocation::live() const noexcept {
  auto anchor = site_.anchor.lock();

  return active_ && anchor && anchor->owner == std::this_thread::get_id() && anchor->state &&
         !site_.lifetime.expired() && anchor->events->scene == site_.scene;
}

bool AsyncInvocation::entered() const noexcept {
  return entered_;
}

AsyncContext AsyncInvocation::context() {
  return AsyncContext(context_, weak_from_this());
}

AsyncTimer AsyncInvocation::after(std::chrono::milliseconds delay) const noexcept {
  if (!live()) {
    return AsyncAccess::rejectedTimer(AsyncError::CLOSED);
  }

  if (delay.count() < 0) {
    return AsyncAccess::rejectedTimer(AsyncError::INVALID_ARGUMENT);
  }

  const auto now = site_.application->now();
  const auto duration = static_cast<SchedulerTime>(delay.count());

  if (duration > std::numeric_limits<SchedulerTime>::max() - now) {
    return AsyncAccess::rejectedTimer(AsyncError::INVALID_ARGUMENT);
  }

  return context_.until(now + duration);
}

portals::detail::AsyncHooks AsyncInvocation::hooks() {
  return {shared_from_this(), resume,
          [](void *value) noexcept {
            static_cast<AsyncInvocation *>(value)->cancel();
          },
          complete};
}

bool AsyncInvocation::resume(void *value, std::coroutine_handle<> frame) noexcept {
  auto &invocation = *static_cast<AsyncInvocation *>(value);

  if (!invocation.live()) {
    return false;
  }

  auto allowed = invocation.site_.application->tree().withScope(invocation.site_.node, [](const UiScope &) {
  });

  if (!allowed) {
    return false;
  }

  invocation.entered_ = true;
  SetupFailurePause setup(invocation.site_.anchor);
  CurrentSceneScope current(invocation.site_.anchor);

  if (auto operation = std::exchange(invocation.operationFailure_, {})) {
    auto failure = asyncFailure(invocation.site_, operation->error, operation->background);

    failure.operation = "ui.async.background";
    (void)invocation.site_.scene->reportError(std::move(failure), {}, invocation.site_.origin);

    // Reporting is deferred until delivery, outside AsyncReceive::await_suspend. An observer may
    // remove/close this invocation; then no user continuation runs, including the handled-error path.
    if (!invocation.live()) {
      return false;
    }

    allowed = invocation.site_.application->tree().withScope(invocation.site_.node, [](const UiScope &) {
    });

    if (!allowed) {
      return false;
    }
  }

  frame.resume();

  return true;
}

void AsyncInvocation::complete(void *value, const UiTaskOutcome &outcome) noexcept {
  auto &invocation = *static_cast<AsyncInvocation *>(value);

  if (!outcome.error || invocation.site_.lifetime.expired()) {
    return;
  }

  auto anchor = invocation.site_.anchor.lock();

  if (!anchor || !anchor->state || anchor->events->scene != invocation.site_.scene) {
    return;
  }

  auto failure = asyncFailure(invocation.site_, *outcome.error);

  (void)invocation.site_.scene->reportError(std::move(failure), {}, invocation.site_.origin);
}

void AsyncInvocation::cancel() noexcept {
  active_ = false;
  pendingFailure_.reset();
  operationFailure_.reset();
  failurePublisher_ = {};

  if (background_) {
    background_->cancel();
  }
}

void AsyncInvocation::fail(BackgroundError error, std::function<std::expected<void, AsyncError>(AsyncError)> publish) {
  failurePublisher_ = std::move(publish);
  fail(backgroundAsyncError(error), error);
}

void AsyncInvocation::fail(AsyncError error, std::optional<BackgroundError> background) noexcept {
  pendingFailure_ = error;
  operationFailure_ = OperationFailure{error, background};

  // A terminal refusal ends this operation, even when its awaiter handles the error and stays alive.
  // Retire before reporting/publication: worker-safe capture destructors and observers may reenter UI.
  auto retired = std::move(background_);

  if (retired) {
    retired->cancel();
    retired.reset();
  }

  if (!live()) {
    return;
  }

  publishFailure();
}

void AsyncInvocation::submit(std::unique_ptr<BackgroundJob> job,
                             std::function<std::expected<void, AsyncError>(AsyncError)> fail) {
  auto *executor = site_.application->background();

  if (!executor) {
    this->fail(BackgroundError::UNAVAILABLE, std::move(fail));

    return;
  }

  failurePublisher_ = std::move(fail);

  try {
    if (!background_) {
      background_ = std::make_unique<BackgroundLatest>(*executor);
    }

    auto admitted = background_->replace(std::move(job));

    if (!admitted) {
      this->fail(backgroundAsyncError(admitted.error()), admitted.error());
    }
  } catch (const std::bad_alloc &) {
    this->fail(AsyncError::NO_MEMORY, {});
  } catch (...) {
    this->fail(AsyncError::CALLBACK_FAILED, {});
  }
}

void AsyncInvocation::publishFailure() noexcept {
  if (!pendingFailure_ || !failurePublisher_) {
    return;
  }

  auto sent = failurePublisher_(*pendingFailure_);

  if (sent || (sent.error() != AsyncError::WOULD_BLOCK && sent.error() != AsyncError::NO_MEMORY)) {
    pendingFailure_.reset();
  }
}

void AsyncInvocation::service() noexcept {
  if (!active_) {
    return;
  }

  if (pendingFailure_) {
    publishFailure();

    return;
  }

  if (background_) {
    try {
      auto ready = background_->service();

      if (!ready) {
        fail(backgroundAsyncError(ready.error()), ready.error());
      }
    } catch (const std::bad_alloc &) {
      fail(AsyncError::NO_MEMORY, {});
    } catch (...) {
      fail(AsyncError::CALLBACK_FAILED, {});
    }
  }
}
} // namespace portals::ui::detail

namespace portals::ui {
AsyncTimer AsyncContext::after(std::chrono::milliseconds delay) const noexcept {
  if (owner_ == std::thread::id{}) {
    return detail::AsyncAccess::rejectedTimer(AsyncError::CLOSED);
  }

  // Affinity precedes observing weak invocation storage or any mutable UI state.
  if (owner_ != std::this_thread::get_id()) {
    return detail::AsyncAccess::rejectedTimer(AsyncError::WRONG_THREAD);
  }

  auto invocation = invocation_.lock();

  return invocation ? invocation->after(delay) : detail::AsyncAccess::rejectedTimer(AsyncError::CLOSED);
}
} // namespace portals::ui
