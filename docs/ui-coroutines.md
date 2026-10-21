# Start a coroutine from a button {#ui_coroutines}

[Русская версия](ru/ui-coroutines.md)

The optional experimental [`UiAsync`](../include/portals/async.hpp) adapter lets
a button action suspend while the existing [scheduler](scheduler.md) continues
servicing input. This program changes **Ready → Waiting → Completed** after a
one-second timer. **Exit** and F10 work while it waits. Repeated **Start** actions
are rejected with `WOULD_BLOCK` while the earlier invocation is running.

This fragment assumes `using namespace portals`, a successfully created
`UiAsync &owner`, `Application &app`, published `ui::Ref<Button> start` and
`ui::Ref<Label> status`, and `delayMilliseconds = 1000`. Register the handler
inside `ui::configure`, returning its checked result to the scene. Owner setup
and cleanup appear in the complete program below.

```cpp
UiTask finishLater(UiAsyncContext context, SchedulerTime deadline,
                   ui::Ref<Label> status) {
  status.text() = "Waiting";

  auto waited = co_await context.until(deadline);

  status.text() = waited ? "Completed" : "Wait failed";

  co_return waited;
}

// Inside the UI configuration callback, after successful
// owner creation.
return start.onClick([&owner, &app, status] {
  auto started = owner.start(finishLater(owner.context(),
    app.now() + delayMilliseconds,
    status));

  if (!started && started.error() != AsyncError::WOULD_BLOCK) {
    status.text() = "Start failed";
  }
});
```

The fragment focuses on waiting and updating the label. The complete program also
retains typed failures, checks owner creation and releases the owner at shutdown.
The borrowed `owner` and `app` must outlive the registered handler; `Ref` observes
the control and does not extend its lifetime. The shorter fluent `ui::async`
adapter remains **TBD — M6**.

<details>
<summary>Complete program</summary>

[Buildable source](examples/guide_async.cpp), linked with `portals::core`.

```cpp
#include <portals/async.hpp>
#include <portals/ui.hpp>

#include <memory>
#include <optional>

namespace {
struct AsyncAction {
  std::unique_ptr<portals::UiAsync> owner;
  std::optional<portals::AsyncError> error;
};

portals::UiTask finishLater(
  portals::UiAsyncContext context,
  portals::SchedulerTime deadline,
  portals::ui::Ref<portals::Label> status,
  std::shared_ptr<AsyncAction> action) {
  status.text() = "Waiting";

  auto waited = co_await context.until(deadline);

  if (!waited) {
    action->error = waited.error();
    status.text() = "Wait failed";

    co_return std::unexpected(waited.error());
  }

  status.text() = "Completed";

  co_return std::expected<void, portals::AsyncError>{};
}
} // namespace

int main() {
  using namespace portals;

  constexpr SchedulerTime delayMilliseconds = 1000;
  auto action = std::make_shared<AsyncAction>();
  ui::Ref<Label> status;
  ui::Ref<Button> start;
  ui::Ref<Button> exit;

  auto scene = ui::scene(ui::vbox(
    ui::label(status, "Ready"),
    ui::button(start, "Start"),
    ui::button(exit, "Exit")));

  scene.onStop([action] {
    if (action->owner) {
      auto closed = action->owner->close();

      if (!closed) {
        action->error = closed.error();
      }

      action->owner.reset();
    }
  });

  const int result = scene
    | ui::configure(
      [action, status](ManagedScene &, Application &app) {
        auto created = UiAsync::create(app.scheduler(),
          app.tree(),
          app.root());

        if (!created) {
          action->error = created.error();
          status.text() = "Async setup failed";

          return;
        }

        action->owner = std::move(*created);
      })
    | ui::configure(
      [action, status, start](ManagedScene &, Application &app) {
        auto bound = start.onClick([action, status, &app] {
          if (!action->owner) {
            return;
          }

          auto started = action->owner->start(
            finishLater(action->owner->context(),
              app.now() + delayMilliseconds,
              status,
              action));

          if (!started && started.error() != AsyncError::WOULD_BLOCK) {
            action->error = started.error();
            status.text() = "Start failed";
          }
        });

        return bound;
      })
    | ui::onClick(exit, ui::quit)
    | ui::onKey("F10", ui::quit)
    | ui::runApp();

  return result != 0 || action->error ? 1 : 0;
}
```

</details>

Build the complete [source](examples/guide_async.cpp) with `portals::core` as in
[package setup](package.md). `Application::now()` supplies the same monotonic
millisecond clock as the timer. The wait suspends; it performs no blocking sleep.
This is coroutine concurrency on the UI thread. It starts no background computation
or I/O and creates no thread. Long computation in the coroutine still blocks input.

`scene.onStop` closes and destroys the async owner before the scene releases its
application. Closing while suspended destroys the frame; the body does not resume
to display **Completed**. The coroutine is a free function with owning parameters;
its `Ref` observes the label and does not own the widget. Text mutations use the
ordinary managed reporting and redraw path. The application retains timer/start
failures in `AsyncAction::error` and returns 1 after cleanup. The coroutine returns the typed wait result: a failed wait sets the adapter's
outcome to `FAILED`, while a successful return sets it to `SUCCEEDED`.

## Wait for an actual provider result

`co_await context.receive<T>(start)` waits for an immutable provider result.
The UI invokes `start` with a worker-safe `AsyncReply<T>` endpoint; an application-owned
worker can publish `shared_ptr<const T>` through `reply.complete(...)` or a typed
failure through `reply.fail(...)`. The application starts, cancels and joins that
worker. Retain the exact reply and result and retry if queue admission is rejected;
acceptance does not mean the coroutine has resumed. Do not dispatch recursively
inside `start`, or mutate controls from a worker. Detailed admission and lifetime
contracts follow below.

A fluent `ui::async` adapter for functions/coroutine lambdas is **TBD — M6** in
[planned features](planned-features.md). Use the explicit owner above with the
current API. Foreign awaiters and infinite generators are outside this adapter.

<details>
<summary>Coroutine reference: ownership, admission, errors and cancellation</summary>

**Own the frame and the suspended data**

Create an owner with `UiAsync::create(scheduler, tree, contextNode)` and check
its `expected<unique_ptr<UiAsync>, AsyncError>` result. The scheduler must own
the node's scope; a different scheduler is `INVALID_ARGUMENT`. The tree and
scheduler must outlive the owner. Removing the context node closes the owner
and cancels pending work.

Use a free or static coroutine function with owning parameters, as in
`finishLater` in the complete program above. `UiTask` is lazy: its body starts only after adoption.
Before adoption the move-only task owns the frame; after adoption the UI owner
does. Creation, resume and destruction belong to the creating UI thread.
Destroying a task or owner on another thread terminates. A task owns its
function parameters, but does not automatically own a captured coroutine
lambda's closure. Do not keep borrowed widget or view references across
suspension. In the complete program, the frame owns copies of `context`,
`deadline`, `status` and `action`. The `shared_ptr<AsyncAction>` retains application
action state; `status` is an observing `Ref<Label>`, and `context` observes the
async owner weakly. Neither extends the tree or scheduler lifetime.
`scene.onStop` closes and destroys the async owner while those services remain alive.

`owner.context()` returns a weak context, suitable as a value parameter. It
does not extend owner lifetime. Only the adapter's exact `AsyncTimer` and
`AsyncReceive<T>` awaiters are supported; arbitrary foreign awaiters are rejected.

**Wait, admit and observe separately**

`co_await context.until(deadline)` waits for absolute monotonic milliseconds
in the scheduler's clock and returns `expected<void, AsyncError>`. Even an
already-due deadline waits for dispatch and the timer budget.

`co_await context.receive<T>(start)` installs a completion registration before
calling the owning UI `std::function<void(AsyncReply<T>)>` provider callback.
The await result is `expected<shared_ptr<const T>, AsyncError>`. `T`, reachable
data and their deleters must support immutable shared ownership and release on
any releasing thread. Worker replies own only the existing publisher and atomic
admission state; they retain no frame, UI callback storage or control owner.
Do not recursively dispatch from the provider-start callback.

`reply.complete(value)` requires a non-null immutable owner; `reply.fail(error)`
publishes a typed failure. Both return `expected<PublishResult, AsyncError>`.
One endpoint admits exactly one `QUEUED` result. Another publication before
delivery returns `WOULD_BLOCK`; after delivery or cancellation it returns
`CLOSED`. Full queue or `NO_MEMORY` rejection leaves admission open: the provider
must retain the exact reply and result and explicitly retry. An accepted success
cannot be replaced with a different failure. Synchronous completion queues data;
the body continues only through scheduler dispatch.

`owner.start(std::move(task), policy)` performs initial resume on UI.
`REJECT_RUNNING` preserves an active invocation; `CANCEL_PREVIOUS` retires a
suspended invocation before adoption. Both return `WOULD_BLOCK` while a frame
is executing. Rejection before adoption preserves caller ownership. Closure
or removal during the first resume returns `CLOSED` after consuming the task;
cancellation or displacement during admission can return `CANCELLED`.
Check `outcome()` separately: its retained `UiTaskOutcome` records the latest
adopted generation and `IDLE`, `RUNNING`, `SUCCEEDED`, `CANCELLED` or `FAILED`,
with the corresponding optional error. Queue acceptance and start admission
do not prove successful business effects.

Body and provider exceptions become `NO_MEMORY` or `CALLBACK_FAILED` inside
adapter boundaries. Registration rejection is a typed await failure.
Frame/parameter allocation and callback construction in the caller expression
can throw before the adapter is entered. Other typed failures include
`INVALID_ARGUMENT`, `WRONG_THREAD`, `CLOSED`, `CANCELLED`, `WOULD_BLOCK` and
`IDENTITY_EXHAUSTED`; check results instead of dropping them.

**Cancel and clean up**

`cancel()` records cancellation and closes the pending endpoint. A suspended
frame is destroyed immediately on UI; executing C++ runs to suspend or return,
then the frame is released. Cancellation does not resume the body to let it
perform cleanup, interrupt running code or join provider workers. Use owning
parameters and RAII for frame cleanup, and close/join provider resources under
their own contract. Invocation generations reject late completions.

`close()` permanently closes the owner and is idempotent. The last outcome
remains UI-readable after closure or node removal; closing an idle/completed
owner preserves it. Disconnect action bindings before their captures expire.
Keep the tree and scheduler alive until the owner has been closed and destroyed.
The adapter consumes one permanent context registration and at most one wait
registration. Existing `SchedulerOptions` capacities and budgets remain the
limits. It requests no redraw by itself; application effects invalidate through
the ordinary control APIs.


</details>
