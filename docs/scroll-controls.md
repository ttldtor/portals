# Scroll ordinary content {#scroll-controls}

[Русская версия](ru/scroll-controls.md)

Wrap one ordinary content root in `ui::scrollArea`; use `ui::vbox`, `ui::hbox`
or `ui::grid` to group several controls. The scene creates its controller,
clips and positions the content, prepares layout, and reveals newly focused
controls. This complete application has eight buttons inside a five-row area.
Use the wheel, arrows, Page Up/Down, Home/End, or drag a thumb. Exit and F10 close
through the standard runner and restore the terminal.

\include guide_scroll_controls.cpp

[Complete source](examples/guide_scroll_controls.cpp).
Link your application to `portals::core`; see [package setup](package.md).
The [guide examples](examples.md) describe the standalone CMake build. Its
`guide_scroll_controls` executable opens a native terminal.

## Choose content and bar placement

Ordinary content remains retained in memory, including off-viewport controls;
it is measured and laid out as an ordinary subtree. For large datasets use
[virtual views](virtual-preparation.md), which own their own scrolling state.
Embedded bars add no Tab stops. `ScrollAreaOptions::focusable` controls the area's
own stop; keyboard focus on a descendant remains on that descendant.

`ScrollAreaOptions` controls bar presence, steps, placement and an optional frame.
Each axis supports `AUTO` (show on overflow), `ALWAYS`, and `NEVER`.
`NEVER` hides the bar while wheel, key and checked programmatic scrolling remain
available. The defaults move three logical cells per wheel event, one per arrow,
and a page with one overlapping cell for Page Up/Down. Home/End select endpoints.

| Placement | Viewport and content |
| --- | --- |
| `RESERVED` (default) | A visible bar reserves a one-cell gutter. |
| `OVERLAY` | The nominal viewport and range stay unchanged; the bar obscures edge cells. Reveal guarantees nominal visibility. |
| `ON_FRAME` | The area's own `frame` supplies the right/bottom track lines, excluding corners, without a second content inset. |

For `ON_FRAME`, supply `options.frame = BorderOptions{}` and
`options.placement = ScrollPlacement::ON_FRAME`; a separate `ui::border` does
not supply that frame. Missing frame/invalid options return
`TreeError::INVALID_ARGUMENT`. `readOnly` disables user scrolling while keeping
focus reveal and checked programmatic commands available. Nested ordinary areas
use the same tree clipping and managed preparation; each owns its own content root.

## Share a controller and issue checked commands

The default area needs no application-held controller. Create an explicit
`ScrollController::create()` only when another control must share its values.
Pass the owning handle to `ui::scrollArea(child, options, controller)` and
`ui::scrollBar(controller, axis)`. One mounted area writes the metrics; the bar
projects that axis and can change its offset. A second area writing the same
controller is rejected. All mounted projections must belong to the same tree
session; a foreign session returns `TreeError::WRONG_SESSION`.

A standalone `ui::scrollBar(reference, axis)` creates its own controller and owns
the metrics. Set them with `reference.setMetrics({extent, viewport})`.
An area-sharing bar cannot change metrics: that command returns
`TreeError::INVALID_ARGUMENT`. Change a mounted area or bar with
`reference.setOffset(axis, {integral, fraction})`; check its
`std::expected<void, ManagedError>` result. The lower-level
`setScrollOffset(tree, node, axis, offset)` and `setScrollMetrics` retain their
checked `TreeError` results. Commands must run on the owning UI thread between
frames. Removed/closed targets fail through the existing typed lifecycle checks.

The controller owns values and no tree, router or widget references.
`snapshot()` returns an owning observation on its creating UI thread, including
a revision and both axes. The handle and copied snapshots may outlive the scene;
they do not wake a retired tree. Removing a bar retires its binding/capture and
leaves other projections alive. Removing an area releases its metrics-writer
lease; it does not grant that role to another projection automatically.

Offsets preserve an exact `uint64_t` integral part plus a finite fraction in
`[0,1)`. The maximum is `max(extent - viewport, 0)`, or zero when viewport is zero;
the fraction is zero at the maximum. `scrollRange`, `scrollThumb` and
`scrollOffsetForThumb` preserve integer endpoints above 2^53 without a round trip
through `double`. Ordinary layout still uses signed 32-bit cell geometry;
unrepresentable layout/translation returns `TreeError::INVALID_GEOMETRY`.
Exact controller metrics do not virtualize an ordinary subtree.

## Native sessions and custom hosts

A mounted scroll registration requests held-button motion immediately before
native terminal open. The ordinary scene query remains false and preserves the
original profile: POSIX queries only coordinate modes 1005/1006/1015/1016 and
uses 1000+1006. Scroll opt-in additionally queries mode 1002 with DECRQM.
Statuses 1 (set), 2 (reset), and 3 (permanently set) admit button motion;
0 (unknown) and 4 (permanently reset) return `TerminalErrorKind::UNSUPPORTED`.
Missing or malformed replies consume the existing setup deadline and retain
the original typed failure. Successful output of 1002h alone does not prove
support. Cleanup restores saved tracking with XTRESTORE and queried encoding
modes through the existing restoration path. Windows uses native mouse records
with held-button identity. See [application lifecycle](application.md).

`nativeMouseReporting=false` takes precedence: no mouse query/mutation occurs,
and dragging is unavailable. Late-mounted controls use the profile already
opened; mounting does not switch terminal modes during a session.

Custom `ApplicationTerminal` and `ApplicationReadyTerminal` hosts must explicitly
return sufficient `pointerCaptureCapabilities()`: held-button motion, matching
LEFT release (or explicit cancellation), and reliable held-button identity.
Their default declaration has no drag capability; wheel, keys and track clicks
remain available. Hosts must call `EventRouter::cancelPointerCapture()` on lost
sequences or external focus/capability loss. The input model cannot detect an
unsignalled OS focus loss. Resize, Escape, session stop and cleanup retire capture.
A drag continues outside the owner's rectangle while capture remains valid.

`ui::runApp()` collects checked mount/preparation/input errors and attempts cleanup
before returning its exit code. Native setup failures remain explicit errors;
capability becomes observable only after successful setup. The xterm
[DECRQM and button-event tracking specification](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html)
describes modes 1002 and 1006; a syntactically valid reply cannot authenticate a host.
