# Render a frame for your host loop {#render}

[Русская версия](ru/render.md)

Use [`renderFullFrame`](../include/portals/render.hpp) to turn your own text/fill
commands into an owning grid of terminal cells. This is useful for a host loop,
a terminal game's score/world layer, or a custom presentation backend that already
owns input and timing. For a control-based application, start with
[Application](application.md), which already lays out and paints the widget tree.

The complete program constructs a score frame. It performs no terminal I/O;
the returned `CellBuffer` is ready for the host's presentation code.

```cpp
#include <portals/render.hpp>

#include <array>

int main() {
  using namespace portals;

  const CellSize viewport{20, 2};
  auto text = GraphemeText::from("Score: 10");
  auto clip = CellRect::from({0, 0}, viewport);

  if (!text || !clip) {
    return 1;
  }

  const std::array<RenderCommand, 1> commands{
    TextCommand{{1, 0}, *text, *clip}};
  auto frame = renderFullFrame(viewport, commands);

  if (!frame) {
    return 1;
  }

  // The owning frame is ready for the host's presentation code.
  return 0;
}
```

Build the [source](examples/guide_render.cpp) with `portals::core` using
[package setup](package.md). The viewport is measured in terminal cells. Keep
text and commands alive until `renderFullFrame` returns; the resulting frame owns
its display bytes/styles and can outlive both inputs.

## Connect the frame to a game or embedding loop

For each changed state, assemble commands with a common width policy and call
`renderFullFrame(viewport, commands)`. Check the result before presenting it.
A new call begins with a blank buffer, so include the complete desired scene.
Your host owns the update cadence, input, simulation and resize handling.

To present in a terminal, pass the owning buffer to `FrameWriter::submit`, then
service `flush` outside rendering; see [diff and output](diff.md) and
[terminal transport](terminal.md). While output is pending, retain its transaction
and wait before submitting another frame. `FrameWriter` manages partial transport
progress and advances committed state only after consistent complete acceptance.
For another backend, resolve buffer cells through [`CellBuffer`](cell-buffer.md)
and map their glyph/style/width to your backend's drawing primitives. Portals
supplies cell content, not a pixel renderer.

Rendering a widget tree inside an existing loop additionally requires UI scheduler
dispatch, checked tree frames, layout and consumption of borrowed paint commands
before the frame ends; the [`Application` implementation](../src/runtime/application.cpp)
is the integration reference. Avoid nesting `Application::run` inside your host loop.
Dedicated game viewport/camera and bounded surface-snapshot publication are
**TBD — M6**; screen-space HUD/free placement has no separate assigned slice.
See [planned features](planned-features.md).

<details>
<summary>Renderer reference: ordering, clipping, ownership, bounds and errors</summary>

**Commands and ordering**

A `RenderCommand` is a `TextCommand` or `FillCommand`. All geometry uses the
[checked half-open cell geometry](geometry.md). Smaller signed `z` values paint
first; equal values preserve the supplied sequence order. Input order is never
mutated. There are no implicit layers or retained scene state.

`TextCommand` contains an origin, a reference to validated `GraphemeText`, an
explicit clip, a `CellStyle` and a z value. It paints one horizontal run. Each
cluster advances by its measured width even when clipped. Controls and unsupported
clusters display the text component's single-cell `?` placeholder; newline and
tab do not move the cursor. Width policy, including its revision, must match the
frame policy for every run, even an empty or offscreen run.

`FillCommand` paints styled ASCII spaces in `area ∩ clip ∩ surface`. These are
written lead cells, including with default style; untouched cells remain the
buffer's default blank cells. Empty fills do no cell work.


Construct text once
under the chosen frame policy; parsing Unicode during widget paint is unnecessary.

**Whole clusters and clipping**

New graphemes must fit completely in both clip and surface. Neither original
UTF-8 nor wide-cell pairs are split at either boundary. Negative origins are valid;
text advancement uses `int64_t`, so adding a run's measured width to an extreme
`int32_t` origin cannot wrap. A coordinate is narrowed only after its complete
footprint fits the nonnegative surface.

Overlays follow [CellBuffer's atomic old-cluster rule](cell-buffer.md): touching
an old continuation removes its lead too. For example, a wide glyph at columns
0–1 followed by a clipped narrow write at column 1 yields blank at 0 and the new
glyph at 1. Cleanup can cross the overlay clip. This deliberate consequence
preserves complete old clusters instead of leaving orphan halves. A fill has the
same rule, including when its cells intersect two different old wide clusters.

**Ownership, errors and thread use**

The scene span and text references are borrowed only until the call returns.
Keep them alive and unchanged for that duration. The renderer retains no pointer,
mutates no input and triggers no callback. The returned buffer owns every display
byte and style; destroying the scene or text does not affect it. Subsequent views
follow `CellBuffer`'s documented mutation/move invalidation rules.

There is no terminal or UI thread affinity. Independent calls and concurrent
reads of unchanged inputs are safe; synchronizing input mutation is the caller's
responsibility. UI scheduling and coherent model/style revisions remain outside
this component. Width-policy changes require the caller to rebuild text and
invalidate relevant layout/paint.

`RenderLimits` bounds command count, aggregate original text bytes and work units.
A repeated run consumes its bytes again. Work means all input text graphemes,
including clipped runs, plus visible fill cells; it is not a CPU-time measurement.
Sorting is bounded by command count, and intern lookup costs remain bounded by
the explicit buffer caps. Limits reject a whole scene without truncation, retry
or a partial result. Zero scene caps accept only inputs with zero counted cost.

Scene preflight checks sums/products before allocation or overflowing arithmetic.
The buffer's extent/pool/width-policy failures are returned as `buffer_error` with
the underlying `CellBufferError`. Renderer temporary storage failure becomes
`ALLOCATION_FAILURE`. No failure changes a previous frame, because no previous
frame is supplied. There is no output encoding or terminal I/O here.


</details>
