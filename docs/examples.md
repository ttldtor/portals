# Runnable examples {#examples}

Start with the [minimal click-handler program](../examples/click_handlers/main.cpp).
For a form with editing, actions, custom widgets and virtual views, see
[declarative composition](declarative-ui.md). [Text and column examples](compact-examples.md)
cover formatting, member projections and optional aggregate columns.

Build with `PORTALS_BUILD_EXAMPLES=ON`. The gallery, dashboard, replay and custom-widget programs run through the same
[Application runner](application.md) and each has a finite `--check` mode for automation.
The check modes use an in-memory terminal and logical clock; they do not prove
native terminal restoration or visual compatibility.

| Program | What to try |
| --- | --- |
| `portals_click_handlers` | Minimal pipe example: click Add Hello; F10 quits. Native interactive mode only; its source is also compiled by the installed-package consumer. |
| `portals_gallery` | Declarative managed form: Tab through editor, checkbox, action and Quit; Enter commits; F2 cycles six themes. |
| `portals_dashboard` | Compact market screen with a table and chart sharing one model; `--stress` adds detailed diagnostic status. |
| `portals_custom_widget` | A custom final meter with a managed presentation adapter and scalar managed state; Left/Right change its level, F2 cycles themes. |
| `portals_replay_example` | The dashboard with the built-in recorded price tape selected by default. |

F10 exits every interactive example. Gallery/dashboard/custom-widget also accept Ctrl+C. Mouse routing is provided by the
existing bindings where supported by the terminal. The examples install no signal
handlers and do not promise cleanup after abrupt process termination.

## Gallery and custom widget options

```text
portals_gallery --theme nord
portals_custom_widget --theme catppuccin-latte
portals_gallery --check --theme gruvbox --ansi16
```

Both programs accept `--theme NAME`, `--ansi16`, `--check` and `--help`.
`--theme` selects the initial palette: `modern` (default), `turbo`, `nord`,
`gruvbox`, `tokyo-night`, or `catppuccin-latte`. F2 cycles in that order,
including wraparound. `--ansi16` quantizes actual emitted colors to sixteen
terminal colors; the terminal's configured ANSI palette determines their final
appearance. `--check` uses a finite in-memory keyboard/resize replay and exits;
it opens no native window. `--help` prints these options and exits. Unknown
options or theme names and missing theme values return exit code 2. Runtime
failures return 1 after the runner attempts terminal restoration; success is 0.
The gallery also retains the historical `initial`, `keyboard`, `mouse` and
`disabled` finite fixture modes. They use the same replay runner afterward.

The interactive gallery uses `gallery::FluentForm`. Its owning scene retains
controls, state and scoped registrations. The example-only `take` helper converts
setup failures to a generic exception. Applications that need detailed diagnostics
should preserve the typed results and use the checked runner boundary.

The four adapted [Textual palettes](styles.md) change colors
through existing style transactions. They do not reproduce Textual's complete
widget design or introduce a CSS cascade. The dashboard/replay example uses
Modern/Turbo with managed table and chart preparation.

For nested tables and charts, the [layout preparation examples](virtual-preparation.md)
show complete C++23 consumers of the installed package, from explicit extents
through layout snapshots to managed models and views. These finite examples
compare terminal output and resize behavior. The interactive dashboard exercises
the same managed factories with delayed provider work.

## Dashboard configuration

```text
portals_dashboard --rate 10000 --fps 30 --rows 1000000 --seed 42
portals_dashboard --rate 0 --delay-ms 250 --ansi16 --ascii
portals_dashboard --mode replay --trace trace.json --duration-ms 5000
portals_dashboard --check --mode replay --delay-ms 250
portals_dashboard --stress --rate 10000 --rows 1000000
```

The default screen puts the editable note and Inspect action on one row, followed
by a short status, the table and the chart. `--stress` preserves the detailed
diagnostic layout used by the existing performance workloads. The shared replay
program accepts the same option. Changing the layout does not select a different
runtime or data source.

Read [the UI composition](../examples/dashboard/dashboard.hpp) separately from
[the data provider](../examples/dashboard/provider.hpp). The provider owns tick
generation and bounded page requests; the UI adapter connects its model to the
managed table and chart. The example uses a borrowed managed scene because
its explicit provider shutdown and runner observers are not exposed by the owning
scene facade. Use this example to study provider integration.

`--rate` accepts 0..10000 scheduled ticks/second; zero demonstrates idle and delayed
range navigation without market updates. `--fps` accepts 1..1000 frames/second.
`--rows` sets 1..1000000000 logical instruments without creating a widget per row.
`--seed` controls the deterministic synthetic tape. `--mode synthetic|replay`
selects the data source. The replay tape lives in `examples/replay/data.hpp` and is
compiled into the executable, so running from another directory needs no data file
or network. Replay loops its 16 recorded prices in order.

`--delay-ms` delays acquisition of an uncached page (0..60000 ms). The first
page is immediately ready. Moving within a useful page keeps its original load
deadline even when the view replaces its model request. A cache miss can show
loading placeholders; completing a useful page does not require releasing the
arrow key. Table and chart keep independent model requests and share bounded
provider page availability.

| Loading option | Default | Meaning |
| --- | --- | --- |
| `--page-rows N` | 256 | Logical rows per fixed page. |
| `--cache-pages N` | 8 | Maximum availability records, including the initial page and queued work. |
| `--cache-bytes N` | 262144 | Charged row payload budget for one generated snapshot, in bytes. |
| `--load-jobs N` | 4 | Maximum simultaneous delayed page jobs. |
| `--prefetch-pages N` | 1 | Adjacent pages considered in each consumer's navigation direction; zero disables prefetch. |

Positive sizes must fit `size_t`. Invalid or undersized combinations fail setup;
a viewport that cannot fit the effective page budget produces a failed range.
The byte charge uses a full page of `ModelRow<Quote>` even for the final short
page. It excludes allocator overhead and old immutable snapshots retained by the
model, so it is not a process memory limit. Demand takes priority over prefetch.
`--check` prints cache hit/miss page probes, useful/cancelled jobs and page/job/byte
high-water counters after shutdown.

This synthetic provider caches availability of stable logical instruments, not
network data. Each snapshot regenerates their values from its own market aggregate
and revision. Reusing arbitrary provider results across revisions needs an explicit
source contract. Readiness publication also works with `--rate 0`; an idle page
does not cause repeated publication or redraw. No full-model cache is built.

`--pump-budget` sets 1..10000 ticks per provider callback, default 512. The source
aggregates every generated tick into OHLC before publishing display snapshots.
Only immutable display updates coalesce. A late timer retains the next ungenerated
ordinal and catches up in bounded callbacks; scheduled ticks and actually generated
`ticks` are distinct. Stop cancels remaining scheduled generation rather than
claiming that ungenerated data was processed. This demo provider runs on the UI
owner; it is not a benchmark of an independent market worker.

The managed scene pins the shared model once per frame for both table and candle
chart, including any bounded layout retries. Logical instruments
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

Start with [the fluent gallery](../examples/gallery/fluent_form.hpp) for editing,
checkboxes, actions and themes. [The fluent custom meter](../examples/custom_widget/fluent_meter.hpp)
shows a final widget whose preparation adapter reads immutable state and retains
presentation for painting.

[The dashboard scene](../examples/dashboard/dashboard.hpp) combines a model,
virtual views and a data provider. Its managed factories own ranges, input bindings
and standard styles. Grid layout determines the view extents.

Provider work remains visible in the dashboard. The table's preparation handler
reads its first row before the chart's origin callback runs; their registration
order keeps the chart aligned with the table. Both handlers receive the same
borrowed frame pin and copy new requests into the existing bounded provider
slots. The owning `pendingLatest` slot is cleared only after the model publisher
accepts it. The provider keeps its timer, retry, cancellation and prefetch policy;
managed preparation adds no second scheduler or cache. On stop, the scene cancels
provider work before closing the managed subtree and model. No example requires
private work files.

For measurements, `dashboard::Options::ingressObserver` can collect bounded
logical queue-latency samples at entry to the snapshot ingress callback. It covers
that ingress only, not the model's separate DATA endpoints. Null disables the
sampling branch. `tickCount`, `scheduledTicks`, `pendingTicks` and `stoppedAt`
expose observed provider progress for an external benchmark without inventing
throughput from the configured rate. Observer storage remains caller-owned.

## Gallery source

[Open the fluent gallery source](../examples/gallery/fluent_form.hpp).

The gallery and custom widget entry points use scoped `ui::quit()` and `ui::runApp`.
The runner reports typed setup and runtime failures after attempting terminal cleanup.
Their scene objects also provide typed `run()` for headless replay.

## Minimal click-handler program

The complete current source is [click_handlers/main.cpp](../examples/click_handlers/main.cpp). It uses observing refs, runtime string styles and late pipe registration. The scene owns the controls; copied refs do not extend their lifetime. It has no CLI options or --check mode; automated tests compile it and exercise registration through the separate headless consumer. The ordinary runner reports typed setup/runtime failures after cleanup. See [callback parameters, stop and lifetime](declarative-ui.md#pipe-click-handlers).
