# LogView {#log-view}

[Русская версия](ru/log-view.md) · [Scroll controls](scroll-controls.md) · [Control examples](control-examples.md)

`LogView` is a scene-owned, bounded plain-text journal for showing recent output.
Append complete batches of UTF-8 text; the control splits them into records, keeps
the newest records within configured limits, wraps each record on grapheme
boundaries, and paints only its prepared visible rows. Its default height is
flexible, so it uses the available vertical space; an explicit size policy can
change that. It has no child widget per record and does not perform terminal I/O,
file I/O or background work.

## Ordinary C++ program

Build the `portals_log_view_example` example with `PORTALS_BUILD_EXAMPLES=ON`,
then run it in a terminal. The view starts with `Ready`. Each Append batch click
adds one numbered three-record batch; the longer Unicode record wraps as the
terminal gets narrower. The example retains at most 12 records. Clear removes
the current records, and Return to tail resumes following. Scrolling upward also
pauses following while new records are retained and counted. F10 exits through
the standard runner.

[Complete source](../examples/log_view/main.cpp)

\include log_view/main.cpp

The example keeps its sample messages in memory. It does not read or write files
or claim to preserve output after the process exits.

## Loading LogView from a scene resource {#log-view-resource}

A scene resource can declare the built-in `LogView` by name. C++ obtains typed
refs and connects ordinary button handlers; the resource contains no executable
callbacks and needs no manual control registration.

[Complete resource program](examples/guide_log_view_resource.cpp)

\include guide_log_view_resource.cpp

The configuration properties are `text`, `maxLines`, `maxBytes`,
`maxBatchBytes`, `maxRecordBytes`, `followTail` and `readOnly`, alongside common
layout, frame and style settings. `text` starts empty; defaults for all four
limits are listed below, `followTail` defaults to `true`, and `readOnly` defaults
to `false`. The resource sample starts with one line; C++ buttons append a
two-record batch, clear retained records or return to the tail. Resource
values configure initial content and bounds; append operations remain C++
commands.

The vertical scrollbar uses the right frame side, and the status/Return-to-tail
footer uses the bottom side. Both sides must remain enabled. Frame, title and style
customization may change their appearance, but disabling either required side is
rejected with typed `INVALID_ARGUMENT` during mount. Keep resource
`frame.sides.right` and `frame.sides.bottom` enabled. There is no public
post-mount frame/options setter. The resource example is the complete loaded-scene
path; it does not add another control-registration or persistence layer.

## Batches, records and bounds

An append is one complete batch, not a fragment of a stream. CRLF, CR and LF each
separate records; CRLF is one separator. Empty records between separators are
kept. A final separator ends its preceding record and does not add a phantom
empty record. An empty batch is a no-op. The control does not join a partial
record across separate appends.

| Limit | Default | Hard ceiling |
| --- | ---: | ---: |
| Retained records (`maxLines`) | 512 | 8,192 |
| Retained UTF-8 payload (`maxBytes`) | 256 KiB | 4 MiB |
| Input batch including separators (`maxBatchBytes`) | 16 KiB | 256 KiB |
| One record, excluding its separator (`maxRecordBytes`) | 4 KiB | 64 KiB |

All limits must be positive. `maxRecordBytes` cannot exceed either
`maxBytes` or `maxBatchBytes`. A setting above its hard ceiling is rejected;
the control does not silently clamp it. `maxBytes` counts retained record payload
without line separators, while the batch limit includes separators. Empty records
count toward `maxLines` but contribute no payload bytes.

Each batch is validated and staged before publication. Invalid UTF-8, a limit
violation or an allocation failure rejects the whole batch and leaves retained
records and counters unchanged. A successful batch keeps the newest records that
fit both retained limits. If an oversized record count displaces records from
the same batch, those records are still counted as accepted input and dropped
output. Drop-oldest is a successful retention policy, not a partial-input error.

The wrap index stores numeric record/byte ranges for every visual row, so a
rewrap still scans retained source text and its graphemes; that work is not limited
to the viewport. Retained source records stay within the configured limits, while
the owning prepared `GraphemeText` paint cache contains only rows intersecting the
visible viewport. The limits bound records, payload and index size, but do not give
an exact process-RSS guarantee: grapheme metadata, record objects, visible caches
and staged old/new state also use memory.

## Wrapping and reading position

`LogView` wraps at its current text width without splitting a grapheme. A single
grapheme wider than the viewport is consumed once and clipped by the existing
terminal text policy. Zero-sized or hidden views draw no rows and preserve the
logical follow state or reading anchor until a usable viewport is prepared.

Following the tail is enabled by default. Set `followTail=false` to start paused
at the first record and visual row; the footer shows Return to tail. A deliberate
scroll away from the bottom also pauses following; reaching the bottom or choosing
Return to tail resumes it. Programmatic
`ui::Ref<LogView>::setOffset(ScrollAxis::VERTICAL, ...)` changes are deliberate
navigation too. Appending while paused preserves the reading anchor and
increments the `newWhilePaused` count. The footer reports retained records and
dropped history; while paused it also reports new records and offers Return to tail.

The anchor is a record identity plus a grapheme byte boundary at the first
visible row. Resizing rewraps records and places the view at the visual row that
contains that boundary. If retention evicts the anchored record, the view moves
to the first surviving record and the dropped count makes that loss visible.
The anchor is not an absolute visual row number, so a width change does not
pretend that the old row number still identifies the same text.

## Append, clear and status

`ui::Ref<LogView>` provides short `append(bytes)`, `clear()` and `tail()` commands
for ordinary event handlers. They return `bool` and report technical failures
through the existing scene diagnostic collector. `appendBatch(bytes)`,
`clearLog()` and `followTail()` are checked alternatives that return typed
results without automatically reporting technical failures. A successful
`appendBatch` returns an owning `LogAppendReceipt` with accepted, dropped and
retained record counts and retained payload bytes.

`status()` returns an owning `LogStatus` containing retained lines and bytes,
cumulative dropped lines, records accepted while following is paused, and the
follow/saturation flags. Saturating counters stop at their representable maximum
instead of wrapping; the flags identify a saturated total. The observation
contains scalar values only and does not retain records or the control. Clear
removes retained records, resets drop and paused-new counters, preserves record-ID
history and resumes following.

Mount a usable control through `ManagedScene::logView(...)` or `ui::logView(...)`.
Direct `WidgetTree::emplace<LogView>` alone is insufficient, even if its base
`ScrollArea` lease is initialized: it does not register the managed entry's
scroll-intent observer and preparation path. Commands and measurement on such a
node return typed `INVALID_ARGUMENT`. There is no standalone raw-control setup
path.

The scene owns the log records. A `Ref<LogView>` observes its control and does
not prolong its lifetime. Append copies caller bytes before successful return.
Programmatic append, clear and tail commands remain allowed when the control is
hidden, disabled or configured read-only; read-only blocks user navigation.
Commands follow the existing UI-thread, outside-frame and live-owner checks.
Later layout or render failure does not turn a successful append into a failed
admission: its records have already been retained.

This bounded control does not provide file tailing, worker or asynchronous
producers, incremental stream framing, filtering, search, selection, clipboard
copy, or persistent storage. Applications choose and own any source or retention
outside this in-memory view.
