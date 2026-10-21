# Dialogs and overlay hosts {#dialogs}

[Русская версия](ru/dialogs.md)

An overlay host keeps one main body and registered dialog layers in the same
retained tree. Opening changes visibility, input admission and focus without a
second event loop or scene reconstruction. Closed dialogs stay mounted and retain
their child controls' state.

## A complete modal form

\include guide_dialog.cpp

[Complete source](examples/guide_dialog.cpp). Link `portals::core`; see
[package setup](package.md). The [standalone guide project](examples.md) builds
`guide_dialog`. The root build with `PORTALS_BUILD_TESTS=ON` builds
`portals_docs_dialog`. Installed consumers compile this exact maintained source.
It opens a native terminal; it has no CLI or automated replay mode.

The editable field and checkbox keep scene-owned state; this example does not
connect to a feed or save settings. Tab stays within the active modal layer.
Escape first reaches its children; an unhandled cancellation dismisses the top
modal. A child may consume Escape. The background F10 handler and main Exit button
are unavailable while the modal is open. Use Close or an unhandled Escape first;
F10 exits from the main form through the standard runner and terminal cleanup.

You can declare the ordinary body and its initially closed dialogs together with
`ui::scene(body, layers...)`. The scene creates one overlay host for those layers.
An ordinary `ui::scene(body)` keeps its existing composition; it does not add a
host. Opening and closing reuse the mounted host and dialog controls.

Use `ui::overlayHost(body, layers...)` when you need explicit host options or a
host reference. Passing that host as the scene body keeps the explicit host;
additional scene layers use it as well. Dialogs belong in layer arguments rather
than inside a `ui::vbox` or another ordinary layout.

The same form accepts a lazy body factory taking `ManagedScene&`, optionally
followed by `Application&`. For advanced runtime settings, put `SceneOptions`
first: `ui::scene(options, body, layers...)`. The original single-body options
overloads remain available. The explicit-origin form
`ui::scene(options, origin, body, layers...)` retains a supplied construction
site for diagnostics; convenience layer adapters otherwise record their adapter site.

You can pass an existing `ui::resource(...)` recipe in a layer argument when its
registered factory returns a dialog. The erased root is checked before reference
publication; an ordinary control in that position fails with `INVALID_ARGUMENT`
and the attempted composition is retired. Factory side effects retain their
existing contract. An explicit host hidden inside an erased resource body is not
the typed root-host override and cannot be combined with extra scene layers.

`ui::overlayHost(body, layers...)` takes one ordinary body and dialog recipes.
`ui::dialog(reference, title, body, options)` creates one initially closed layer.
Titles use existing owning text/resource conversion. This C++ composition does
not register `Dialog` or `OverlayHost` as built-in scene-resource loader tags. Ref copies captured before
mount share a slot published only after complete successful mounting. Reuse a
closed dialog by opening it again; do not remount its controls for every opening.

## Checked commands and publication

`reference.open()` and `reference.dismiss()` return an owning
`ui::DialogCommandResult`. Inside admitted button/key/dismissal callbacks, the
short calls in the example participate in existing diagnostics and runner stop
behavior. Outside that scope, inspect the result: its boolean test reports
success, `error()` copies the owning failure, and `result()` returns
`std::expected<void, ManagedError>`. There is no exception shortcut, hidden
retry queue, or new callback return type. `reference.opened()` is a checked
`std::expected<bool, ManagedError>` query rather than a retained state reference.

Use these UI-affine operations on the owning thread. A cold/unpublished or closed
owner returns `CLOSED`; a removed identity returns `STALE_NODE`; a foreign session
returns `WRONG_SESSION`. Initial anchored opening before usable completed layout
is available returns `WOULD_BLOCK`, without scheduling a future opening. Call
again explicitly after normal preparation if the application still wants the
panel. Centered opening needs no previously published geometry; normal
preparation computes its placement.

Validation and allocation failures before publication leave the attempted
transition unpublished. After visibility/input/focus publication, a blur/gain or
dismissal callback can fail while the transition remains committed. Inspect
`opened()` instead of assuming every error means the panel stayed unchanged.
`ManagedError` retains its phase, node, primary cause and independent cleanup
information; the runner reports after attempting terminal restoration.

## Input, focus and retirement

Modal layers confine new input and focus to their subtree. Opening from an
admitted callback lets that callback finish; it does not replay the event at a new
target. Background pointer capture is cancelled before further outside delivery.
Tab and Shift-Tab use the existing focus order inside the top modal. A modal with
no eligible child still shields the body, with empty focus and working unhandled
Escape. Nested modal layers use the top layer; dismiss it before dismissing a
lower modal. A stale/removed opener is never reused as a focus-return identity.

`.onDismiss(callback)` receives `DialogDismissReason::PROGRAMMATIC` for an explicit
dismissal or `CANCEL` for managed cancellation. It accepts `void` or exactly
`std::expected<void, TreeError>`. Notification follows the committed transition;
a failure does not reopen the panel or replay notification on retry. A live
already-closed layer is a dismissal no-op. Reentrant opening/dismissal of the same
host during staging, focus/capture delivery, dismissal notification or teardown
returns `WOULD_BLOCK`. Do not destroy the owning scene/application from callbacks.

Hiding a layer ends activation. Disabling ends input/modality and returns focus
while retaining its disabled picture. Raw show/reenable does not reactivate the
managed session; use a new checked `open()`. Removal retires registrations and
capture. These retirement paths do not invoke user dismissal callbacks. Admitted
callbacks retain their captures through return, including self-removal.

A rejected model notification does not undo its committed model value or the
synchronous dialog operation; equal assignment does not retry rejected delivery.
Existing [property receipts](property.md) and [managed preparation](managed-controls.md)
remain in force. An application task deliberately calling a still-live Ref later
is a new command, not an automatically cancelled library notification.

## Nonmodal panels and anchors

Set `DialogOptions::mode` to `DialogMode::NON_MODAL` to leave the body interactive.
Opening does not steal keyboard focus. Pointer activation does not automatically
enter a keyboard focus scope; explicit focus or Tab can enter the panel. Dismissal
returns focus only if the panel still owns the relevant transition, preserving a
new body selection. Escape closes a nonmodal panel only when cancellation routes
to that layer; a background Escape is not a global panel-close shortcut.

These options are compiled in the installed consumer:

\snippet dialog.cpp dialog-options

Apply them with `ui::dialog(reference, title, body, helpOptions)`. The consuming
modifier `.anchor(opener, DialogPlacement::BELOW_START)` resolves a cold body Ref
at complete mount; `ABOVE_START` prefers the other side. The anchor must be in
this exact host's ordinary body subtree. Another host or any dialog-owned target
is rejected with `INVALID_ARGUMENT`, including targets in hidden dialog content.
A failed complete mount publishes no partial Ref slots and retires its subtree.

Initial anchored opening needs live usable published geometry; unavailable or
fully clipped geometry returns `WOULD_BLOCK`. For an already-open panel, a stale,
hidden or fully clipped anchor falls back to centered placement on the next
prepared pass. A disabled but geometrically live anchor remains usable. Placement
uses owning geometry from the existing bounded preparation, including enclosing
scroll areas. It introduces neither an inter-dialog anchor graph nor extra passes.

## Geometry and appearance

The host's intrinsic size comes only from its body. Each dialog has a full-host
structural root and an opaque bordered panel. It is not a `SizedWidget`; configure
its panel through these owning options, rather than applying `ui::width/height`:

| Option | Default and constraint |
| --- | --- |
| `mode` | `MODAL`; `NON_MODAL` keeps outside input available. |
| `placement` | `CENTER`; anchored preferences are `BELOW_START` and `ABOVE_START`. |
| `maximumPanelSize` | 60 by 20 cells; both caps must be strictly positive. |
| `viewportMargin` | 1 cell, nonnegative; reduced when the viewport is tiny. |
| `anchorGap` | 1 cell, nonnegative. |
| `closeOnEscape` | `true`; children still receive cancellation first. |
| `frame` | Existing `BorderOptions` glyph and paint-budget policy. |

The panel shrinks to available space; tiny/zero viewports yield clipped or empty
geometry without negative extents. Centering rounds down. Anchored placement can
flip to the side with more available height and then clamps to the host. Paint
and hit testing share the existing tree's clipping and sibling order. There is
no shadow/backdrop engine, dragging window manager or separate style cascade;
use existing [style layers](style-layers.md) for supported presentation.

`OverlayHostOptions::maxLayers` defaults to 32 and must be positive. It counts all
registered layers, including hidden ones. The options-first overload is
`ui::overlayHost(hostOptions, body, layers...)`. Removing a layer retires its
registration; repeated open/close does not register another copy.

Closed and unchanged dialogs need no polling timer or permanent redraw. Scene
shutdown retires them with other managed resources. MessageBox results,
save/discard workflows and asynchronous dialog results are application
compositions, not additional promises of this foundation.

## Ready-made modal choices

Use `ui::messageBox`, `ui::confirmDialog` or `ui::unsavedChangesDialog` when a
modal needs one of the maintained button sets. The [complete source](examples/guide_message_box.cpp)
is included below; link `portals::core` as for the dialog guide. The standalone
guide project builds `guide_message_box`; the root build with
`PORTALS_BUILD_TESTS=ON` builds `portals_docs_message_box`.

\include guide_message_box.cpp

| Preset | Buttons | Default focus |
| --- | --- | --- |
| `messageBox` | `ACKNOWLEDGE` | `ACKNOWLEDGE` |
| `confirmDialog` | `CONFIRM`, `CANCEL` | Safe `CANCEL` |
| `unsavedChangesDialog` | `SAVE`, `DISCARD`, `CANCEL` | Safe `CANCEL` |

`MessageOptions::defaultChoice` can select another button in that preset.
`CONFIRM` and `DISCARD` require the explicit `allowDestructiveDefault` opt-in.
These helpers require a modal `DialogOptions` configuration with Escape enabled;
use `ui::dialog` for custom layouts, button sets or non-modal panels. Captions
and title/body text are owning and may use the existing synchronous resolver.
Captions must be non-empty single-line text. `textLimits` defaults to
16,384 input bytes, 4,096 bytes per cluster, 16,384 graphemes and 32,768 cells;
`maxBodyLines` defaults to 4,096 wrapped lines.

`MessageBox` is a UI-thread-affine observer. Its copies do not own the scene,
widget or callbacks; a cold handle reports `CLOSED`, a retired identity reports
`STALE_NODE`, and wrong-thread/session access is checked. Mount the associated
dialog, then call `open(callback)` or `cancel()` through the ordinary declarative
handler chain. The action receives `const MessageResult&`; check it before
reading the choice. A value is a user/programmatic choice, while an error is a
technical managed failure retaining its full `ManagedError`. `CANCEL` is a
choice, never a technical failure. `SAVE` records a request only: the example
does not persist data or prove that a save completed.
The example displays successful action requests. For application-specific recovery,
inspect the complete managed failure in the callback.

`lastResult()` returns an owning immutable snapshot once a request completes;
before then it reports `WOULD_BLOCK`. Requests remain busy while callback
delivery or async retirement is pending. Callback delivery uses the existing
UI scheduler and runner. `receive()` integrates with an existing `UiTask` and
`UiAsyncContext`; it does not add an event loop, custom awaiter or coroutine
runtime. Async delivery may be deferred when the bounded completion queue is
full; the existing after-dispatch retry handles available capacity. The scene
limits outstanding/reserved/retired MessageBox requests to 32 by default
(`ManagedOptions::maxMessageRequests`). Queue exhaustion and teardown can
still reject or cancel delivery, so an asynchronous result is not a durable
business record.
