# UI coroutines on the existing scheduler {#ui_coroutines}

Include `<portals/async.hpp>` for the experimental, optional `UiAsync` adapter.
It owns at most one `UiTask` frame and one suspended wait, using the existing
[scheduler](scheduler.md), queues, clock and dispatch budgets. Callback APIs
remain available. The adapter creates no thread, executor or second event loop.

## Own the frame and the suspended data

Create an owner with `UiAsync::create(scheduler, tree, contextNode)` and check
its `expected<unique_ptr<UiAsync>, AsyncError>` result. The scheduler must own
the node's scope; a different scheduler is `INVALID_ARGUMENT`. The tree and
scheduler must outlive the owner. Removing the context node closes the owner
and cancels pending work.

Use a free or static coroutine function with owning parameters, as in the
complete example below. `UiTask` is lazy: its body starts only after adoption.
Before adoption the move-only task owns the frame; after adoption the UI owner
does. Creation, resume and destruction belong to the creating UI thread.
Destroying a task or owner on another thread terminates. A task owns its
function parameters, but does not automatically own a captured coroutine
lambda's closure. Do not keep borrowed widget or view references across
suspension. The example's shared application `View` still requires its tree
to remain alive; shared ownership is not a lifetime spell for borrowed members.

`owner.context()` returns a weak context, suitable as a value parameter. It
does not extend owner lifetime. Only the adapter's exact `AsyncTimer` and
`AsyncReceive<T>` awaiters are supported; arbitrary foreign awaiters are rejected.

## Wait, admit and observe separately

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

## Cancel and clean up

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

## Complete callback and coroutine consumers

Both installed-package programs bind a button, reject a repeated action, wait
for a timer, encounter a full data queue, retain and retry its result, report
provider failure, replace pending work, deliver from a worker, remove the node
and explicitly close. They verify late-reply rejection, outcomes and cleanup.
The provider's eight retained request slots are an example setting, not a
new library queue. The shared file is part of each complete client footprint.

- [Before: callback state machine](https://github.com/ttldtor/portals/blob/main/tests/package_consumer/async_callback.cpp).
- [After: free owning coroutine and button binding](https://github.com/ttldtor/portals/blob/main/tests/package_consumer/async_coroutine.cpp).
- [Complete shared scenario, provider, errors and cleanup](https://github.com/ttldtor/portals/blob/main/tests/package_consumer/async_scenario.hpp).

### Before

\include{lineno} async_callback.cpp

### After

\include{lineno} async_coroutine.cpp

### Shared scenario

\include{lineno} async_scenario.hpp

Compare each consumer together with the entire shared helper. A shorter
coroutine body alone does not establish a shorter whole client or lower
complexity; these listings make no measured claim. This adapter does not
provide infinite generators, foreign executors or universal async composition.
