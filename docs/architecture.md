# Architecture

Portals is a C++23 retained widget library. The application owns the runtime,
models, and terminal session. One UI thread applies updates, routes input, and
builds frames. Workers can publish immutable data through bounded queues; they
cannot edit the widget tree.

This page describes the implemented components. A standard application runner,
integrated diagnostic tracing, scrolling extensions, charts, and themes are
under development. Until the runner is available, the application coordinates
the phases below explicitly. The gallery in `examples/gallery/gallery.hpp`
demonstrates the current coordination; the earlier [API sketches](api-design.md)
are proposals, not declarations of implemented interfaces.

## Components and boundaries

![Components and data flow](images/components.svg)

| Component | Responsibility | Does not do |
|---|---|---|
| `TerminalSession` and `InputDecoder` | Bounded native I/O and semantic input | Choose a widget or change a model |
| `Scheduler` | Bounded publication, callback dispatch, timers, cancellation | Read the terminal or run a background UI loop |
| `EventRouter` | Focus, hit testing, action resolution, checked delivery | Own widget storage or perform terminal output |
| `WidgetTree` | Identity, parent ownership, checked borrows, geometry, invalidation | Own application models or define the application's loop |
| `Property<T>` | Immutable value/revision, invalidation and observer receipts | Guarantee atomic multicast or roll back callback side effects |
| `Model<T>` | Immutable snapshots, scoped ranges and bounded requests | Start provider workers or load data during paint |
| Layout and widgets | Measure, arrange and emit clipped render commands | Perform blocking I/O or domain mutation in frame hooks |
| `CellBuffer`, renderer and diff | Consume commands and describe desired cells | Claim that pending bytes are already visible |
| `FrameWriter` | Retain pending output and advance committed frame state | Repair uncertain terminal state by guessing |

The core uses the standard library and native OS interfaces. Unicode processing
is isolated behind `GraphemeText` and the vendored utf8proc dependency. Build
tools, test oracles, fuzzers and Doxygen are not runtime dependencies of an
installed application. See [text](text.md), [rendering](render.md),
[diff](diff.md), and [terminal contracts](terminal.md).

## Who runs the event loop?

The application does. `Scheduler::dispatch()` performs one bounded dispatch;
it does not start a thread or continue calling itself. `TerminalSession::read()`
provides native input with explicit deadlines and stop handling. Scheduler
`waitFor()` waits for scheduler activity; it is not a universal wait covering
the native terminal as well. Their coordination belongs to the application.

A loop must service control input, due timers, and data publications, prepare
the views, build a requested frame, flush output, and wait without starving
other work. These are responsibilities, not a prescription to block on each
source sequentially. In particular, output backpressure must not turn a frame
into an unbounded blocking operation. The final application runner will choose
the concrete waiting and frame-cadence policy.

The scheduler's control and data lanes have separate capacities and dispatch
budgets. `REPLACE_LATEST` can replace a complete pending data snapshot for the
same sender. It must be used **after** lossless domain aggregation: dropping a
display snapshot is different from dropping a financial tick. Queue limits
bound retained owners, not arbitrary payload bytes or callback execution time.
See [scheduler fairness and overflow](scheduler.md).

## From input to a frame

![Action processing and frame construction](images/event-flow.svg)

1. Decode native input into semantic events. Windows exposes decoded events;
   POSIX exposes bytes for the decoder. Do not deliver the same input twice.
2. Dispatch through the existing router on the UI owner. Keyboard delivery
   uses focus; pointer delivery also requires valid arranged geometry and clips.
3. Run the checked handler. A property compares the proposed value with its
   current immutable snapshot. Equal assignment requests no new frame.
4. A changed property invalidates its target and publishes observer notifications.
   Notifications run when the application dispatches the scheduler. A successful
   property receipt can still contain notification rejections.
5. Prepare virtual views between frames using a coherent model pin. The views
   stage bounded, owning presentation data before measure/arrange/paint.
6. Consume the coalesced frame request and build the frame inside
   `WidgetTree::frame`. Render every borrowed command before that callback ends.
7. Submit the resulting owning cell buffer to the writer and flush with a
   deadline. Committed frame state advances only after the output contract is
   satisfied; partial output remains pending or becomes explicitly uncertain.

An arbitrary handler that returns normally does not prove a model change.
Likewise, an empty diff can follow a real domain update that has no visible
effect. Future diagnostic tracing must distinguish these cases rather than
equating "handler returned" with "screen changed".

## Ownership and cancellation

![Ownership and observing handles](images/ownership.svg)

Construct the scheduler before the tree, and keep it alive until the tree and
UI-affine subscriptions are released. Keep the tree alive while properties,
routers and bindings borrowing it are used. Models are explicit application
objects, not children of the widget tree. Retained immutable snapshots may
outlive their UI consumers, subject to their payload's destruction contract.

`Node<T>` owns no widget and has no pointer operators. Its identity contains a
session, slot and generation. `read` and `edit` validate the handle and lend a
reference for the callback only. A copied handle neither prolongs lifetime nor
makes a stale node valid again.

Removing a subtree invalidates its handles and closes node scopes immediately.
An already executing callback retains its borrowed widget until it returns;
physical reclamation follows the outer borrow boundary. Subscription cancellation
prevents new invocations and does not forcibly unwind an active callback or join
a provider worker. See [tree lifetime](widget-tree.md) and
[model requests](model.md).

The frame boundary is another borrow boundary. `TextCommand` borrows text;
recording commands does not transfer ownership. The application renders them
inside the frame and retains the resulting `CellBuffer`, not the borrowed
command list. A paint failure can occur after earlier submissions, so the frame
coordinator must discard a widget's partial command transaction and choose a
clipped placeholder. The tree itself does not roll those submissions back.

## Virtual lists and tables

An immutable `ModelFrame<T>` pins a coherent model revision. Sibling list/table
views can prepare from that same pin without copying business rows. Each view
uses its own scoped range consumer and requests only its viewport plus bounded
overscan. A table reads one range for all columns and formats only visible cells.

`RowId` and `ColumnId` represent stable identities, not current positions.
Selection can survive a reorder; an index alone cannot express that guarantee.
Provider readiness, errors and cancellation remain explicit. The application
dispatches returned request receipts to its provider and later publishes results.
Preparation does not silently start a worker.

`prepareVirtualList` and `prepareVirtualTable` are tree-checked wrappers: they
validate access, account for ancestor visibility, enter a guarded edit, and
invalidate presentation when required. Their free-function spelling is an API
choice; the safety requirement is the checked boundary, not that spelling.
See [VirtualList](virtual-list.md) and [VirtualTable](virtual-table.md).

## Customization without replacing the runtime

Use public widget hooks for custom presentation, model snapshots for a custom
data provider, formatter callbacks for typed table columns, and configured
resource budgets for application limits. Keep the same ownership, frame and
thread boundaries. Reusing these boundaries allows custom components to share
routing, cancellation and rendering with ordinary controls.

Convenience APIs should remove repeated coordination from a normal application
while preserving these extension points and observable errors. A short example
that merely hides its lifetime management in an unnamed helper is not a smaller
public contract. Executable basic and customized examples will accompany the
application runner.

## Implemented guarantees and future work

The current tests cover checked identity, callback self-removal, cancellation,
bounded model ranges, Unicode boundaries, diff application, partial output and
native Windows/Linux cleanup. Fuzzing adds bounded decoder/text campaigns;
it does not prove the absence of all bugs. See [verification](testing.md).

Coroutines, general Undo/Redo, system clipboard adapters and Drag-and-Drop are
future work. Their diagrams and APIs must not be read into the components above.
In particular, this library currently has no coroutine execution contract or
second event loop hidden behind the scheduler.
