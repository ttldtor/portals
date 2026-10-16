# Terminal diagnostic probe

[Русская версия](ru/terminal-probe.md)

`portals_terminal_probe` inspects the terminal environment and collects limited
DA/CPR replies. Run it separately after closing your Portals application.
Use the [result guide](#terminal-probe-result) to interpret the report and the
[observation table](#terminal-observations) to see what has been checked.

<details>
<summary>Standalone probe execution contract</summary>

`portals_terminal_probe` is a standalone C++23 diagnostic executable. It is the
sole owner of its standard terminal handles while running; do not run a second
reader or a UI session against those handles concurrently. It does not link the
Portals core or expose a library TerminalSession API. All report values own their
strings, and calls run on the main thread. No callbacks or borrowed results escape
the executable. There is no model, widget or layout invalidation involved.

</details>

## Build and use

The executable is built by default when Portals is the top-level CMake project
on Windows and POSIX.
Embedded projects default `PORTALS_BUILD_TOOLS` to OFF. No extra dependency or
download is required. Run these commands from a Portals source checkout:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug --target portals_terminal_probe
./build/windows-msvc-debug/portals_terminal_probe --help
./build/windows-msvc-debug/portals_terminal_probe --no-query
./build/windows-msvc-debug/portals_terminal_probe --timeout-ms 250 --max-bytes 4096
```

On Linux, build with `linux-gcc-debug` and run
`./build/linux-gcc-debug/portals_terminal_probe` from the terminal under test.
`--headless` is an alias for `--no-query`. Inspection mode neither changes terminal
modes nor reads input. Redirecting either stdin or stdout automatically skips
queries. It never opens `/dev/tty` or `CONIN$` to bypass redirection.

Options accept positive decimal integers: `--timeout-ms` is 1..10000 (default
250), and `--max-bytes` is 1..65536 (default 4096). `--width-policy narrow|wide`
selects the **advisory** ambiguous-width policy (`NARROW` by default). This probe
does not measure glyph widths or choose a Unicode version; its report explicitly
labels those facts unknown. The library
uses its explicit `WidthPolicy`; see [text width](text.md).

Exit status is 0 for a completed diagnostic observation, including unsupported
queries, byte limits, missing replies or skipped queries. Status 1 reports a native
I/O/mode/restore failure or failed report output. Status 2 reports invalid options.
Missing replies are not classified as proof that a feature is unsupported.

<a id="terminal-probe-result"></a>
## Read the result

To obtain terminal replies, run the probe without `>` redirection or a pipeline.
Redirecting stdin or stdout skips queries. Copy the completed report after the
program exits. Use `--no-query` to inspect the environment without reading input
or changing modes.

| Report field | Meaning | Next action |
|---|---|---|
| `query.stop="replies"` | Matching replies were collected for both queries | Keep `query.da.reply` and `query.cpr.reply` with the run conditions |
| `query.stop="disabled"` | `--no-query` or `--headless` was selected | This is environment inspection; terminal replies were not tested |
| `query.stop` starts with `skipped:` | Suitable terminal handles are unavailable, for example because of redirection | Repeat in the intended terminal without redirection |
| `query.da.reply` or `query.cpr.reply` contains `unknown` | No matching reply was received | Check the environment, timeout and `query.error`. A missing reply does not establish its cause |
| `query.error` differs from `none` | A terminal operation or restoration failed | Keep the cause; use a new terminal session after a restoration failure |
| `input.byte_limit_hit="true"` | The input byte budget was exhausted | Repeat without keyboard input; if needed, increase `--max-bytes` within its documented range |
| `input.unmatched` is not empty | The probe consumed bytes that were not accepted as DA/CPR replies | Repeat without keyboard input for a clean observation. These bytes have already been consumed |

Exit code 0 means the observation completed. Queries may still have been skipped
or replies absent. Successful DA/CPR establish the replies received in that run;
keys, mouse and Kitty support need separate checks.

<a id="terminal-observations"></a>
## Observed terminal behavior

These are retained historical runs, limited to the named programs and conditions.
Compatibility with the current Portals release requires a repeat run.
The records were reviewed on 16 October 2026. Unknown facts remain unknown.

| Terminal and environment | Confirmed observation | Method | Not tested |
|---|---|---|---|
| XTerm 390 (`390-1ubuntu3`), Ubuntu 24.04 / WSL2 / WSLg, probe on 7 October 2026 | DA and CPR received; before/after termios settings matched | Real XTerm through a `script` PTY; the driver did not inject replies | Keys, mouse, event phases, glyph width and appearance |
| Windows Terminal package `1.24.11911.0`, executable FileVersion `1.24.2607.10001`; Windows, OS version unknown | The application received key, text and mouse events; resize and mode/code-page restoration passed | Windowed application run; input records injected through `WriteConsoleInputW` | Physical keyboard/mouse input, DA/CPR, event phases and glyph appearance |
| XTerm 390, Linux / WSL2 / WSLg; windowed application run | Key and text events received; resize and termios/descriptor-flag restoration passed | Automated X11 driver: `XSendEvent` and `XResizeWindow` | Physical input, DA/CPR in this run, event phases and glyph appearance; mouse delivery unconfirmed |

In the last run, the application did not enable DEC mouse tracking modes
1000/1006. Zero delivered events does not determine XTerm's capabilities.
All input scenarios in this table used automation; they did not exercise manual
input or visually assess Unicode glyphs.

The [observation records](measurements/terminal-observations.json) contain separate
scenarios, tested binary hashes where available and redacted results. The dates
and runtime source revisions of the windowed runs are not established and remain
`null`. The Windows Terminal package version and executable FileVersion are
recorded separately. The probe does not reliably identify either.

Kitty, individual left/right modifiers and the full PRESS/REPEAT/RELEASE set were
not tested by these observations. Kitty negotiation in Portals is **TBD — M7 / PRTLS-89**.
Current phase delivery depends on the adapter; the ordinary VT decoder emits
PRESS, including repeated bytes. See [keyboard input](input.md).

<a id="terminal-observation-checklist"></a>
## Check your environment

1. Close the Portals application. Record its build, OS, independently observed
   terminal version (for example, from its About dialog) and launch path:
   local, WSL/SSH or a multiplexer.
2. Run `portals_terminal_probe --no-query` and keep the initial modes,
   code pages or termios values from the report.
3. Run `portals_terminal_probe --timeout-ms 250 --max-bytes 4096`
   without redirection. Do not type during the query window. Then repeat
   `--no-query` and compare mode fields with the initial report. Record DA/CPR
   success and restoration separately.
4. Run the [first application](first-application.md) separately: check Tab,
   Enter, F2, F3 and F10. Use separate scenarios for the mouse, paste, resize
   and glyphs your application needs. Record the conditions and actual results,
   including what was not tested.

Do not run the probe and application against the same terminal input concurrently.
Synthetic PTY/ConPTY and replay tests check their own boundaries; they do not by
themselves establish behavior in a particular GUI terminal. Query and restoration
contracts follow below.

## Report and evidence

`portals-terminal-probe-v1` emits `key="value"` lines. All dynamic values are
escaped: quotes/backslashes are quoted, control bytes, DEL and non-ASCII bytes
become `\xhh`. Report values cannot inject ANSI/OSC sequences or new report lines.
Environment hints are capped at 1024 bytes per variable, with an explicit
`.truncated` field when necessary. Environment strings are untrusted evidence;
`WT_SESSION`, `TERM`, `TERM_PROGRAM`, `TERM_PROGRAM_VERSION`, `TMUX`, `STY` and SSH
hints do not establish the actual terminal identity, version or transport path.

Windows reports the OS build from `RtlGetVersion` when available, native processor
architecture, console modes/code pages, viewport and backing buffer dimensions.
Console handles can be mediated by ConPTY, so the underlying frontend is unknown.
POSIX reports `uname`, `isatty`/`ttyname`, `TIOCGWINSZ` and termios flag values.
Modes and dimensions describe the state **before** temporary query-mode changes.
Underlying local/PTY/SSH/multiplexer transport remains unknown unless supplied by
an external verified observation.

## Query window and input classification

The limited query set is primary DA (`ESC [ 0 c`) and cursor position DSR/CPR
(`ESC [ 6 n`), written once. The collector accepts primary DA candidates
`ESC [ ? number [;number...] c` and positive two-coordinate CPR candidates
`ESC [ row ; column R`. Numeric overflow, malformed sequences, other control
sequences and duplicate replies remain in `input.unmatched`. Eight-bit CSI and
secondary/tertiary DA are intentionally not queried or decoded. There is no
negotiation of mouse, paste, graphics, color depth or Unicode width capability.

The collector stores the first matching candidate for each outstanding query,
separately from unmatched input. It handles replies across read/chunk boundaries,
caps a candidate sequence at 64 bytes and caps total retained input at
`--max-bytes`. Completed replies stop further waits; any bytes already read in the
same batch are still classified. Partial sequences are flushed to unmatched input
at the end. There is one steady-clock deadline covering mode setup, query output
and response collection; incoming bytes do not extend it. Readiness waits use only
the remaining time. POSIX reads/writes are nonblocking during the query window.

Windows enables `ENABLE_VIRTUAL_TERMINAL_INPUT` during the query window so ConPTY
preserves incoming CPR bytes instead of translating them into function-key records.
It uses counted `ReadConsoleInputW` records after a readiness wait, rather
than a blocking byte read on a console signaled by mouse/resize events. ASCII
key-down characters enter the byte collector. Other records, including UTF-16
non-ASCII characters, key releases, mouse and resize events, are counted separately
in `input.non_byte_events`, with a bounded 1024-byte numeric summary. ASCII repeat
remainders excluded by the byte cap are also recorded in that summary, so records
already removed from the queue are not silently dropped. Record
consumption is also limited to `--max-bytes` records. Key repeats are expanded
only within the remaining byte budget. This is a query diagnostic, not a complete
Unicode/key/mouse input decoder.

Native API execution and OS scheduling are not a hard real-time guarantee.
In particular, Win32's small synchronous console query write and counted console
record operations have no per-call cancellation timeout. The program never
performs a line/byte input read with an unbounded wait, but cannot promise that
a stalled console host returns from a native call within the collection deadline.
`query.elapsed_ms` records observed duration; external process timeouts remain
appropriate for automated diagnostics against untrusted/stalled hosts.

DA/CPR do **not** authenticate terminal identity. The xterm documentation
explicitly notes that modified function keys can encode the same bytes as CPR.
Typed or pasted bytes and stale responses can also imitate replies. The tool
reports syntactic **candidates**, not trusted feature negotiation. Data arriving
after the deadline is not drained or claimed as a late reply. It can remain in
the OS input queue and later reach the parent shell; unmatched input that the
probe did read is reported and consumed, not replayed. Avoid typing during the
short interactive query window when collecting clean evidence. Inspection mode
is available when consuming any input is undesirable.

## Mode lifetime and checks

Temporary modes are scoped and restored on ordinary success, timeout, input
limits, native errors and C++ exception unwinding. Failed explicit restoration
is reported as an error and the guard retries on destruction. Windows disables
line/echo/processed input and quick edit, and enables VT input plus processed/VT
output. POSIX disables canonical/echo/signal interpretation
and selected input translations, uses `VMIN=VTIME=0`, and restores descriptor
flags. No input flush is performed. POSIX SIGINT/SIGTERM/SIGHUP handlers cause the
query loop to exit and restore modes. Forceful process termination, SIGKILL,
console closure or fatal OS failure cannot be guaranteed recoverable by guards.

Automated tests cover fake-clock deadlines, every split point, bytewise chunks,
reply grammar, duplicates, garbage, sequence/input limits, byte conservation and
report escaping. A captured-stdout subprocess fixture verifies redirected/default,
`--no-query`, `--headless`, help and invalid timeout invocations complete without
raw query bytes or input reads. A synthetic POSIX PTY fixture runs the real native
path and checks termios/descriptor restoration after replies, a missing-response
deadline, the byte cap and SIGTERM. A headless Windows ConPTY fixture checks that
a host-injected CPR survives the real native mode guard and counted record collector,
and that modes/code pages return to their exact initial values. A queue marker
synchronizes injection before collection; system ConPTY may answer queries itself,
so the injected CPR deliberately differs from its local cursor position. The test
includes the private native implementation to exercise its guard without a product
test hook. These tests do not establish real terminal behavior.

For an interactive qualification, run the native Windows build in a real Windows
Terminal, and the Linux build in a real Linux terminal emulator. Record executable
build/configuration, OS/terminal versions from independently observed sources,
transport, report, elapsed time and modes before/after. Test missing-reply timeout
and typing during the window as separate cases. A WSL shell alone, a synthetic PTY or a headless runner does not
replace a run in a real terminal frontend; record NOT_RUN when that surface
is unavailable.

Primary references:

- [Microsoft Console VT sequences: query state and mode flags](https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences)
- [Microsoft ReadConsoleInput: counted input records and handle readiness](https://learn.microsoft.com/en-us/windows/console/readconsoleinput)
- [Microsoft GetNumberOfConsoleInputEvents](https://learn.microsoft.com/en-us/windows/console/getnumberofconsoleinputevents)
- [Microsoft RtlGetVersion](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-rtlgetversion)
- [xterm control sequences: DA, DSR, and CPR/function-key ambiguity](https://invisible-island.net/xterm/ctlseqs/ctlseqs.html)
- [POSIX poll](https://pubs.opengroup.org/onlinepubs/9799919799/functions/poll.html)
- [POSIX terminal interfaces](https://pubs.opengroup.org/onlinepubs/9799919799/basedefs/V1_chap11.html)
