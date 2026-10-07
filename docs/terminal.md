# Terminal session

`<portals/terminal.hpp>` provides a move-only POSIX terminal session. Windows
currently returns `TerminalErrorKind::unsupported`; the separate diagnostic probe
does not implement this API. Linux PTY and pipe fixtures exercise the POSIX backend.
Other POSIX targets have not been qualified.

## Ownership and modes

Call `TerminalSession::open({input_fd, output_fd}, options)` from the terminal
owner. Both descriptors must refer to TTYs. The session duplicates them with
close-on-exec and closes only its own duplicates. The caller retains its original
descriptors, but must grant exclusive access to **all aliases and the terminals**
until close: duplicate descriptors share file status flags and termios belongs to
the terminal. No logger, widget, other session or external thread may change modes
or consume/write the same streams during this interval.

Both original termios states and both file status flags are saved before the first
mutation, including when input/output are the same descriptor. Input disables
canonical mode, echo, signal-generating control keys, input translations and
XON/XOFF output flow control (`IXON`), uses eight-bit characters, and sets `VMIN=1`, `VTIME=0`.
Output disables `OPOST`; both streams become nonblocking. Ctrl+C is consequently
an input byte, not an application stop signal. No alternate screen, mouse-reporting
or bracketed-paste VT modes are enabled by this layer.

All calls, moves and destruction belong to one executor. A synchronized ownership
transfer between executors is allowed; concurrent calls are not. The session
installs no process-wide signal handler and never runs UI code from a signal.
Allocation failures propagate as exceptions; native and contract failures use
owning `TerminalError` values. The native error code is `errno` on POSIX.

`close()` discards pending output, independently restores every changed mode and
flag in reverse order, and closes the owned duplicates. It reports the first
restoration/close error and is idempotent. Destruction and move assignment perform
the same best-effort cleanup without throwing; use explicit close to observe an
error. A failed `open()` also attempts every acquired restoration; its error keeps
the original native code plus the first rollback error in `restoration_code`.
Disconnected terminals may no longer permit restoration. Abrupt process death,
external descriptor mutation and signal termination do not run a promised cleanup.

## Input, deadlines and resize

`read(deadline, stop)` returns one owning `TerminalRead`. On POSIX, `bytes` contains
at most `max_input_bytes` raw bytes and `events` is empty. Feed these bytes to the
separate `InputDecoder`; the decoder does not own terminal I/O. `events` provides
the bounded semantic-event result boundary for a future native console backend.
Returned bytes, events and cell sizes survive subsequent calls, moves and close.

The deadline is an absolute `steady_clock` time point. Read/write retries after
`EINTR` recompute remaining time. Each poll lasts at most `wait_cadence` (10 ms by
default); stop is checked between native operations. This is a polling latency
bound, not a real-time scheduling guarantee. A pending stop takes precedence over
an expired deadline. EOF follows the last readable bytes, including simultaneous
`POLLIN | POLLHUP`. A PTY hangup is still read as EOF when querying its size fails.

`size()` returns columns/rows as `CellSize`. While reading, the owner samples
`TIOCGWINSZ` on each loop and reports a changed size through `resized`. Rapid changes
may coalesce; a change during an otherwise idle read is observed within the polling
cadence plus OS scheduling. Resize is not an error and does not alter earlier data.
The session neither installs nor replaces `SIGWINCH` handling. The kernel's
[window-size interface](https://man7.org/linux/man-pages/man2/TIOCGWINSZ.2const.html)
is the source of geometry.

## Output and renderer state

`submit(std::string)` accepts one batch of at most `max_output_bytes` (1 MiB by
default); another submission is rejected while bytes remain pending. The session
copies the accepted bytes into compact owned storage so a caller's oversized
reserved capacity is not retained. No borrowed span or callback survives the call.
This costs a bounded copy; it avoids an unbounded internal output queue.

`flush(deadline, stop)` returns the **successful native byte count for that call**,
the remaining count, and a status. It advances by the exact positive result of each
nonblocking write. Deadline, stop and errors preserve the unwritten suffix, allowing
an explicit later flush. A zero-progress native write is an `EIO` error. An empty
batch is already complete and flush returns progress even with an expired deadline.
The caller must handle errors rather than retry indefinitely.

Completed bytes are not proof that a complete VT transaction was interpreted or
displayed. The renderer owns its belief about the visible terminal; interrupted or
failed output requires its explicit recovery/resynchronization policy. No terminal
I/O, flush, mode change or waiting belongs inside paint/measure/arrange.

The private POSIX writer also handles a closed pipe without killing its caller.
It temporarily blocks `SIGPIPE` in the calling thread, preserves a previously
pending signal, consumes a newly pending signal after `EPIPE`, then restores the
previous mask. The process-wide disposition is unchanged. Public session creation
still rejects redirected pipes; the lower-level writer is tested independently.
The implementation follows the native [write](https://man7.org/linux/man-pages/man2/write.2.html),
[poll](https://man7.org/linux/man-pages/man2/poll.2.html),
[thread signal mask](https://man7.org/linux/man-pages/man3/pthread_sigmask.3.html) and
[signal wait](https://man7.org/linux/man-pages/man2/sigtimedwait.2.html) contracts.

## Verification

`portals.terminal.posix` uses its own child process, PTYs and nonblocking pipes.
It checks exact mode/flag restoration, caught exceptions, move ownership, bounded
input/output, resize, deadline/stop, partial writes, input EOF and closed-pipe
survival with default SIGPIPE disposition. A narrow private syscall seam supplies
deterministic short writes, EINTR and elapsed time. Linux link wrappers inject
termios acquisition/restoration failures without adding public injection APIs.
The child has a ten-second watchdog and CTest adds a fifteen-second outer timeout;
no product UI or user terminal is opened or modified.
