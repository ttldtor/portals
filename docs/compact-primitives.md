# Compact frames, gauges and sizing

[Русская версия](ru/compact-primitives.md)

Build a small retained scene with standard widgets. The button uses the existing
Button/action/router path with caption-only chrome; Enter, Space and a left press
activate it. F10 also exits. No client widget subclass or polling loop is needed.

## Ordinary composition

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  auto content = ui::vbox(ui::label("Transfer"),
    ui::gauge(.65).width(24),
    ui::plainButton("Exit", [] {
      ui::quit();
    }));
  auto scene = ui::scene(ui::hbox(
    ui::border(std::move(content)).width(30).height(7)));

  scene.onKey(key::F10, ui::quit);

  return scene.runApp();
}
```

## Pipe composition

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  auto scene = ui::scene(ui::hbox(
    ui::vbox(
      ui::label("Transfer"),
      ui::gauge(.65).width(24),
      ui::plainButton("Exit", [] {
        ui::quit();
      }))
      | ui::border()
      | ui::width(30)
      | ui::height(7)));

  return scene
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
}
```

## Size and flex policy

`width(cells)` and `height(cells)` replace only the selected axis with fixed
nonnegative bounds. `flexX(weight)`, `flexY(weight)` and `flex(weight)` select the
existing flex mode with a positive weight and a zero preferred basis, preserving
minimum/maximum bounds. This shares the available space by weight even when a
child measures its entire offered viewport. Modifiers run in
order at mount: `width(10).flexX()` still has a ten-cell cap; `flex().height(3)`
keeps horizontal flex and fixes height. Negative dimensions and zero weights
produce `ManagedError` with `TreeError::INVALID_ARGUMENT`; the failed subtree
is removed and references remain unpublished. The same operations are available
as consuming methods and pipe stages. Parent layouts apply these policies;
the application root itself receives the viewport extent.

## Frame and gauge contracts

`border(child, options, style)` and its `frame` alias own one consuming child
recipe. They mount within the same transaction, including nested refs and child
errors. A one-cell inset is measured and placed by the existing LinearLayout.
The inner clip also constrains fixed/minimum-size overflow for painting and hit
testing. Empty allocations paint nothing. One-cell rows/columns collapse to
corners and the remaining edge; two-by-two frames contain four corners and no
content. Frame painting never clears or paints the inner content.

`gauge(value, options, style, policy)` is strictly determinate. Defaults are a
range of `[0, 1]`, ten-cell intrinsic width and one-cell intrinsic height. Bounds
must be finite with `minimum < maximum` and a finite representable span; values
must be finite and inside the inclusive range. Invalid values, NaN/infinity,
negative preferred widths and zero paint caps fail with `INVALID_ARGUMENT`.
The fill covers `floor(width * fraction)` columns across the allocated height.

Use `setGaugeValue(tree, node, value)` between frames: it validates before changing
state, requests only paint, and requests no redraw for an equal value. Inside an
already dirty `onPrepare` callback, `Gauge::display(value)` updates the snapshot
without requesting another frame. Neither operation creates a timer or animation.

`BorderOptions` and `GaugeOptions` select Unicode or ASCII glyphs and a positive
`maxPaintCells` cap (default 16384). Each decoration owns its GraphemeText with
the complete receiving WidthPolicy, including revision/emoji/ambiguous policy.
A Unicode decoration measuring more than one cell falls back to ASCII. A frame
checks its allocated perimeter, and a gauge its allocated area, before submitting
commands. Over-cap paint returns `WOULD_BLOCK`, even for a partially clipped
allocation; raise the explicit cap only when the surrounding render budgets also
permit it. Fully clipped nodes follow the existing tree skip behavior. The widget's
paint implementation allocates no heap storage. Submitted text stays widget-owned
through the enclosing tree frame; the renderer consumes it before that frame ends.

`ControlStyle` passed to these factories owns foreground/background and other
normal/focused/disabled values. These are manual defaults beneath theme rules,
not explicit theme overrides. For Border/Gauge these defaults are construction-only;
dynamic colors use the existing style resolver, including exact-Ref application
styles, so cached computed styles and theme precedence stay coherent. Border/Gauge participate in the existing style
resolver and disabled-ancestor synchronization. `plainButton` returns the same
`Button` as `button`, so existing refs, bindings, keyboard and pointer routing work;
only mandatory brackets are removed. Existing `button` defaults keep their brackets. Both factories map caption resource
limits to `NODE_LIMIT`, malformed UTF-8 to `INVALID_ARGUMENT`, and preserve the
original `TextError` payload and transactional cleanup.

## Scoped application style

This complete example gives the button owning constructor defaults and an exact-Ref
application style. `ui::style(ref, pss)` copies its source before deferred mounting;
its application layer overrides theme declarations and survives theme replacement.
The `*` selector still matches only the referenced node because the layer has an
exact node scope. Scoping does not change selector matching: a managed Button
automatically has type `Button` and also matches `Control` selectors. Explicit
metadata follows the [style target contract](styles.md).
An unsuccessful factory publishes neither its refs nor a partial scoped style;
the same Ref and owning style stage can be used in a fresh scene.

```cpp
#include <portals/ui_style.hpp>

int main() {
  using namespace portals;

  ui::Ref<Button> exit;
  ControlStyle defaults;
  defaults.normal.foreground = RgbColor{180, 180, 180};

  auto scene =
    ui::scene(ui::border(ui::vbox(ui::gauge(.65).width(24),
      ui::plainButton(
        "Exit",
        [] {
          ui::quit();
        },
        defaults)
        .reference(exit))));

  scene
    | ui::style(exit,
      "* { foreground: #ffffff; background: #203040; }");

  return scene
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
}
```

## Low-level extension and compatibility

Existing three-argument `WidgetTree::arrange` semantics are unchanged. The additive
four-argument overload intersects an extra surface `contentClip` into the node's
ordinary inherited/root clip; descendants inherit the resulting geometry clip.
`Layout::childClip` defaults to the full allocation, so existing Row/Column/Grid,
Overlay and custom layouts keep their allocation and clipping behavior. Border
uses the hook for its inner clip. Invalid custom clips and overflowing transforms
return typed geometry errors, and the caller discards the failed pass.

The added virtual hook and Button storage require rebuilding the SDK and consumers.
Source compatibility is preserved; compatibility with old object files is not promised.
See [declarative composition](declarative-ui.md) and [layout policies](layout.md).
