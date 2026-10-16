# Row, Column and size policies

[Русская версия](ru/layout.md)

All sizes are integer terminal columns/rows. Rectangles are parent-local,
half-open values; `WidgetTree::arrange` translates and intersects clips through
the checked geometry API. Row orders visible children left to right; Column
orders them top to bottom. Disabled children still occupy space. Hidden children
and their descendants are never measured or arranged and contribute no gap.

Each child may implement the const `LayoutItem` interface. Ordinary widgets use
automatic policies and their guarded `Widget::measure` result. `SizedWidget`
owns a convenient policy pair. Each axis has minimum, preferred, maximum,
mode and a positive flex weight. `INTRINSIC_EXTENT` selects the measured size;
other preferred values are nonnegative. Preferred is clamped to minimum/maximum.
Invalid bounds, modes, weights and options return `TreeError::INVALID_ARGUMENT`.

Fixed children preserve their clamped preferred extent. Automatic children use
their preferred extent. Flex children also start at their clamped preferred
extent, then share spare space in proportion to weights. Each round assigns
integer quotients and gives remaining cells in stable child order, skipping
children whose maximum has been reached. Capped shares are redistributed; when
all flex maxima are reached, unused space remains. Equal weights in 11 cells
therefore produce widths 6 and 5, in insertion order.

When space is short, later automatic/flex children yield preferred cells first,
down to their minima. Fixed extents and all minima survive. `LayoutOverflow::CLIP`
preserves these extents even when children/gaps/padding exceed the available
space; ancestor clips remove the overflow from painting and hit testing.
There is no negative size or implicit compression below minimum. Representational
overflow is an explicit `INVALID_GEOMETRY`, rather than wrapping coordinates.

Padding is applied on each side; gaps occur only between participating children.
Main alignment applies to the remaining group space after allocation. Start and
stretch start at the leading inset, center gives floor(free/2) before the group,
and end consumes all spare space before it. Cross stretch (or cross flex) fills
the available extent subject to bounds; fixed remains fixed. Cross start/center/end
aligns the resulting child extent using the same floor rule. Underflow aligns
at the leading inset because there is no spare space.

## Frame ownership and errors

`layoutTree` is an external two-phase owner-thread traversal. It obtains owning
structure/policy/measurement snapshots through `inspect` and `read`, measures
children before parents, then calls guarded `arrange` on parents before children.
Its temporary traversal table dies when the pass returns; it is not another
retained tree. A plain parent supports a single-child viewport passthrough;
multiple children require a `Layout` container.

The caller must invoke `layoutTree` **inside an already active `tree.frame`**.
The pass does not create a frame. This precondition is contractual: the current
tree API has no active-frame query. One frame can encompass layout, paint and
rendering of borrowed commands:

A preliminary pass may instead capture an owning `LayoutSnapshot`, as the
[layout-driven view preparation](virtual-preparation.md) runner does. It does
not retain widget borrows or render commands; final layout, paint and rendering
still share one frame.

```cpp
// Between frames, after a viewport change:
auto invalidation = tree.invalidate(root, portals::Invalidation::LAYOUT);
if (!invalidation) {
  return invalidation;
}

std::expected<void, portals::TreeError> layout;
auto frame = tree.frame([&] {
  layout = portals::layoutTree(tree, root, viewport, viewport);

  if (!layout) {
    return;
  }

  // Paint the arranged tree and render borrowed commands before returning.
});
```

`setSizePolicy` and `setLinearOptions` validate owning values, edit on the
UI owner between frames, and request layout invalidation. Construction and other
custom policy sources must follow the same invalidation discipline. Viewport
changes likewise require `invalidate(layout)` before entering the next frame.
After a failed pass the coordinator must abort use of that pass, including paint
and hit testing; the solver does not promise transactional rollback or automatic
invalidation from inside a frame. Invalidated geometry is never a valid old layout.

All tree errors propagate; arithmetic failures return `INVALID_GEOMETRY`, library
allocation failures return `NO_MEMORY`, and thrown custom callbacks follow
`tree.read`/measure/arrange's `CALLBACK_FAILED` boundary. No callback or span is
retained. Pure `Layout` and `LayoutItem` callbacks run inside a checked read borrow
in the enclosing frame and must not call tree operations, recursively dispatch
layout, mutate models or perform I/O. A scoped thread-local guard technically
rejects recursive `layoutTree` calls with `WOULD_BLOCK`. The tree technically
protects its ordinary Widget hooks and rejects editing inside frames. Arbitrary
model mutation/I/O and direct tree calls from a custom pure callback remain
contract violations; the library does not claim to sandbox user C++ code.

## Observe final arranged bounds

Register `ui::onLayout(ref, callback)` on a scene to receive the initial valid
geometry and subsequent changes to its local, surface or clip bounds. The scene
owns the registration and captures. The callback runs on the UI thread during a
subsequent scheduler dispatch, after the successful final layout, paint and owning
render have left the tree frame. Preliminary view-preparation passes do not notify.
This event identifies arranged geometry; terminal output may still be pending.

[Example source](examples/guide_layout_observe.cpp).

\include guide_layout_observe.cpp

`geometry.local` is parent-relative; `surface` includes ancestor translations;
`clip` intersects the viewport and ancestor clips. Each `NodeGeometry` owns its
values: copy the callback argument to retain it. Equal bounds do not notify or
request another frame. A callback may change controls or remove/close its scene;
its executing capture survives retirement. Changes request later work through
the normal runner. At EOF the existing frame cap remains in force: callback
mutations may remain dirty without another final frame or notification.

For a checked read, use `ref.geometry()` or `ref.size()`, returning
`std::expected<NodeGeometry, ManagedError>` and `std::expected<CellSize, ManagedError>`.
Size is the local arranged extent. Reading never starts layout or reports an
error. Before a successful final frame, while hidden, or after layout invalidation
or rearrangement, reads return `TreeError::INVALID_GEOMETRY` until the next final
publication. Equal-valued replacement geometry still revokes the old read.
Unpublished/destroyed references return `CLOSED`, removed identities retain their
typed stale-node error, and foreign threads return `WRONG_THREAD` before owner
access. Disabled controls keep their geometry.

Hidden ancestors cancel pending observations. Showing a hidden subtree produces
a new initial valid observation, including hide/show between final frames. Removal and close
cancel queued observations; already running callbacks finish safely. Runtime
registration through `ref.onLayout(callback)` follows the same contract and first
queues its current final value after the current dispatch, if one is available.

The existing scheduler DATA capacity, subscription capacity and data budget bound
the queue and deliveries. `ManagedOptions::maxLayoutHandlers` defaults to 256.
Newest pending values replace older ones. Queue-full recovery makes at most
`maxLayoutPublicationRetries` attempts (default 3) without requesting redraw.
Exhaustion cancels that pending value and reports `SchedulerError::WOULD_BLOCK`
once through the managed error channel; it does not mark it delivered. A later
final frame can begin a fresh attempt. Callback failures retain their original
typed cause and existing scene-stop policy. Registering against another scene
or after retirement fails through the normal checked registration channel.

## Extending layouts and controls

Controls can derive from `Widget` and provide intrinsic measure only, or derive
from `SizedWidget` for configurable policies. Grid/Overlay can implement `Layout`:
`childAvailable` supplies measurement bounds, `measureChildren` aggregates owning
`MeasuredChild` values (including stable `NodeId` for matching placements after hiding siblings), and `arrangeChildren` returns exactly one owning local
rectangle per visible child. The interface pointer remains inside `tree.read`.
A custom solver must follow the frame/purity/error contract above. No Node
downcast or core Widget header change is required.
