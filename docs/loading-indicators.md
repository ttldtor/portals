# Show an animated loading indicator {#loading-indicators}

[Русская версия](ru/loading-indicators.md)

Use `ui::spinner()` for work whose completion percentage is unknown. The scene
owns its timer and cleanup. This complete program starts with a braille indicator;
Start and Stop control it without keeping a scheduler token:

[Complete source](examples/guide_loading_indicators.cpp).

\include{lineno} guide_loading_indicators.cpp

Build `portals_docs_loading_indicators` with tests enabled. Start is a no-op when
already active. Stop cancels the pending tick; starting again keeps the current
frame and waits one fresh interval. `Ref<Spinner>::start()`, `stop()` and
`label(...)` are checked UI-thread commands. Their failures enter the ordinary
scene error collector, so a simple button callback can ignore the returned
`expected`; callers needing an immediate result can inspect it explicitly.

## Select a sequence

| Preset | Appearance | Frames |
| --- | --- | --- |
| `SpinnerPreset::DOTS` (default) | `o..`, `.o.`, `..o` | 3 |
| `SpinnerPreset::LINE` | Rotating ASCII line | 4 |
| `SpinnerPreset::BRAILLE` | One-cell braille positions | 4 |
| `SpinnerPreset::BAR_BOUNCE` | One marker moving across five cells and back | 8 |

Set `.preset(...)`, `.interval(150)`, `.direction(SpinnerDirection::REVERSE)`,
or `.alignment(LayoutAlign::CENTER)` on the recipe. Equivalent pipe modifiers
use the same names. The interval is positive monotonic milliseconds; its default
is 100. Horizontal alignment supports START, CENTER and END. Width and height
use ordinary terminal-cell sizing and clipping.

For an owning custom single-row sequence, use `.frames("[  ]\n[= ]\n[==]")`.
LF separates complete frames. The source cannot contain empty frames, carriage
returns, terminal controls or a zero-width frame. Custom sources must be valid
under the receiving width policy. Built-in braille uses ASCII when its glyphs
cannot occupy one cell. A nonempty custom sequence overrides the preset.

The configurable defaults are 64 frames, 4096 total UTF-8 source bytes, 64 cells
per frame and 16384 allocated paint cells. The corresponding methods are
`.maxFrames(...)`, `.maxFrameBytes(...)`, `.maxFrameCells(...)` and
`.maxPaintCells(...)`. Invalid settings produce typed preparation errors.

## Labels, styles and resources

`.label(...)` accepts owning literal, formatted or localized `TextValue` input.
The label follows the sequence with one space and contributes to its intrinsic
width. `ui::spinnerLabel(...)` is the pipe form; `ui::label(...)` still creates
an ordinary Label. Runtime `loading.label(...)` stages a checked replacement;
it resets the animation to frame zero and restarts its interval. Localization
uses the explicitly supplied resolver, as for the other text-reference commands.
There is no implicit live locale or model subscription.

The default style type is `Spinner`. Ordinary foreground/background, bold,
italic and underline rules use the current stylesheet and theme layers. Manual
`ControlStyle` supplies the active and dim appearances; inactive and reduced-motion
presentation use the disabled variant. Independent colors for trailing segments
and multi-row animation shapes are not provided by this API.

Configuration resources use the same settings:

```text
scene {
  Spinner#loading { preset=BRAILLE label="Loading quotes" interval=100 width=30 }
}
```

`preset`, `frames`, `interval`, `direction`, `alignment`, `active`, `reducedMotion`,
`label`, `maxFrames`, `maxFrameBytes`, `maxFrameCells` and `maxPaintCells` are
canonical leaves. They compose with the existing common size, frame, identity
and style properties. A label can use the existing `i18n(...)` resource resolver.

## Animation lifetime

The managed indicator uses the existing scheduler and monotonic clock. It has
at most one pending timer. Hidden, fully clipped, disabled, inactive and removed
indicators do not continuously request frames. Layout and ordinary mutations
resume eligible animation; the indicator does not poll for visibility. Resume
does not replay missed ticks. `.reducedMotion(true)` renders frame zero without
a timer. Closing the scene cancels its timer registrations.

A standalone low-level Spinner remains a static retained widget; managed scene
construction supplies animation. Existing session constraints still apply:
destroy the ManagedScene outside callbacks and tree frames, and keep the clock
reader nonblocking and non-reentrant. Removing a widget from a supported callback
does not require destroying its scene. See [managed lifecycle](managed-controls.md).
