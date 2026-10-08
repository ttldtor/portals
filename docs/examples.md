# Runnable examples {#examples}

Build with `PORTALS_BUILD_EXAMPLES=ON`. Each program runs through the same
[Application runner](application.md) and has a finite `--check` mode for automation.
The check modes use an in-memory terminal and logical clock; they do not prove
native terminal restoration or visual compatibility.

| Program | What to try |
| --- | --- |
| `portals_gallery` | Tab through editor, checkbox, action and Quit; Enter commits; F2 switches themes. |
| `portals_dashboard` | Synthetic market, shared table/chart model, selection and delayed virtual ranges. |
| `portals_custom_widget` | A custom meter widget and independent immutable model; Left/Right change its level. |
| `portals_replay_example` | The dashboard with the built-in recorded price tape selected by default. |

F10 or Ctrl+C exits every interactive example. Mouse routing is provided by the
existing bindings where supported by the terminal. The examples install no signal
handlers and do not promise cleanup after abrupt process termination.

## Dashboard configuration

```text
portals_dashboard --rate 10000 --fps 30 --rows 1000000 --seed 42
portals_dashboard --rate 0 --delay-ms 250 --ansi16 --ascii
portals_dashboard --mode replay --trace trace.json --duration-ms 5000
portals_dashboard --check --mode replay --delay-ms 250
```

`--rate` accepts 0..10000 scheduled ticks/second; zero demonstrates idle and delayed
range navigation without market updates. `--fps` accepts 1..1000 frames/second.
`--rows` sets 1..1000000000 logical instruments without creating a widget per row.
`--seed` controls the deterministic synthetic tape. `--mode synthetic|replay`
selects the data source. The replay tape lives in `examples/replay/data.hpp` and is
compiled into the executable, so running from another directory needs no data file
or network. Replay loops its 16 recorded prices in order.

`--delay-ms` delays first acquisition of an uncached viewport range (0..60000 ms).
The initial 256-row page is immediately ready; scroll beyond it to see placeholders.
Two independent bounded range slots serve table and chart, with intentionally
reversed completion order. Changing a range cancels its old request. Once acquired,
that current range can be regenerated with new immutable market revisions, so a
continuous market does not permanently starve a delayed viewport.

`--pump-budget` sets 1..10000 ticks per provider callback, default 512. The source
aggregates every generated tick into OHLC before publishing display snapshots.
Only immutable display updates coalesce. A late timer retains the next ungenerated
ordinal and catches up in bounded callbacks; scheduled ticks and actually generated
`ticks` are distinct. Stop cancels remaining scheduled generation rather than
claiming that ungenerated data was processed. This demo provider runs on the UI
owner; it is not a benchmark of an independent market worker.

The table and candle chart use a single provider pin per frame. Logical instruments
share the market OHLC with a deterministic price offset. Table formatting covers
only visible rows; preparation retains visible rows plus two overscan rows per
side. No million-row materialization is performed. The title and editable note
exercise Unicode. Dashboard candles use one-cell Unicode wicks and filled bodies
by default; `--ascii` selects ASCII labels and candle brushes. `--ansi16` selects
actual ANSI16 encoding, not an advertised capability flag. Both themes are
compiled typed equivalents of the distributed PSS themes and require no runtime
theme-file lookup. Themes change controls, table colors and the chart background;
rising/falling market colors remain distinct.

Tab switches focus, arrows/Home/End select/scroll the table, Enter commits the
note, and F2 switches Modern/Turbo. The Inspect button deliberately sets the note
to its existing value. With `--rate 0 --trace trace.json`, focus Inspect and press
Enter: the metadata trace distinguishes handler return, `value_unchanged`, and
`frame_not_requested`. Text values and terminal bytes are not exported. File errors
are reported after terminal restoration, independently of a prior run failure.

`--duration-ms` stops an interactive run after at most the next provider service
opportunity (up to 3600000 ms); zero leaves it interactive. Output/host stalls can
still extend real cleanup, as documented by `TerminalSession`. `--check` instead
replays a finite keyboard/resize script for one logical second and prints observed
frame/tick/byte/queue totals after close.

## Where to start in the source

`examples/gallery/form.hpp` shows explicit nodes, models and scoped bindings with
`Application` handling the recurring terminal/frame mechanics. The older
`gallery.hpp` finite fixture remains for existing semantic replay tests.
`examples/custom_widget/main.cpp` shows an alternative provider and a widget which
formats its presentation before rendering. `examples/dashboard/dashboard.hpp`
shows shared pin preparation and an owning `pendingLatest` slot that is cleared
only after the model publisher accepts it. No example requires private work files.

For measurements, `dashboard::Options::ingressObserver` can collect bounded
logical queue-latency samples at entry to the snapshot ingress callback. It covers
that ingress only, not the model's separate DATA endpoints. Null disables the
sampling branch. `tickCount`, `scheduledTicks`, `pendingTicks` and `stoppedAt`
expose observed provider progress for an external benchmark without inventing
throughput from the configured rate. Observer storage remains caller-owned.
