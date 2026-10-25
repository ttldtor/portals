# Start a coroutine from a button {#ui_coroutines}

[Русская версия](ru/ui-coroutines.md)

A `ui::async` handler can suspend while the existing scheduler continues servicing
input. This program changes **Ready → Waiting → Completed** after one second.
**Exit** and F10 remain usable during the wait. Repeated **Start** receives
`AsyncError::WOULD_BLOCK` and preserves the running invocation.

[Complete source](examples/guide_async.cpp), linked with `portals::core`:

\include{lineno} guide_async.cpp

The factory runs inside `runApp`'s setup error boundary. The scene retains the
handler, cancellation and coroutine owner; the program stores no binding token.
Capturing `status` by value retains a weak control observation. Removing the
button or closing its scene cancels its suspended task. The coroutine frame is
destroyed before the retained lambda closure and arguments.

## Choose the execution context

Coroutine statements before and after `co_await` run on UI. Waiting itself does
not move expensive work to a worker. Use `context.background<Result>(input, work)`
for a computation: `input` is owning immutable data, and `work` is a separate
worker-safe callable accepting `const Input&` and `std::stop_token`. It returns
`shared_ptr<const Result>` or `expected<shared_ptr<const Result>, AsyncError>`.
A result may itself be `expected<Value, DomainError>`; transport failures do not
replace that domain error.

A fluent async binding opts its scene into the [background service](application.md#application-background),
including timer-only handlers: the default synchronous runner starts two workers
with sixteen pending slots. Ordinary synchronous scenes start no workers.
Hosted sessions require a borrowed executor. Prepared completions wait for the
existing scheduler; workers never resume frames or mutate controls.

## Ownership, errors and repeated actions

Both capturing coroutine lambdas and ordinary coroutine functions are supported.
`ui::async(function, owningArguments...)` copies its prototype for each admitted
invocation. Reference captures and pointers remain caller-owned borrows.
`ui::async(UiTaskRestart::CANCEL_PREVIOUS, function, arguments...)` explicitly
replaces a suspended invocation. An executing invocation still rejects reentry.
Cancellation is cooperative and does not roll back previous UI effects.

Every UI resume enters its own current-scene scope and leaves it before
suspension or return. `ui::quit()` therefore addresses that handler's scene,
including after a wait; no scene remains entered between resumes.

`context.after(duration)` rejects negative delays and deadline overflow;
`context.until(deadline)` uses absolute scheduler milliseconds. Zero delay and
already-due timers still wait for scheduler dispatch. `context.receive<T>(start)`
keeps the checked low-level provider endpoint for custom host integration.

Failed starts and task outcomes retain `ManagedError::async` in existing diagnostics.
A terminal background admission failure is observed once at `ui.async.background`,
with its exact `ManagedError::background` when the executor supplied one. A task
that returns that failure also produces its own `ui.async` completion diagnostic,
without inheriting a background cause. Immediate propagation therefore records two
boundaries of one failure; it does not mean two independent failures. Handling the
await result does not erase the admission diagnostic. A later unrelated task error
never inherits that earlier cause. Temporary `WOULD_BLOCK` admission is retained
for retry and is not reported as a terminal failure.

The default runner reports recoverable diagnostics after cleanup. A recoverable action
failure alone does not change a successful runner exit to 1. Setup and fatal
runner errors retain their existing nonzero exit policy. Low-level `UiAsync`
remains available when the caller needs explicit ownership and outcome policy.

<details>
<summary>Coroutine reference: ownership, admission, errors and cancellation</summary>

**Own the frame and the suspended data**

Create an owner with `UiAsync::create(scheduler, tree, contextNode)` and check
its `expected<unique_ptr<UiAsync>, AsyncError>` result. The scheduler must own
the node's scope; a different scheduler is `INVALID_ARGUMENT`. The tree and
scheduler must outlive the owner. Removing the context node closes the owner
and cancels pending work.

For this low-level API, use a free or static coroutine function with owning parameters.
The fluent `ui::async` wrapper additionally owns a captured coroutine lambda. `UiTask` is lazy: its body starts only after adoption.
Before adoption the move-only task owns the frame; after adoption the UI owner
does. Creation, resume and destruction belong to the creating UI thread.
Destroying a task or owner on another thread terminates. A task owns its
function parameters, but does not automatically own a captured coroutine
lambda's closure. Do not keep borrowed widget or view references across
suspension. In the complete fluent program, the invocation owns the lambda
closure containing the observing `status` reference, and its frame owns `context`.
Neither extends the tree or scheduler lifetime. The fluent binding retires the
frame before releasing the closure. With the low-level API, the caller must close
and destroy the async owner while its tree and scheduler remain alive.

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
