# Scroll ordinary content {#scroll-controls}

[Русская версия](ru/scroll-controls.md)

Wrap one ordinary content root in `ui::scrollArea`; use `ui::vbox`, `ui::hbox`
or `ui::grid` to group several controls. The scene creates its controller,
clips and positions the content, prepares layout, and reveals newly focused
controls. This complete application has eight buttons inside a rounded frame titled Sections. The frame is 28 columns
wide and six rows high, leaving at most four content rows; the scrollbar uses
the frame edge.
Use the wheel, arrows, Page Up/Down, Home/End, or drag a thumb. Exit and F10 close
through the standard runner and restore the terminal.

[Complete source](examples/guide_scroll_controls.cpp):

\include guide_scroll_controls.cpp

Link your application to `portals::core`; see [package setup](package.md).
The [guide examples](examples.md) describe the standalone CMake build. Its
`guide_scroll_controls` executable opens a native terminal.

## Compare on-frame scrollbar skins

This keyboard-only gallery places the same eight-label content root, 40 cells
wide, in four 30-by-6 outer areas arranged 2 by 2. Each area uses `ON_FRAME`,
with tracks on its right and bottom frame edges, and one of four skins: legacy,
frame, minimal or ASCII. The frame inset leaves a 28-by-4 content viewport
before any other layout gutters. The ordinary eight-row content is not
virtualized. Tab focuses an area, arrow keys scroll it, and F10 quits.

Current limitation: with the default Nord part colors, a focused `legacy`,
`frame` or `minimal` full-block thumb can blend into the content background
while the track is highlighted.
The recording shows this behavior; focus does not change the scrolling range.

![Native terminal recording comparing on-frame scrollbar skins](images/scrollbar-gallery.gif)
[Static PNG frame](images/scrollbar-gallery.png)

Build `guide_scrollbar_gallery` with the [standalone guide project](examples.md).
The [complete source](examples/guide_scrollbar_gallery.cpp) is included below.

\include guide_scrollbar_gallery.cpp

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

For `ON_FRAME`, use `.framed("rounded", "Sections").scrollOnFrame()`; the pipe
form is `| ui::framed("rounded", "Sections") | ui::scrollOnFrame()`. Advanced
options remain available: `options.frame = BorderOptions{}` and
`options.placement = ScrollPlacement::ON_FRAME`. A separate `ui::border` does
not supply the area’s own frame. Missing frame/invalid options return
`TreeError::INVALID_ARGUMENT`. `readOnly` disables user scrolling while keeping
focus reveal and checked programmatic commands available. Nested ordinary areas
use the same tree clipping and managed preparation; each owns its own content root.

## Compose nested forms {#nested-scroll-areas}

This complete form uses editable connection fields and internal-state checkboxes.
Each area owns an `ON_FRAME` frame; the inner form is six rows high and the outer
form is nine. Tab moves between controls, Space toggles a checkbox, and Exit or
F10 closes the standard runner. Values live in this scene and are not persisted.

\include guide_nested_scroll.cpp

Build `guide_nested_scroll`
with the [standalone guide project](examples.md), or `portals_docs_nested_scroll`
with `PORTALS_BUILD_TESTS=ON`. The installed-package consumer compiles this same
source; it does not run the interactive program as an automated test.

Wheel input starts at the hit descendant and follows ordinary ancestor routing.
If the inner area changes its offset, it handles the whole event, including a
partial move to its boundary; it does not forward an unused delta. A later event
at that boundary leaves its offset unchanged and can scroll the outer area.
A descendant handler that handles the event stops this propagation.

Manual wheel or checked `setOffset` scrolling keeps focus on the same control
and does not repeatedly reveal that unchanged focus. A newly focused control
re-arms reveal through the enclosing areas. `reference.ensureVisible()` requests
a checked one-shot reveal for the next prepared frame without changing focus;
check its `std::expected<void, ManagedError>` result. See [focus](focus.md).
`readOnly` disables an area's user scrolling, not its child controls or checked
commands and focus reveal; an ignored wheel event may still reach an ancestor.

Paint and hit testing use the same inherited content clips. The inner frame
moves with the outer content, while a `ui::border` outside an area stays fixed.
Tracks on the area's own frame exclude corners and add no second gutter; content
cannot receive input through clipped frame cells. Hiding an ancestor or disabling
an area cancels its drag capture; hiding preserves the canonical offset. Removing
an area retires its bindings and capture, so late motion/release cannot reuse the
removed target. Resize or content removal clamps offsets during visible preparation.

Both forms are ordinary retained subtrees: even off-viewport children participate
in ordinary layout, whose work grows with their count. For large logical datasets,
[virtual views](virtual-preparation.md) prepare bounded visible/overscan ranges and
own their own scroll state. Nesting ordinary areas does not add virtualization.

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

## Style scrollbar parts {#scrollbar-skins}

Use `.scrollbar("frame")` after `.scrollOnFrame()` to match the scrollbar track
to the prepared frame, including custom frame lines. An unframed control uses
single lines for this preset. The equivalent pipe is `| ui::scrollbar("frame")`.

[Complete C++ source](examples/guide_scrollbar_skin.cpp) and
[complete loaded-scene source](examples/guide_scrollbar_resource.cpp) show the
same form. Build `portals_docs_scrollbar_skin` or `portals_docs_scrollbar_resource`
with tests enabled; both run interactively through `runApp`.

\include guide_scrollbar_skin.cpp

| Preset | Track | Thumb |
| --- | --- | --- |
| `legacy` (default) | Existing shaded track | Existing block |
| `frame` | Prepared oriented frame line | Block |
| `minimal` | Blank | Block |
| `ascii` | Dot | Hash |

`ScrollbarSkin` supplies owning sparse horizontal and vertical glyph overrides,
optional decorative ends and a corner glyph. Each supplied glyph is validated
under the receiving width policy: one printable one-cell grapheme, at most
`maxGlyphBytes` UTF-8 bytes (default 64). Track and thumb cannot be empty; empty
ends suppress their decoration, and an omitted or empty corner retains the fill.
Invalid settings return typed errors before changing the prepared skin.

Ends are disabled by default. Enabling them paints in existing track cells;
the thumb has priority. This does not reserve cells, create arrow buttons or
change clicking, range, dragging, focus or offsets. `ON_FRAME` keeps the frame's
existing corners; the independent corner glyph applies only where the existing
geometry exposes a scrollbar intersection.

PSS prefixes `scrollbar-track-`, `scrollbar-thumb-`, `scrollbar-ends-` and
`scrollbar-corner-` accept `foreground`, `background`, `bold`, `italic` and
`underline`. They use the ordinary state, selector and layer rules and retain
explicit `false` overrides. Colors and text attributes are shared by both axes;
glyph overrides are axis-specific. Runtime, typed and compile-time sheets use
the same declarations. Existing common styles remain the fallback.

Loaded configuration uses `scrollbar { pattern=FRAME ends=true }`, with optional
`maxGlyphBytes`, `horizontal { track="-" thumb="#" start="<" end=">" }`,
`vertical { track="|" thumb="#" start="^" end="v" }` and `corner="+"`.
Dotted leaves such as `scrollbar.vertical.thumb` are equivalent. A skin never
enables a frame or chooses its placement implicitly. Low-level callers can use
`setScrollbarSkin(tree, node, skin)` for a checked atomic replacement.
