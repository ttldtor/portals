# Planned features {#planned-features}

[Русская версия](ru/planned-features.md)

**TBD** marks planned capabilities. Milestones describe delivery order;
calendar dates are not assigned here. See [user guides](guides.md) for current API.

## Milestones

Available capabilities refer to the current source branch; this does not
identify a released version.

| Feature | Status / milestone |
|---|---|
| Named reference lookup | Available — M1; [named references](declarative-ui.md) |
| Scoped styling by Ref and its file convenience | Available — M1; [reference styles and files](declarative-ui.md) |
| Scene text/file loader with allowlisted registry | Available — M1; [syntax and examples](scene-loading-guide.md) |
| Text sources and shared caption state | Available; [shared state](bindings-guide.md) and [read-only sources](text-property.md) |
| Managed state observation | Available; [latest-value delivery](bindings-guide.md#managed-observation) |
| Derived state and binding expressions | Available for [unary managed sources](bindings-guide.md#managed-derivation); live text/list adapters remain planned |
| PSS v1 selectors, parts and inheritance | **TBD — M5** |
| Expanded theme tokens and computed dependency graph | **TBD — M5**; current typed LiveTheme parameters already exist |
| Bounded runtime style expressions | **TBD — M5** |
| Canonical PSS themes, common defaults and generated embedded resources | **TBD — M5** |
| Import external theme formats into PSS | **TBD**; separate slice and schedule not assigned |
| Code-provided `$name`, checked assignment and localized `this.text` sizing | **TBD — M5** |
| `onLayout` and checked arranged-geometry reads through Ref | Available — M1; [layout observation](layout.md) |
| `ui::async` for functions and coroutine lambdas with `co_await` | **TBD — M6** |
| Game viewport/camera and bounded surface snapshot publication | **TBD — M6** |
| Screen-space HUD, free placement and manual/auto switching | **TBD**; overlay foundation M2, surface M6; separate slice not assigned |
| UI state persistence across application runs | **TBD — M6** |
| Kitty keyboard protocol: detection, mode restoration, modifier keys and diagnostics | **TBD — M7** |
| Complete cookbook/gallery/documentation sweep | **TBD — M7** |

The reorganized [examples](examples.md) are available in the current source branch (M1).
The design details below remain proposals. Runtime PSS v0 currently accepts literal values.

## Style parameters

<a id="planned-style-parameters"></a>

**TBD — M5:** typed runtime parameters, expressions, source diagnostics and
dependency-driven updates. Scoped style sources through `Ref` are already
available; see [reference styles](declarative-ui.md).

| Proposed form | Meaning |
|---|---|
| `$name` | A typed value supplied or observed from application code |
| `theme.text` | A semantic value from the active theme |
| `this.text` | The current control's resolved property |

Names are resolved in their own namespaces. A fallback from `$text` to
`theme.text` must be explicit. `theme` and `this` use no dollar prefix.

Proposed PSS syntax — **TBD — M5**:

```css
/* Proposed syntax; runtime PSS v0 does not accept it. */
Button {
  foreground: $text;
  background: theme.surface;
  min-width: len(this.text) + 2;
}
```

`this.text` contains formatted, localized text. `len` in size expressions
measures terminal cells with the layout's `WidthPolicy`. Language and text
argument changes invalidate dependent measurements. Ordinary buttons retain
intrinsic sizing; formulas customize constraints. Border and padding are counted
once. Percentage bases, ranges and rounding remain undecided. CSS length units
such as `px`, `em`, `vh` and `vw` have no agreed support.

### Updating parameters

Proposed API — **TBD — M5**; the return type and explicit-result overload remain open:

```cpp
// Proposed API; not currently compilable.
ui::currentScene.setParam("width", value);
```

Assignment is intended to validate all affected consumers before publishing
the update atomically. Rejection preserves the engine's parameters and computed
styles and reports a typed error. External property/model changes and callback
side effects remain the caller's responsibility.

Parameters belong to the scene's upper runtime override context, with theme
values available. Explicit overrides survive theme changes; affected dependencies
are recalculated. Assignment updates expressions that use the parameter.
Coordination with `USER` is **TBD — M5**; existing cascade priorities remain.
`currentScene` is restricted to its admitted context and must not retain an
owning global scene or use a previous scene outside that context.

An explicit optional policy may let an inapplicable data value fall through to
the next applicable lower declaration or default. Applicability is checked per
consumer. The default policy reports unbound parameters and rejects assignment
to unknown names. Syntax errors, unknown functions or fields, memory failures,
wrong-thread calls, closed scenes and lifecycle errors remain errors under the
optional policy.

`ui::Params`, `ui::params`, notification-driven `ui::bind(source, transform)`,
`??` and `grayScale` are possible spellings under review — **TBD — M5**.
Captured C++ variables need property/model notifications to be observed.
Updates use UI delivery and scene cleanup; typed values remain data.
Dependent colors request PAINT, sizes request LAYOUT, and unchanged results
avoid extra redraw. The existing experimental C++ tokens and `mix` are covered in
[compile-time styles](compile-time-styles.md).

## Themes

<a id="planned-themes"></a>

**TBD — M5:** built-in themes will use a single editable PSS source.
Generated C++ materialization and embedded resources will derive from it,
allowing applications to run without theme files in their working directory.
The existing factories remain available in the documented version.

Theme loading will validate the stylesheet and atomically replace `THEME`,
preserving `APPLICATION` and `USER`. It uses the existing style resolver.
Common control/state rules will use typed semantic parameters with complete
light and dark defaults. Themes can override parameters and rules.
Automatic system light/dark detection has no finalized contract or separate schedule.

```text
PSS source -> embedded resource or file -> validation -> THEME replacement
external theme -> importer -> normalized PSS ---------^
```

### External formats

Importers are **TBD**, with a separate milestone and budget still to be assigned.
They will convert supported fields to PSS parameters and rules. External parsers
remain outside required core dependencies. Code-based theme imports must not execute code.

Windows Terminal color-scheme JSON and Base16/Tinted YAML are proposed first formats.
Each importer needs a format version, field mapping, defaults, unsupported-field
description and source/license notices. ANSI palettes need explicit, configurable
mappings for control roles, selection, errors and domain colors such as bid/ask.

Runtime PSS expressions remain **TBD — M5**. The existing typed `LiveThemeRef`
requires explicit UI-thread updates for domain property changes.
