// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include "async_scenario.hpp"

namespace async_example {
// A free coroutine function owns every parameter needed across suspension. No closure borrow.
UiTask load(UiAsyncContext context, SchedulerTime deadline, std::shared_ptr<Provider> provider,
            std::shared_ptr<View> view) {
  auto timer = co_await context.until(deadline);

  if (!timer) {
    co_return std::unexpected(timer.error());
  }

  auto result = co_await context.receive<int>([provider](AsyncReply<int> reply) {
    provider->start([reply](Result result) {
      return result ? reply.complete(*result) : reply.fail(result.error());
    });
  });
  view->apply(result);

  if (!result) {
    co_return std::unexpected(result.error());
  }

  co_return std::expected<void, AsyncError>{};
}

struct CoroutineForm {
  Harness &h;
  UiTaskRestart restart{UiTaskRestart::REJECT_RUNNING};
  std::unique_ptr<UiAsync> owner;
  ActionBinding buttonBinding;

  explicit CoroutineForm(Harness &value) : h(value) {
    owner = take(UiAsync::create(*h.scheduler, *h.tree, h.button));
    buttonBinding = take(bindButton(*h.router, h.button, [this] {
      auto started = owner->start(load(owner->context(), h.now + 10, h.provider, h.view), restart);

      if (!started) {
        h.view->trace.push_back(errorName(started.error()));
      }
    }));
  }

  ~CoroutineForm() {
    close();
  }

  UiTaskOutcome outcome() const {
    return take(owner->outcome());
  }

  void close() {
    buttonBinding.disconnect();
    take(owner->close());
  }
};
} // namespace async_example

int main() {
  return async_example::run<async_example::CoroutineForm>();
}
