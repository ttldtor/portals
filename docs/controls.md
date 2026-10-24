# Basic controls

[Русская версия](ru/controls.md)

`Label`, `Button` and `CheckBox` implement the existing `SizedWidget` extension
contract. They own validated `GraphemeText`, measure in terminal cells and emit
ordinary fill/text commands. Their intrinsic height is one row; layout policies
can allocate a larger rectangle. No control performs terminal I/O. Control bytes
in text remain data and render as the text subsystem's placeholders.

For a command shared by several buttons or explicit key bindings, see
[shared actions](actions.md).

```cpp
const auto run =
  tree.emplace<portals::Button>(parent, std::move(caption));
// Check each expected result in application code.
auto action = portals::bindButton(router, *run, [&] {
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

## Form fields

`ui::formField(...)` groups an editor with a label, help and a validation
message. See [Form fields](form-fields.md) for the complete settings form,
explicit checks, editing modes and custom editors.

## Bounded log output

`ui::logView(...)` displays recent plain-text records in a bounded, scene-owned
journal. It wraps by grapheme, retains the newest records within explicit line
and byte limits, and keeps the reading position stable when new records arrive.
See [LogView](log-view.md) for complete C++ and loaded-resource programs, limits,
append and clear behavior, and follow-tail navigation.

## Loading indicators {#spinner-control}

`ui::spinner()` creates an animated, scene-owned `Spinner`. Presets, custom
frames, an optional label, direction and reduced motion can be set with recipe
methods or pipe modifiers. `ui::Ref<Spinner>` exposes `start()`, `stop()` and
`label(...)`; callers do not own timers. See the complete program, resource
syntax, styling and lifetime rules in [Loading indicators](loading-indicators.md).

## Protected password input {#password-control}

`Password` is a separate `SizedWidget` for a protected single-line value. Include
`<portals/ui_password.hpp>` for the managed recipe and `ui::PasswordRef`, an alias
of `ui::Ref<Password>`. The scene owns the field, model and binding; the Ref observes
them and becomes usable after the complete recipe has mounted. See
[managed ownership](managed-controls.md).

This complete program reports whether a value was supplied. F10 exits. The
callback receives a `SecretView` only for its synchronous invocation; it does not
copy the password into the label.

```cpp
#include <portals/ui_password.hpp>

int main() {
  using namespace portals;

  ui::PasswordRef password;
  ui::Ref<Button> check;
  ui::Ref<Label> status;

  return ui::scene(ui::vbox(
    ui::password(password).placeholder("Password"),
    ui::button(check, "Check"),
    ui::label(status, "Enter a password")))
    | ui::onClick(check, [password, status] {
        return password.withPlaintext([status](SecretView value) {
          status.text() = value.empty() ? "Empty" : "Supplied";
        });
      })
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

The checked Ref operation collects failures in the scene once. Its returned
result passes through the click adapter to `runApp` without duplicate reporting. Application code can instead
inspect its `expected<void, ManagedError>`. `ManagedError::password` retains the
typed `PasswordFailure`, including independent protected-access, callback and
cleanup causes, without formatting secret bytes. Use the view only with a
synchronous consumer. Retaining its pointers or making plaintext copies makes
their lifetime and erasure the application's responsibility.

`password.value()` returns a checked owning `PasswordValue` identity for the
current protected draft. `password.value(value)` applies a protected candidate
under the field's editing policy. Both run on the UI owner between frames.
`PasswordValue::create(bytes, options)` is checked; a default-constructed value
is unavailable, whereas checked creation can produce a valid empty value. Copies
share immutable protected bytes and compare identities, without comparing or
decrypting their contents. Snapshots can retain old encrypted values after a
replacement until their last owner releases them. Plaintext borrows remain
restricted to the creating thread.

There is no password text/draft/caption getter, formatter, Undo/Redo history,
outbound copy or cut. `.readOnly()` blocks user mutation while the explicit
protected setter remains available. `ui::password(ref, config)` accepts
`PasswordConfig`; its optional `initial` provider returns
`expected<PasswordValue, PasswordFailure>` once during mount. Its `validate`
predicate borrows a `SecretView` synchronously; `false` retains the protected
draft without publishing it. To share a committed
model, create a `ManagedState<PasswordValue>` in the receiving scene and pass it
to `ui::password(ref, state)`. An external state and an initial provider cannot
be combined.

### Editing, presentation and limits {#editor-presentation-limits}

The default `EditMode::IMMEDIATE` publishes each accepted, semantically valid
change. `.editMode(EditMode::ON_COMPLETION)` keeps a protected draft until Enter,
programmatic activation or focus loss. `EXPLICIT` commits on Enter or activation
and keeps the draft on focus loss. Invalid UTF-8, line separators and whole-value
limit failures preserve the previous draft. Semantic rejection retains the new
protected draft. An external revision cannot overwrite a dirty deferred draft;
confirmation reports `CONFLICT`, and Escape securely adopts the latest model.
Escape cannot undo an edit already published in immediate mode.

A successful `PasswordResult::commit` owns the exact
`PropertyUpdate<PasswordValue>` receipt. Rejected observer notifications do not
undo the commit; inspect the delivery counts separately. Result callback failure
also cannot undo it. Managed scenes recover the latest clean snapshot before
frames even when a notification was rejected.

The default display uses one ASCII `*` per extended grapheme and reveals its
count. `.noEcho()` and the leading pipe `| ui::noEcho()` hide value, placeholder
and selection glyphs, measure one cell and fix the caret at the origin. Passing
`false` restores the mask. Layout and paint use prepared numeric metadata and
public mask/placeholder text without decrypting a value. A placeholder is public
text, never a place to supply a secret.

`PasswordOptions::limits` and `securePolicy.maxBytes` both default to 4096 input
bytes; both bounds apply. Larger explicit bounds remain limited by
`MAX_SECRET_BYTES`, 1 MiB. Whole edits fail instead of truncating a value. The
`securePolicy` fields are `maxBytes`, `storage`, `lockPages`,
`excludeFromDumps` and `encryptWhileIdle`. Password requires native guarded
storage and idle encryption on Windows or Linux. It rejects `WIPE_ONLY`, disabled
encryption and unsupported guarantees, including the native policy on WASM;
there is no automatic downgrade. Page locking is optional; dump exclusion is an
optional Linux guarantee and is unsupported on Windows.

Paste is disabled by default. Explicitly set
`PasswordOptions::pastePolicy = PasswordPastePolicy::ENABLED` to admit routed
paste or an injected `Clipboard` service. Owned service payload bytes and scalar
encoding temporaries are wiped after use. The system clipboard, decoder input,
service internals and caller buffers remain outside protected storage.

Guard pages, wiping and idle encryption do not promise protection against a
debugger or memory inspection tool such as ArtMoney with access to the process.
Plaintext exists during an admitted borrow. Ordinary
[LineEdit](line-edit.md) `PASSWORD` display and `noEcho()` continue to use plaintext
models, getters and history; their masking is a presentation option.

### Resources and explicit ownership

A resource can declare `Password#password { text = "", placeHolder = "Password" }`
or omit `text`. Nonempty text literals and text-resource expressions are rejected,
including inherited settings. Supply secret values through typed C++ state,
a checked provider or a captured `ManagedState<PasswordValue>` in a registered factory. Resource settings
expose every `securePolicy` field and `pastePolicy`; they do not accept secret
document text as protected storage. See [scene resources](scene-loading-guide.md).

String-resource diagnostics now identify their source as the fixed public label
`<string resource>`, including parse and limit failures. They no longer retain the
whole input document in that diagnostic field. Code that displayed or matched
that payload must account for this compatibility change. File-resource
diagnostics continue to identify the file path. The caller's source buffer or file
is not erased by rejecting a literal.

For explicit low-level ownership, include `<portals/password.hpp>`, create
`Password` in the tree and pass a shared `Property<PasswordValue>` to
`bindPassword(tree, router, node, model, onResult, clipboard, validate)`. Keep the
returned `PasswordBinding` on UI; the tree, router and scheduler must outlive it
and its model. Call `binding.sync(routerFocus)` before frames after model/input
changes. Its checked `setValue`, `confirm` and `cancel` operations retain typed
failures. Admitted callbacks survive removal of the field or its ancestor and
destruction of the binding. The [managed API](managed-controls.md) owns these
registrations and performs synchronization for ordinary application code.

## Input and focus

A button invokes its callback for each unmodified `Action::ACTIVATE`, including
Enter mapped by `EventRouter`, unmodified `TextInputEvent` with scalar `U' '`, or
left mouse **press**. A release does not activate again. Motion, paste, other text
and modified gestures are ignored. Native key repeats mapped to separate activate
events are separate activations; this component does not infer physical key holds.
These basic controls do not maintain persistent pressed/hover state or pointer capture.

Hidden or disabled subtrees are excluded by the real router. Callbacks may remove
the current widget or its ancestor and may destroy their token. Existing router
and tree borrows keep the admitted callback and widget alive until return.
Exceptions become `CALLBACK_FAILED`; completed user effects are not rolled back.

`EventRouter` remains the sole focus owner. Before a frame, recover focus once and
copy that presentation snapshot into every basic control, including labels:

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
`renderFullFrame` before leaving `tree.frame`. Borrowed commands/text must not escape the
frame; the resulting owning `CellBuffer` may be submitted to `FrameWriter` after it.

## Run the gallery {#run-the-gallery}

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

## CheckBox with three states {#check-box-with-three-states}

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
are included above as complete source files.

For a ready-made three-state control, use `ui::threeState("Selection", CheckState::MIXED)`.
It returns a `Description<ThreeState>` and accepts `ui::Ref<ThreeState>` first.
`ThreeState` reuses CheckBox behavior with a fixed three-state cycle; both
`ThreeState` and `CheckBox` type selectors match it. Each instance owns its state
unless an existing shared managed state is supplied explicitly.

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

### Compose change handlers

Use `.threeState()` or `| ui::threeState()` to give an ordinary CheckBox the
three-state cycle. Its type and Ref stay unchanged. A checkbox bound to a boolean
model cannot represent mixed state: incompatible settings fail during preparation
instead of converting the model.

`ui::onChange(ref, callback)` adds a handler without managing binding tokens.
This complete application displays each newly committed state:

\include guide_check_change.cpp

The example builds as
`portals_docs_check_change` with tests enabled, or as an ordinary installed-package
application linked to `portals::core`.

Use a `bool` parameter for a boolean-backed checkbox, or `CheckState` for a
three-state-backed checkbox. An incompatible typed handler fails preparation.
An advanced handler can accept `const PropertyUpdate<bool>&` or
`const PropertyUpdate<CheckState>&` to inspect the owning commit receipt,
including rejected notifications. A generic value handler must support both
backing types.

The existing factory callback runs first, followed by added handlers in
registration order. Registrations made during dispatch start with the next
activation. Failed and unchanged updates do not invoke the added handlers;
external model writes do not invoke them either. Removing the control or closing
the scene stops the remaining handlers. A handler error follows the existing
typed diagnostics path and does not roll back the committed value.

`ManagedOptions::maxChangeHandlers` defaults to 256 retained handlers and
`maxChangeHandlersPerNode` to 64. Both limits must be positive; admission beyond
a limit returns `NODE_LIMIT`. Allocation failures return `NO_MEMORY` without
retaining a partial registration. These limits are independent of editor handlers.

## Frames and outer dimensions {#control-frames}

A frame paints selected one-cell edges around content. Add `.framed()` to an
ordinary sized recipe, or use `| ui::framed()`. The recipe keeps its original type,
Ref and handlers. This complete program closes through the button, F10 or Ctrl+C:

```cpp
#include <portals/ui_style.hpp>

int main() {
  using namespace portals;

  ui::Ref<Button> exit;
  auto scene = ui::scene(ui::vbox(ui::button(exit, "Exit").framed("rounded", "Action").width(18).height(3)));

  return scene
    | ui::style("Border:focused { foreground: #62b4d7; }")
    | ui::onClick(exit, ui::quit)
    | ui::onKey("F10 | Ctrl+C", ui::quit)
    | ui::runApp();
}

```

`single` (also `plain`), `rounded`, `double`, `heavy` and `ascii` select built-in
patterns. The second argument supplies an owning top-edge title: `.framed("rounded", "Action")`,
or `| ui::framed("rounded", "Action")`. The same overload is available for ScrollArea.
Unknown names produce a typed `frame.pattern` error during preparation.
The terminal width policy is checked when glyphs are prepared; built-in symbols
that do not occupy one cell fall back to ASCII. A custom source must contain six
printable, single-grapheme, single-cell glyphs, each at most 64 UTF-8 bytes.

The ordinary recipe gains a passive `Border` parent. Width, height, flex,
visibility and enabled modifiers on the cold recipe address that outer frame,
regardless of whether they precede or follow `.framed()`. Existing custom setup
callbacks still receive the original widget. To observe or remove the wrapper,
declare `ui::Ref<Border> outer` and use `.framed(outer, "rounded")`.
Runtime tree operations on the original child remain local: removing it leaves
the frame; removing the outer frame retires the entire subtree. A child Ref does
not implicitly retarget its parent.

For example, `.height(6)` requests six **outer terminal rows**. With top and bottom
edges the content clip has at most four rows. It does not mean six child controls;
children retain their own layout policies and may have different extents. Parents
can constrain the allocation. Titles do not increase intrinsic width.

For advanced construction use `FrameOptions`. `sides` controls each edge and its
inset; `title` is an owning UTF-8 top-edge title; `titleAlignment` selects START,
CENTER or END. The default title limit is 4096 bytes. `customGlyphs` supplies
corners and horizontal/vertical edges. `border.maxPaintCells` defaults to 16384;
title work includes the complete submitted title's bytes and graphemes, including
a clipped tail. Invalid preparation submits no frame commands. Painting retains
no external text references and performs no allocation itself.

`ScrollArea.framed(...)` configures its own frame, without a second Border or a
double inset. Add `.scrollOnFrame()` or `| ui::scrollOnFrame()` to place its bars
on that frame; the modifier preserves the existing bar policies and does not
enable a missing frame. For `ScrollPlacement::ON_FRAME`, a potentially visible horizontal
track requires the bottom side, and a vertical track requires the right side.
Track cells take precedence over frame and title cells. The explicit outer-Ref
overload is unavailable for ScrollArea because it creates no wrapper node.

Colors, boldness and focused/disabled appearances use the existing stylesheet
layers. The wrapper's type is `Border`; the child's type, ID and classes remain
unchanged. Focused descendants select the frame's focused appearance, while
ancestor disabled state takes precedence. Geometry and glyph patterns remain
construction settings; they are not new PSS properties. Legacy `BorderOptions`,
`ScrollAreaOptions`, `ui::border` and `ui::frame` remain available.

See [loaded frame properties](scene-loading-guide.md#loaded-frames) for the same
construction settings in scene resources.


## Decorate and style a frame title {#frame-title-decoration}

Add `.titleDecoration("square")` after `.framed("rounded", "Sections")`, or use
`| ui::titleDecoration("square")`. Decorations change the top-edge opening
without adding a row or changing the content inset:

| Name | Opening |
| --- | --- |
| `none` (default) | `─Sections─` |
| `spaced` | `─ Sections ─` |
| `square` | `─[ Sections ]─` |
| `angle` | `─< Sections >─` |
| `round` | `─( Sections )─` |

These fragments show only the title opening. The selected frame pattern still
supplies the rest of the perimeter. The default preserves existing title output.
`TitleDecoration::SQUARE` is the corresponding enum form. For custom ends use
`.titleDecoration(CustomTitleDecoration{.prefix = "{ ", .suffix = " }"})`.
The frame owns both strings; each end is limited to 64 UTF-8 bytes and 16 cells.
Every end grapheme must be printable and occupy one terminal cell under the
receiving width policy. Invalid names or custom ends produce a typed preparation
error. An empty title or missing top side leaves the existing frame unchanged.
If both ends and at least one complete title grapheme do not fit, the complete
title opening is omitted. Otherwise only the title is clipped, at grapheme
boundaries. Scroll tracks retain priority over frame cells.

The [complete scroll application](scroll-controls.md) styles its line, title,
and ends independently through the same stylesheet cascade:

```css
ScrollArea {
  frame-line-foreground: #7d8797;
  frame-title-foreground: #62b4d7;
  frame-ends-foreground: #62b4d7;
  frame-title-bold: true;
}
```

Each prefix also accepts `background`, `bold`, `italic`, and `underline`.
Use the `Border` selector for an ordinary framed recipe. Missing part attributes
inherit the resolved common style of that state; a title-only override does not
replace the line or background. Focused and disabled rules use the existing
state selectors and style layers. These are paint attributes; decorations and
glyph patterns remain construction settings.
