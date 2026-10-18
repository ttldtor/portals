# Runnable examples {#examples}

[Русская версия](ru/examples.md)

Start with **click handlers**, then the **gallery**. Both use the owning declarative
scene: describe controls, attach behavior, and finish with `ui::runApp()`.
The library collects typed setup errors and restores the terminal before reporting
runtime failures. Low-level APIs remain available for customization and tests. The custom-widget section explains painting and preparation explicitly; the first examples do not require those details.

Build with `PORTALS_BUILD_EXAMPLES=ON`. Examples open your terminal. Automated
replay terminals, finite checks and stress workloads live under `tests/` and
`tools/benchmarks/`, outside the public entry points.

## Choose an example

The categories describe the learning path; the source directories remain stable.

| Category | Program and complete source | What it demonstrates |
| --- | --- | --- |
| Basics | [click handlers](../examples/click_handlers/main.cpp), `portals_click_handlers` | Observing refs, late click handlers and short quit. No CLI. |
| Basics | [compact framed form](../examples/compact/main.cpp), `portals_compact_form` | Ready-made border and gauge, fixed sizing, a plain button and exact-ref styling; no custom widget or CLI. |
| Basics | [resource form](../examples/scene_resource/main.cpp), `portals_scene_resource` | Runtime scene text, named references and existing managed actions; see [scene loading](scene-loading-guide.md). |
| Basics | [gallery](../examples/gallery/main.cpp), [form](../examples/gallery/fluent_form.hpp), `portals_gallery` | A function returning a lazy scene, internal editor/checkbox state, buttons, six themes and CLI preflight. |
| Showcase | [editor history](../examples/editor_history/main.cpp), `portals_editor_history` | Immediate, editing-finished and explicit model application; Undo/Redo; draft and published values. |
| Showcase | [dashboard](../examples/dashboard/main.cpp), [composition](../examples/dashboard/dashboard.hpp), `portals_dashboard` | A table and chart sharing an immutable model, delayed pages and bounded provider work. |
| Customization | [custom widget](../examples/custom_widget/main.cpp), [meter](../examples/custom_widget/meter.hpp), [scene factory](../examples/custom_widget/fluent_meter.hpp), `portals_custom_widget` | A final widget, retained painting, managed presentation and a function returning a lazy scene. |
| Experimental | [compile-time styles](compile-time-styles.md) | Compile-time descriptors and PSS checking: optional advanced material, not prerequisites for a form. |

F10 exits the interactive programs. The gallery, custom widget and dashboard
also bind the exact Ctrl+C shortcut. Their F10 quit bindings are unmodified;
modified quit chords do not inherit the older modifier-insensitive matching.
Mouse routing depends on the active terminal protocol.
No example promises restoration after forced process termination.

## Complete first program

This is the complete current source, including its entry point. It has no private
helpers, manual binding tokens, stop source or presentation-sync loop.

[Open the source](../examples/click_handlers/main.cpp).

```cpp
// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Run a minimal lazy scene with a late click handler.
 * @return Zero on success, one on a typed setup/runtime failure after library cleanup/reporting.
 * Click Add Hello to append one undoable text change; F10 quits. This minimal program defines no CLI options.
 * The standard runner owns terminal restoration.
 */
int main() {
  using namespace portals;

  ui::Ref<LineEdit> message;
  ui::Ref<Button> add;

  auto scene = ui::scene(ui::vbox(ui::lineEdit(message, ""), ui::button(add, "Add Hello")));

  return scene
    | ui::onClick(add,
                  [message] {
                    message.text() += "Hello!";
                  })
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
}
```

Clicking Add Hello appends an undoable change. `ui::quit` is a command accepted by
`ui::onKey`; inside an admitted handler, `ui::quit()` closes its current scene.
Refs observe controls and do not prolong their lifetime. See
[handler parameters, propagation and lifetime](declarative-ui.md#pipe-click-handlers).

## Themes, application styles and customization

Public examples use the standard runner and default style-layer metadata. Explicit source labels, trace export, replay clocks and transport assertions belong to diagnostics material and test harnesses; they are not prerequisites for a form.

The gallery and custom widget accept `--theme NAME` and `--ansi16`; F2 cycles
`modern`, `turbo`, `nord`, `gruvbox`, `tokyo-night`, and `catppuccin-latte`.
The latter four adapt Textual palettes, not its complete widget design.
`--ansi16` maps emitted colors to the terminal's configured sixteen-color palette.

The gallery is the ordinary `gallery::form(ui::opts(options))` function; there
is no stationary gallery class to manage. Its [native Lyra schema](../examples/gallery/options.hpp)
binds `-t`/`--theme` and `--ansi16` to caller-owned options. The final
`ui::runApp(argc, argv, ui::lyra::cli(lyraCli))` parses before application settings,
mounting or terminal acquisition. `--help` returns zero; invalid options return
two with an owning command-line diagnostic. Keep the options alive through the
synchronous run. Link this CLI entry point with `portals::lyra`; the gallery's
form itself uses the core scene API. See [command-line startup](cli.md).

```text
portals_gallery --theme nord --ansi16
portals_gallery --help
```

Include `<portals/ui_style.hpp>` for style stages. A theme supplies the theme layer.
Add stable application declarations with
`scene | ui::style(add, "* { bold: true; }")`; subsequent theme changes preserve
application overrides. An explicit file source uses `ui::file(path)` with the same
style stage. Read [style layers and precedence](style-layers.md), including checked
failure behavior and layer limits, before adding multiple overrides.

Current PSS rules set concrete declarations. Runtime `$name`, `theme.text`,
content-size expressions and importing arbitrary external theme formats are
planned work; these examples do not pretend those bindings already exist.
Colors and supported attributes can be customized. A general effects system,
shadow engine and arbitrary border styles are not supplied by this catalogue.

## Compact controls

[The framed form](../examples/compact/main.cpp) combines `ui::border`, `ui::gauge`,
`ui::plainButton` and `.width(24)`. Its late click handler appends an undoable
text change; the exact-ref stylesheet affects only its Add button. F10 or Ctrl+C
requests ordinary runner cleanup. It has no parser, transport emulator or local
widget implementation. See [compact primitives](compact-primitives.md) for
complete method and pipe forms, clipping and checked limits.

The dashboard uses `.flexX()` for its editor and `.flexY(2)` / `.flexY()` for
the table and chart. These declarations use the existing layout solver; the
provider and model lifetimes remain separate from UI composition.

## Dashboard data and loading

```text
portals_dashboard --rate 10000 --fps 30 --rows 1000000 --seed 42
portals_dashboard --rate 0 --delay-ms 250 --ansi16 --ascii
portals_dashboard --mode replay --duration-ms 5000
```

Read [the UI composition](../examples/dashboard/dashboard.hpp) separately from
[the provider](../examples/dashboard/provider.hpp). A recorded price source is
an ordinary provider mode; it is not a replay terminal or another event loop.
The provider owns bounded page requests, retry and cancellation. Its shutdown
runs before the scene retires the model. Virtual tables and charts share managed
preparation and use layout-derived extents.
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
The separate finite dashboard check prints cache hit/miss page probes, useful/cancelled jobs and page/job/byte
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
side. No million-row materialization is performed. The title and editable note exercise ordinary text controls. Dashboard candles use one-cell Unicode wicks and filled bodies
by default; `--ascii` selects ASCII candle brushes; the title already uses ASCII. `--ansi16` selects
actual ANSI16 encoding, not an advertised capability flag. Both themes are
compiled typed equivalents of the distributed PSS themes and require no runtime
theme-file lookup. Themes change controls, table colors and the chart background;
rising/falling market colors remain distinct.

Tab switches focus, arrows/Home/End select/scroll the table, Enter commits the
note, and F2 switches Modern/Turbo. The Inspect button deliberately sets the note
to its existing value. With the test-owned diagnostic launcher (`portals_dashboard_check --rate 0 --trace trace.json`), focus Inspect and press
Enter: the metadata trace distinguishes handler return, `value_unchanged`, and
`frame_not_requested`. Text values and terminal bytes are not exported. File errors
are reported after terminal restoration, independently of a prior run failure.

`--duration-ms` stops an interactive run after at most the next provider service
opportunity (up to 3600000 ms); zero leaves it interactive. Output/host stalls can
still extend real cleanup, as documented by `TerminalSession`. A separate test harness replays finite keyboard/resize input and prints frame/tick/byte/queue totals after close.

The gallery and custom widget entry points use scoped `ui::quit()` and `ui::runApp`.
The runner reports typed setup and runtime failures after attempting terminal cleanup.
Their scene objects also provide typed `run()` for headless replay.

## Customize a final widget

The custom-widget entry point uses the same application pipeline as the gallery.
`meter_example::meter(ui::opts(options))` returns an owning scene. The implementation
shows the additional work needed for a new widget: retained presentation,
state-backed input, preparation, measurement and painting. Read these files after
the first control examples.

\include{lineno} custom_widget/main.cpp

The native editor and dashboard also use Lyra preflight and the standard runner.
Their stationary application objects own the state borrowed by callbacks; keep
each object alive until its synchronous `runApp` call returns. The editor now
uses the standard runner exit policy: a successful typed run returns zero while
recoverable diagnostics are still reported. Fatal setup, input or cleanup
failures return one.

## Verification and complete advanced specimens

Finite harnesses are in `tests/examples/`; low-level before specimens and shared
test terminals are in `tests/support/`. They are built separately from interactive
programs. The installed-package consumer compiles the public sources and executes
finite contracts against the installed SDK. Offline staging includes their test
support as well as the public source.

See [compact API examples](compact-examples.md),
[low-level versus managed composition](declarative-ui.md),
[virtual preparation](virtual-preparation.md), and
[performance measurements](managed-performance.md) for complete advanced consumers.
Passing a headless replay does not establish native terminal appearance or
restoration under every host.

## Guide examples

The small programs in these guides live in `docs/examples`. From the Portals source root:

```powershell
cmake -S docs/examples -B build/guide-examples -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/guide-examples
```

Run `build/guide-examples/docs_basic.exe` on Windows or `build/guide-examples/docs_basic` on Linux/macOS. F10 closes interactive programs. See the [editor history demonstration](editor-history-demo.md) for editing scenarios.

## Browser sample

[Gallery and Level in WebAssembly](wasm-gallery.md) are opt-in experimental browser scenes. They use xterm.js and the incremental runner; building native examples does not build this target.

The [CheckBox with three states example](../examples/check_state/main.cpp) uses
an explicit input cycle and the standard terminal runner. Its optional
[group update helper](../examples/check_state/group.hpp) preserves partial failures.
