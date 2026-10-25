# Running an application {#application}

[Русская версия](ru/application.md)

The [scene](glossary.md#glossary-scene) facade in [`<portals/ui.hpp>`](../include/portals/ui.hpp)
lets you create controls, attach actions and run a native terminal application.
**Apply** changes the label. **Exit** or F10 closes the application.

\include{lineno} guide_application.cpp

This complete program links to `portals::core`;
see [package setup](package.md). The scene mounts its description when run,
opens the terminal, services input and redraws changed controls. `ui::runApp()`
attempts terminal restoration and scene cleanup before returning 0 on success
or 1 on failure. Its default error report goes to stderr after cleanup.

`ui::Ref<Label>` observes the label; the scene owns the control. The action
captures the reference by value, and operations on the control run on the UI thread. The text
assignment uses the normal invalidation path; no explicit repaint is needed.
See [text properties](text-property.md) for checked operations and [events](input.md)
for input handling. The [coroutine guide](ui-coroutines.md) explains actions that
wait without blocking input. [Diagnostics](diagnostics.md) covers custom error reporting.

## Run configuration

### Include scene construction in the error boundary

Pass a factory to `ui::runApp` when creating references, application data or the
scene can fail. The factory returns an owning scene; its local references are
captured by value in actions. This complete program needs no outer `try`/`catch`:

\include{lineno} guide_application_factory.cpp

The equivalent function
form is `ui::runApp(factory)`. The factory runs once inside the checked boundary;
it may also return `std::expected<ui::Scene, ManagedError>`. Existing factories
returning a description remain supported. Allocation failures become `NO_MEMORY`,
and other factory exceptions become `CALLBACK_FAILED`.

Return a new cold scene on the current UI thread. An already prepared, unrun
scene is also accepted and reuses its preparation. Completed, closed and
moved-from results are rejected before terminal access; a retained prior failure
is preserved, otherwise the rejection is `CLOSED`. Existing move restrictions
for active or foreign-thread scenes still apply.

If a factory abandons a local scene whose shutdown fails, the runner retains
a cleanup cause in the existing error envelope. A thrown or returned primary error keeps its cleanup cause;
additional causes beyond the single cleanup slot are counted in the final
report. An otherwise successful factory with a failed abandoned scene does not
open the terminal.

A scene returned by the factory keeps its own policies and error reporter.
An explicit `RunAppOptions::report` overrides that reporter and also receives
failures before a scene exists. The runner first takes an owning copy of this
explicit reporter; if that copy fails, stderr receives the failure and the factory
does not run. Cleanup precedes the final runtime report. Arguments and lambda
captures constructed before entering `runApp` remain outside its boundary.

For command-line applications, use
`factory | ui::runApp(argc, argv, ui::lyra::cli(lyraCli))` with
[`<portals/ui_lyra.hpp>`](../include/portals/ui_lyra.hpp). Parsing happens first:
help returns 0 and invalid arguments return 2 without invoking the factory or
opening the terminal. A construction or runtime failure returns 1. Keep option
storage alive until this synchronous call returns; construct controls from the
parsed options inside the factory. See [command-line parsing](cli.md).

### Application policies

`ui::applicationOptions(...)` supplies `ApplicationOptions` before application
creation. You can configure cadence, input/output bounds and native screen policy there.
The native default uses the alternate screen and restores the caller's screen
on close. An explicit `NativeScreenMode::MAIN_SCREEN` setting leaves the final
frame on the main screen. Window resizing remains under the terminal host's control.

`ui::scene(factory)` is available when building controls requires scene-owned state or
access to `Application`; `ui::configure(...)` runs work after mounting.
`scene.onStop(...)` must be registered before preparation. The callback must
cancel and join external producers and release services borrowing the application
before its managed nodes retire.
Data borrowed by control callbacks must remain alive until those controls retire.
An installed callback can retain a shared owner, or the application can keep an
owner outside the scene. A capture in the scene recipe alone is insufficient:
successful preparation releases the recipe and pending configuration callbacks.
Capturing an owner only in `onStop` is also insufficient because that callback
finishes before the managed nodes are destroyed. Avoid owning cycles between
presentation data and external producers.
Callbacks must not destroy or reenter the scene or runner.

## Custom scenes and transports

Direct access to [`Application`](../include/portals/application.hpp) is available when you need
`ApplicationScene::input`, `prepare`, custom widget preparation, or your own
`ApplicationTerminal`. The low-level runner returns `expected<ApplicationStats,
ApplicationError>`; the caller checks it and reports errors after terminal cleanup.
The facade above already owns this runtime for ordinary applications.

<details>
<summary>Runner reference: preparation, bounds, ownership and native transport</summary>

**Frame preparation**

`prepare(application, size)` runs only when a dirty frame can be submitted. It
runs after the cycle's single scheduler dispatch, outside `WidgetTree::frame`.
Synchronize bindings and focus, take **one [model pin](glossary.md#glossary-model-pin) per model**
(an immutable `ModelFrame<T>` capturing data and availability), and prepare all sibling
views from that pin using their final layout extents. `VirtualTable` and `Chart`
retain their own copies of the pin and immutable prepared data. Dispatch returned
`ModelRequest` receipts outside rendering; no provider work starts implicitly.

For nested virtual views, opt into [layout-driven preparation](virtual-preparation.md).
The runner then calls `prepareViews(application, snapshot)` after a preliminary
layout, outside the tree frame. All siblings use the same owning geometry and
the pins retained by `prepare`; bounded layout retries never repin models.
The original explicit-size path remains the default.

The runner performs layout, traverses visible nodes, translates local paint
commands through the arranged surface geometry and clips, and consumes every
borrowed text command into an owning `CellBuffer` before leaving the frame.
Custom widgets therefore implement the same `measure/arrange/paint` contract as
built-in widgets. Painting must not do I/O, model mutation, scheduler dispatch,
or asynchronous work. Geometry-driven paint invalidation is consumed by the frame
which already painted it; idle does not schedule periodic repaint.

**Service bounds and errors**

One owning terminal batch/raw suffix and one rejected CONTROL payload survive
between cycles. Native Windows events are not decoded a second time. POSIX bytes
retain arrival timestamps across suffix retries; decoder deadlines shorten input
waits. Input capacity pressure pauses admission without dropping keys. Every cycle
has one bounded dispatch and at most one frame submission/flush. Dirty state stays
in the tree while output is pending. The next frame deadline is measured from
actual submission/output completion; late work never causes frame catch-up bursts.

EOF is a transport cutoff: the decoder finishes, admitted CONTROL is delivered,
and already submitted output is flushed before cleanup. It does not wait for a
future frame deadline, timer or DATA publication. An accepted edit may therefore
remain dirty and absent from the final output if EOF arrives before its next
frame; there is no shutdown exception to the frame cap. Cancellation
can stop earlier and does not promise to deliver remaining CONTROL.

`inputSlice` is a positive read deadline; `read(now)` would return an immediate
deadline without polling. A configured cadence still causes input/resize sampling
in idle, but no frame is rendered or emitted without invalidation. The loop uses
terminal reads as its wait; the cadence bounds how long worker publications can
wait before dispatch. It does not claim an OS-integrated scheduler wait primitive.

`outputSlice` defaults to two seconds. An unfinished retryable transaction stays
owned by `FrameWriter`; any transport error, including Windows
`OUTPUT_UNCERTAIN`, ends the run. It never reopens or replays uncertain bytes.
Native close still requires the console host/driver to make progress; the runner
does not promise a hard shutdown deadline for a stalled host.

`ApplicationError` preserves the failing phase and original component error.
A separate `restoration` field preserves close errors alongside an earlier
failure. Hook/allocation/observer failures are explicit. Stop first closes the
scheduler, then invokes `scene.stop` to cancel/join scene-owned producers, then
closes the terminal. This also happens after errors and pre-requested cancellation.
No diagnostics, exceptions or metrics are printed while the terminal is active.

**Clocks, replay and measurements**

The native overload requires the default steady clock. The custom
`ApplicationTerminal` overload borrows an exclusive terminal through `close` and
supports an injected monotonic clock. Its millisecond epoch must map directly to
the terminal's `TerminalDeadline`; mixing a fake epoch with native I/O is rejected.
Custom transports must honor the configured batch bounds and own their results.
Both overloads are single-use.

An optional borrowed `ApplicationObserver` receives preparation/render, encode,
flush, stop and close durations. With no callback, these measurements perform no
clock reads or sample allocation. The observer must not reenter or perform I/O;
its exception fails the run. `CONTROL_LATENCY` samples only the runner's own
input/resize channel, read at callback entry, in logical clock time. It is not a
distribution for every scheduler channel. Returned `ApplicationStats` owns totals;
benchmarks own and bound their sample storage.

Diagnostics remain disabled unless explicitly configured on the scheduler.
Input correlation enters at admission, survives CONTROL delivery and frame
preparation, and reaches the actual writer submission/output. Export snapshots
only after `run` restores the terminal.
EOF, cancellation, and output failure are observable outcomes, not evidence that
a real terminal visually displayed a frame.

**Managed native transport**

The native `Application::run(scene)` overload uses `openNativeApplicationTerminal`.
By default, `ApplicationOptions::nativeScreen` is `NativeScreenMode::ALTERNATE_SCREEN`:
the adapter enters a separate screen with VT `1049` before rendering and returns
to the caller's main screen on close. The main screen's contents, cursor and
rendition belong to the terminal host and are preserved through this lifecycle.
The adapter also preserves cursor visibility: Windows saves/restores console
cursor information; POSIX uses xterm saved private mode `25`.

Set `nativeScreen = NativeScreenMode::MAIN_SCREEN` to paint the caller's screen
directly. This explicit opt-out leaves the final frame and cursor in that screen;
it does not erase the frame at exit. The factory accepts the same screen policy
as its final argument. Custom/replay transports own their screen policy and are
not wrapped automatically. `TerminalSession::open/close` retain their low-level
mode-only contract and add no screen switch.

Changing the terminal window size during the run keeps the **new** size after
exit. The terminal host may reflow or crop the main screen and clamp its saved
cursor to the new geometry. Portals does not restore an old window size, an old
pixel layout, or absolute cursor coordinates. This follows the host's
[Windows alternate-screen behavior](https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#alternate-screen-buffer)
and [xterm VT1049 protocol](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html).

`ApplicationOptions::nativeMouseReporting` defaults to true. On POSIX, setup
queries coordinate encoding modes 1005, 1006, 1015, and 1016 with DECRQM, saves
tracking modes 9, 1000, 1001, 1002, and 1003 with XTSAVE, then enables 1000 button
tracking and 1006 SGR cell coordinates. Unsupported or permanently incompatible
encodings fail before their mutation. No initial mode is assumed disabled.
The reader preserves non-query startup bytes, including input between fragmented
replies, for its first read. Retained input is capped by `max_input_bytes`; one
constant-sized incomplete query suffix can coexist with it. Missing replies,
excess input, or transport errors fail setup explicitly. Failed setup consumes
already read input and returns no replay transport. Matching query syntax cannot
authenticate terminal replies against byte-identical injected input.

Cleanup restores encodings from their queried values and tracking with XTRESTORE.
This split avoids a verified xterm 390 defect: its encoding restoration casts the
saved numeric value to an 8-bit Boolean, while tracking restoration retains the
integer value. See the versioned primary sources
[ptyx.h macros](https://github.com/ThomasDickey/xterm-snapshots/blob/xterm-390/ptyx.h)
and [charproc.c saved modes](https://github.com/ThomasDickey/xterm-snapshots/blob/xterm-390/charproc.c).
Windows uses native console mouse events and sends no mouse setup bytes.
Mounted [scroll controls](scroll-controls.md) request a session button-motion
profile before native open. POSIX additionally queries 1002 with DECRQM and
requires set, reset or permanently-set status; unknown/permanently-reset or
missing replies fail setup explicitly. It then enables 1002 with 1006 instead of the
original 1000 profile, using the same saved tracking restoration. The default
scene query is false; explicit mouse opt-out still takes precedence. Controls
mounted later cannot change the opened profile. Custom terminal hosts explicitly
declare motion/release guarantees through `pointerCaptureCapabilities()`; its
default is empty. External focus/capability loss requires explicit host
cancellation because no unsignalled OS focus-loss event is inferred.
Mouse opt-out skips only mouse protocol I/O; it still manages the selected screen.

Close uses a fresh `modeTimeout` budget independent of application cancellation.
It first drains retryable output. A failed or uncertain write remains an error;
cleanup nevertheless attempts to return to the main screen. On Windows, it first
cancels and joins the old writer, then synchronizes with the console host after an
uncertain write, before starting the final restoration budget. Cancellation can
finish the client thread while accepted bytes still occupy the host. The
distinct restoration batch begins with CAN to cancel an incomplete VT sequence
and never replays uncertain frame bytes. A separate confirmed
DECSC capture protects the caller's current cursor/rendition even when a partial
entry never switches buffers: xterm's `1049` reset can restore its saved cursor
while already on the main screen. Unconfirmed capture uses CAN only; failed
startup suffixes are discarded rather than completed during rollback. Screen
state is armed for rollback before entry is submitted, so partial entry still
attempts an exit. Ordinary exit uses `1049` restoration without an extra DECRC. Cleanup capacity is
checked before changing the managed modes. Explicit close reports output and OS
restoration failures; repeated close adds no output. Destruction is best effort.

OS-mode restoration and owned-handle cleanup are always attempted, even when VT
restoration fails.
An expired output budget, broken transport, paused host, or abrupt process death
can leave the alternate screen active: no exact restoration is promised then.
Windows cancellation/join and native mode restoration still require host/driver
progress and have no hard shutdown deadline. Check the result of `run` and
`ApplicationError::restoration`; restored OS modes alone do not prove screen restoration.

This policy requires compatible VT1049 support and starts on the caller's main
screen. Nested alternate-screen owners, concurrent terminal output, and external
mode mutations are unsupported. POSIX XTSAVE overwrites one-level tracking and
cursor-visibility caches: current modes are restored, but previous saved caches
are not preserved. Confirmed DECSC capture also overwrites the saved cursor
cache; its previous contents cannot be preserved. Unsupported emulators do not acquire an exact VT restoration
guarantee; use the explicit opt-outs for an external terminal owner.

Setup and restoration bytes bypass FrameWriter and frame byte counters. A custom
transport decorator sees frame submissions and flushes, while factory setup and
close perform separate protocol I/O. Include them in lifecycle timing, not
rendered-frame throughput.

</details>


## Background work and shutdown {#application-background}

Include `<portals/background.hpp>` to use the optional bounded worker service.
`Application::background()` returns the borrowed service for the current session.
A custom scene requests it by overriding `requiresBackgroundExecutor()`. Ordinary
scenes start no worker threads. Synchronous `run` creates a pool only for a scene
that requests one and has no supplied `ApplicationOptions::backgroundExecutor`.
The defaults are two workers and sixteen pending jobs. Running jobs and prepared
results waiting for publication occupy worker reservations, outside that pending
count. These are count bounds; providers must also bound input and result bytes.

Native `Application::run(scene)` prepares the pool before opening its terminal.
With `run(scene, terminal)`, the caller has already provided the transport.
Hosted `start`/`step` requires a borrowed executor for an opting-in scene and
returns `ApplicationFailure::BACKGROUND_FAILED` with
`ApplicationError::background == BackgroundError::UNAVAILABLE` if none is supplied.
The host keeps its executor alive through session cleanup and schedules steps
while work can complete. The application never stops or joins that borrowed pool.

Each existing application cycle dispatches the scheduler and calls the service's
nonblocking `service()`. Worker readiness wakes the scheduler condition; it does
not interrupt a native terminal read. `inputSlice` therefore still bounds the
configured read deadline, subject to the transport's own guarantees. A foreign
event loop must arrange its next application step.

Shutdown first stops admission and requests cooperative cancellation of owned
work. It retires UI callbacks and attempts terminal restoration before explicitly
joining owned workers. This final synchronous join may wait for user work that
ignores cancellation. Stop callbacks execute synchronously and must be short.
`BackgroundPool` destruction also joins as a safety net: never put an owning pool
in a widget, scene callback or coroutine frame. A scene owns cancellation tickets
for its jobs, not the worker pool. An earlier application failure and a separate
terminal restoration failure remain available alongside a typed background cause.

`BackgroundLatest` retains at most one unadmitted intent per source and cancels
the preceding ticket when replaced. Call its `service()` from the source's
existing UI service stage to retry after a capacity change. No self-post loop or
additional scheduler is created. Immutable result publication retries retain the
same prepared value; they do not repeat the worker computation. Input, result
and callable destruction must be safe on a worker or the owning host thread.
