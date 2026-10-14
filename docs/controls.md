# Basic controls

`Label`, `Button` and `CheckBox` implement the existing `SizedWidget` extension
contract. They own validated `GraphemeText`, measure in terminal cells and emit
ordinary fill/text commands. Their intrinsic height is one row; layout policies
can allocate a larger rectangle. No control performs terminal I/O. Control bytes
in text remain data and render as the text subsystem's placeholders.

```cpp
const auto run = tree.emplace<portals::Button>(parent, std::move(caption));
// Check each expected result in application code.
auto action = portals::bindButton(router, *run, [&] { start_job(); });
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
portals_gallery --check
```

The interactive gallery supports keyboard editing, focus traversal, checkbox and
button activation, mouse input and resize. F2 cycles themes; F10 exits.
`--check` runs a finite in-memory replay and exits without opening a terminal.
See [runnable examples](examples.md) for build paths, options and exit codes.

The basic-control tests cover activation, ancestor eligibility, Unicode ownership,
invalidation, callback removal, subscription cancellation and queue errors.
For the control contracts, see [focus and events](focus.md),
[properties](property.md), [layout](layout.md) and [rendering](render.md).
