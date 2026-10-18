# Basic controls

[Русская версия](ru/controls.md)

`Label`, `Button` and `CheckBox` implement the existing `SizedWidget` extension
contract. They own validated `GraphemeText`, measure in terminal cells and emit
ordinary fill/text commands. Their intrinsic height is one row; layout policies
can allocate a larger rectangle. No control performs terminal I/O. Control bytes
in text remain data and render as the text subsystem's placeholders.

```cpp
const auto run = tree.emplace<portals::Button>(parent, std::move(caption));
// Check each expected result in application code.
auto action = portals::bindButton(router, *run,
                                  [&] {
                                    start_job();
                                  });
```

The tree exclusively owns each widget. `Node` observes it. The application owns
`ActionBinding` and `CheckBoxBinding` tokens and keeps them for as long as input
should be connected. Text/style setters and binding operations run on the UI
owner between frames. `TreeError` propagates checked lifetime, session, affinity
and frame errors. Equal text/style/value synchronization is a no-op; changed text
or width policy invalidates layout, while style, focus and checked state invalidate
paint. `GraphemeText::from` reports text errors before widget construction.

## Input and focus

A button invokes its callback for each unmodified `Action::ACTIVATE`, including
Enter mapped by `EventRouter`, unmodified `TextInputEvent` with scalar `U' '`, or
left mouse **press**. A release does not activate again. Motion, paste, other text
and modified gestures are ignored. Native key repeats mapped to separate activate
events are separate activations; this component does not infer physical key holds.
There is no persistent pressed/hover state or pointer capture at this stage.

Hidden or disabled subtrees are excluded by the real router. Callbacks may remove
the current widget or its ancestor and may destroy their token. Existing router
and tree borrows keep the admitted callback and widget alive until return.
Exceptions become `CALLBACK_FAILED`; completed user effects are not rolled back.

`EventRouter` remains the sole focus owner. Before a frame, recover focus once and
copy that presentation snapshot into every control, including labels:

```cpp
auto focused = router.focused();
// Check focused, then check every sync result.
for (auto control : controls) {
  portals::syncControl(tree, control, *focused);
}
```

`syncControl` also reads eligibility through all ancestors, so a disabled parent
changes label and child appearance. The cached flags only choose paint style;
they never authorize input or redirect focus. Call this after input/model changes
and before consuming the frame request. Never call `router.focused()` from paint
or inside `tree.frame`: recovery mutates UI state.

## Checkbox binding policy

`bindCheckbox(tree, router, node, sharedModel, callback)` returns an owning
`CheckBoxBinding`. The shared model is a `Property<bool>`; its invalidation target
and the receiving checkbox must belong to the same tree. The binding owns one
ordinary property subscription and one router registration, and holds the model
alive. Tree/router/scheduler outlive the binding and model. Destroy or move the
binding on UI. Token captures may be reclaimed by the router's next collecting
operation, so model release need not be immediate at token destruction.

The model's latest committed value wins. A gesture reads its current snapshot and
calls `set(!value)` once, immediately synchronizes the checkbox, then invokes the
mandatory result callback. The callback receives the exact `CheckBoxResult`:

- An unexpected error means the model did not commit; the checkbox stays unchanged.
- A successful receipt describes the committed value/revision. Rejected observer
  notifications do **not** undo the value. Inspect `rejected`, `cancelled` and
  `notification_error` separately from the commit result. Do not retry the toggle.

External model updates and observer delivery never invoke the business callback.
An observer reads the latest snapshot instead of replaying an old event value.
Call `binding.sync()` before every frame after external sets or event dispatch:
it recovers even when all notifications were rejected by the bounded scheduler.
Equal `Property::set` does not retry notifications. Multiple checkboxes can share a
model and converge on the same value, including disabled views. Synchronize all
bindings before one coherent frame; no model is read or changed during paint.

If a property observer capture's destructor removes the receiving node during
publication, the committed receipt is still delivered to the admitted action
callback. No presentation update is needed for a removed node. Application
callbacks can perform another model update; synchronize again before rendering.

## Rendering and customization

`ControlStyle` owns normal/focused/disabled `CellStyle` values, not a theme engine.
Defaults use bold/underline for focus and gray/italic for disabled. Behavior does
not depend on style. Buttons add `[ ` and ` ]`; checkboxes add `[ ] ` or `[x] `.
Decorations are rebuilt under the text's complete width policy when text changes.

Controls submit **local** commands. The frame owner translates origins/areas and
clips by `NodeGeometry::surface`, intersects with `NodeGeometry::clip`, and calls
`renderFullFrame` before leaving `tree.frame`. The example's `SurfaceSink` shows
that adapter for both text and fills. Borrowed commands/text must not escape the
frame; the resulting owning `CellBuffer` may be submitted to `FrameWriter` after it.

## Run the gallery

Configure with `PORTALS_BUILD_EXAMPLES=ON` and build `portals_gallery`.
Run the executable from a native terminal:

```text
portals_gallery
portals_gallery --theme nord
portals_gallery --help
```

The interactive gallery supports keyboard editing, focus traversal, checkbox and
button activation, mouse input and resize. F2 cycles themes; F10 exits.
`--help` prints the native Lyra help and exits before opening a terminal.
Finite replay checks use the separate `portals_gallery_check` test harness.
See [runnable examples](examples.md) for build paths, options and exit codes.

The basic-control tests cover activation, ancestor eligibility, Unicode ownership,
invalidation, callback removal, subscription cancellation and queue errors.
For the control contracts, see [focus and events](focus.md),
[properties](property.md), [layout](layout.md) and [rendering](render.md).

## CheckBox with three states

`CheckState::UNCHECKED`, `CHECKED` and `MIXED` are explicit model values.
`checkState()` reads the last valid presentation; legacy `checked()` is true only
for `CHECKED`. The existing `Property<bool>`, `CheckBoxResult`, binding and
`ui::checkbox("Caption", bool)` keep their boolean behavior.

A complete native program uses the same scene owner, input router and runner:

\include check_state/main.cpp

The default `CheckBoxOptions::cycle` is `CheckCycle::TWO_STATE`: unchecked toggles
to checked, checked to unchecked, and mixed to checked. Opt in to `THREE_STATE`
for unchecked → checked → mixed → unchecked. Unmodified Enter/Space and left
mouse press use the same policy; release, motion, paste and modified gestures
are ignored. Hidden/disabled ancestors prevent activation. There is no separate
read-only checkbox mode. The `[-] ` mixed prefix has the same width as `[ ] `
and `[x] ` and remains distinct without color, including disabled, linear and
high-contrast output. Styling uses the existing normal/focused/disabled control
styles; this change adds no PSS pseudo-state.

Use `ManagedScene::state(CheckState::MIXED)` and `scene.bind(node, state, callback,
options)` for shared managed state, or `ui::checkbox(text, state, callback, style,
sizePolicy, options)` in a declaration. Both accept an optional typed
`CheckStateResult` callback. `.reference(ref)` and the managed-state Ref factory
publish the existing observing `Ref<CheckBox>` after successful mount. A callback
receives the exact `PropertyUpdate<CheckState>` set receipt: a successful commit
can contain rejected/cancelled notifications. Equal values do not publish again.
The callback cannot undo a commit; exceptions retain the existing typed failure
boundary. External model updates do not invoke the business callback.

For explicit ownership, `bindCheckbox(tree, router, node, sharedProperty,
callback, options)` returns a move-only `CheckStateBinding`. The model is shared;
the tree, router and scheduler remain borrowed and must outlive the token/model.
Destroy or move it on UI between frames. An admitted callback survives removal
of its node or destruction of its binding. Moving leaves the source empty;
`sync()` then returns `INVALID_ARGUMENT`. Call `sync()` before frames after
external changes: notification rejection does not roll back a model commit,
and an equal `set()` does not retry notification. Managed scenes perform this
recovery before frames. Old queued values cannot replace a newer snapshot.
Only actual presentation changes invalidate paint, and failed invalidation
retains the previous presentation. No state-switch allocation occurs.

Invalid `CheckCycle` or committed `CheckState` values report
`TreeError::INVALID_ARGUMENT`; an invalid raw Property value remains committed,
but cannot change the glyph or be silently toggled to a valid value. Registration,
affinity, stale identity, closed owner, active-frame and allocation failures use
the existing `TreeError`/`ManagedError` paths. A failed registration retains no
action/subscription. Argument construction can throw before entry as for the
boolean API.

### Aggregate and partial failure customization

`aggregateCheckState(span<const CheckState>)` is allocation-free and constexpr:
empty/all-unchecked input yields unchecked, all-checked yields checked, and other
valid combinations yield mixed. It returns `expected<CheckState, TreeError>`;
invalid enum input returns `INVALID_ARGUMENT`.

A group update is an application policy, not a transaction. The following complete
helper attempts every child in input order, preserves each actual error/commit
receipt, then reads authoritative snapshots to compute the aggregate:

\include check_state/group.hpp

Call `checkbox_example::updateSelection(children, CheckState::CHECKED)` or
`UNCHECKED` on UI. Inspect **every** `children` receipt and its `rejected`/`cancelled`
fields, report partial failures, then set the parent aggregate only if
`aggregate` succeeded. A failed child's old committed value participates in the
aggregate. Empty groups yield unchecked. Null owners are explicit child errors;
receipt storage failure occurs before writes. This helper retains no model or
callback; captures in application callbacks must outlive admitted input.
Do not wire parent and child business callbacks recursively: external Property
updates already bypass business callbacks. The helper and the native program
are [complete source files](../examples/check_state/main.cpp), with
[group policy source](../examples/check_state/group.hpp).

Use `ui::checkbox(text, CheckState::MIXED,
CheckBoxOptions{.cycle = CheckCycle::THREE_STATE})` for explicit input configuration.
The same options-first form accepts a shared `ManagedState<CheckState>`; an
optional receipt callback follows the options. Both forms copy the owning
options immediately. Existing callback-first/trailing-options calls remain
available, including an empty callback written as `{}`. The exact options-type
constraint keeps those braced calls unambiguous.

`GroupUpdate` exposes `committed`, `unchanged`, `failed`, `rejectedNotifications` and
`cancelledNotifications` separately. `reportGroupUpdate(result, output)` prints
these counters while the caller retains every receipt and the aggregate result.
