# Application runner {#application}

`portals::Application` owns one `Scheduler`, `WidgetTree`, and `EventRouter` on
its creating UI thread. Construct it before a scene and destroy the scene first.
The scene owns typed node observers, models, bindings and its own data source.
`Application::run` borrows that scene for one run; callbacks must never destroy
or reenter either owner.

```cpp
#include <portals/application.hpp>
#include <portals/controls.hpp>

int main() {
  auto created = portals::Application::create();
  if (!created) return 1;
  auto app = std::move(*created);
  auto text = portals::GraphemeText::from("Quit");
  if (!text) return 1;
  auto button = app->tree().emplace<portals::Button>(app->root(), std::move(*text));
  if (!button) return 1;
  auto quit = portals::bindButton(app->router(), *button, [&] { app->requestStop(); });
  if (!quit) return 1;
  portals::ApplicationScene scene;
  auto result = app->run(scene);
  // The terminal has been restored; report result.error() here in a full program.
  return result ? 0 : 1;
}
```

The runnable [gallery](examples.md) demonstrates input bindings, explicit quit,
control synchronization, two themes, and error reporting. There is no DSL or
second dispatcher. Extend `ApplicationScene::input` for global commands; it runs
inside the existing bounded CONTROL channel before normal `EventRouter` routing.
For ordinary control actions retain the existing `ActionBinding`/control bindings.

## Frame preparation

`prepare(application, size)` runs only when a dirty frame can be submitted. It
runs after the cycle's single scheduler dispatch, outside `WidgetTree::frame`.
Synchronize bindings and focus, take **one pin per model**, and prepare all sibling
views from that pin using their final layout extents. `VirtualTable` and `Chart`
retain their own copies of the pin and immutable prepared data. Dispatch returned
`ModelRequest` receipts outside rendering; no provider work starts implicitly.

The runner performs layout, traverses visible nodes, translates local paint
commands through the arranged surface geometry and clips, and consumes every
borrowed text command into an owning `CellBuffer` before leaving the frame.
Custom widgets therefore implement the same `measure/arrange/paint` contract as
built-in widgets. Painting must not do I/O, model mutation, scheduler dispatch,
or asynchronous work. Geometry-driven paint invalidation is consumed by the frame
which already painted it; idle does not schedule periodic repaint.

## Service bounds and errors

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
frame; there is no shutdown exception to the frame cap. Finite replay scenarios
that assert visible changes must leave sufficient time before EOF. Cancellation
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

## Clocks, replay and measurements

The native overload requires the default steady clock. The custom
`ApplicationTerminal` overload borrows an exclusive terminal through `close` and
supports an injected monotonic clock. Its millisecond epoch must map directly to
the terminal's `TerminalDeadline`; mixing a fake epoch with native I/O is rejected.
Custom transports must honor the configured batch bounds and own their results.
Both overloads are single-use. See `examples/replay/terminal.hpp` for an in-memory
transport; it is example code, not a second production terminal backend.

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
only after `run` restores the terminal, as the dashboard's `--trace` demonstrates.
EOF, cancellation, and output failure are observable outcomes, not evidence that
a real terminal visually displayed a frame.

## Native mouse lifecycle

The native `Application::run(scene)` overload opens the same managed transport as
`openNativeApplicationTerminal(handles, terminalOptions, modeTimeout, mouseReporting)`.
Use that factory when decorating a native transport with custom measurements,
then pass the returned `ApplicationTerminal` to the custom `run` overload. The
factory's timeout defaults to two seconds; the native runner uses `outputSlice`.
`ApplicationOptions::nativeMouseReporting` defaults to true. Set it to false, or
pass false to the factory's `mouseReporting` parameter, to skip mouse setup on
terminals that cannot answer the required queries. Custom transports own their policy.

On POSIX, setup queries coordinate encoding modes 1005, 1006, 1015, and 1016 with
DECRQM, saves tracking modes 9, 1000, 1001, 1002, and 1003 with XTSAVE, then enables
1000 button tracking and 1006 SGR cell coordinates. Unsupported or permanently
incompatible encodings fail before mutation. No initial mode is assumed disabled.
The reader preserves non-query startup bytes, including input between fragmented
replies, for its first read. Retained input is capped by `max_input_bytes`; one
constant-sized incomplete query suffix can coexist with it. A missing reply,
excess input, or transport error fails setup explicitly. Failed setup consumes
already read input and returns no replay transport. Matching query syntax cannot
authenticate terminal replies against byte-identical injected input.

Cleanup restores encodings from their queried values and tracking with XTRESTORE.
This split avoids a verified xterm 390 defect: its encoding restoration casts the
saved numeric value to an 8-bit Boolean, while tracking restoration retains the
integer value. See the versioned primary sources
[ptyx.h macros](https://github.com/ThomasDickey/xterm-snapshots/blob/xterm-390/ptyx.h)
and [charproc.c saved modes](https://github.com/ThomasDickey/xterm-snapshots/blob/xterm-390/charproc.c).
Windows uses native console mouse events and sends no mouse queries or setup bytes.

With mouse reporting enabled, close first drains retryable POSIX output and then
restores modes, sharing one fresh `modeTimeout` deadline independent of cancellation.
It always closes the underlying OS session afterward. Partial setup, exceptions,
and destruction use the same cleanup path. A blocked or broken output path can
prevent VT restoration; explicit `close` reports that failure alongside any OS
cleanup failure. Destruction performs best effort and cannot report errors. Mouse
opt-out performs OS cleanup only. `TerminalSession` still performs no VT setup
and never flushes in close/destruction.

The managed policy requires an emulator implementing the relevant
[xterm control sequences](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html).
There is no acknowledgement of terminal interpretation. XTSAVE overwrites the
emulator's one-level saved tracking cache: active modes are restored, but a previously
saved cache is not preserved. Nested owners, concurrent terminal output, and external
changes to these modes are unsupported. Unsupported emulators do not acquire an
exact VT restoration guarantee; the explicit opt-out avoids their query requirement.

Setup and restoration bytes bypass FrameWriter and frame byte counters. A custom
transport decorator sees frame submissions and flushes, while factory setup and
close perform separate protocol I/O. Include them in lifecycle timing, not
rendered-frame throughput.
