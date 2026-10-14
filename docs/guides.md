# User guides {#user-guides}

[Русская версия](ru/guides.md)

Start with the declarative API. Choose a task below, follow a small example,
then use the reference chapter for exact limits. These guides describe the
published API baseline; future features are marked **TBD**.

## Contents

| Task | Guide | Detailed contract |
|---|---|---|
| Compose and run a form | [Short library guide](index.md), [declarative composition](declarative-ui.md) | [Application](application.md) |
| Prepare a stylesheet or load a PSS file | [Styling guide](styling-guide.md) | [PSS syntax](styles.md), [layers](style-layers.md) |
| Choose, customize and update a theme | [Themes guide](themes-guide.md) | [Typed parameters](compile-time-styles.md) |
| Review experimental compile-time styles | [Compile-time styles](compile-time-styles.md), [compile-time PSS](compile-time-pss.md) | Separate from runtime PSS; review both frontends before choosing |
| Connect state, references and event handlers | [Bindings and events](bindings-guide.md) | [Properties](property.md), [text references](text-property.md), [input](input.md) |
| Load a scene from text or a file | [Scene resource loading](scene-loading-guide.md) | **TBD — M1 / U84** |
| Arrange controls and manage focus | [Layout](layout.md), [grid/overlay](grid.md), [focus](focus.md) | [Managed lifecycle](managed-controls.md) |
| Display changing or large datasets | [Models](model.md), [virtual preparation](virtual-preparation.md), [lists](virtual-list.md), [tables](virtual-table.md) | [Queries](model-queries.md) |
| Handle failures and restore the terminal | [Declarative reporting](declarative-ui.md), [diagnostics](diagnostics.md) | [Terminal session](terminal.md), [limitations](limitations.md) |
| Handle text, Unicode, edits and Undo | [Text properties](text-property.md), [LineEdit](line-edit.md), [editor history](editor-history.md) | [Cell width](text.md) |
| Await work without blocking UI | [UI coroutines](ui-coroutines.md) | [Scheduler](scheduler.md) |
| Install, test or debug a client | [Package](package.md), [testing](testing.md), [terminal probe](terminal-probe.md) | [Development](development.md) |

## Example ladder

- **Small:** one complete form with a control, one action and an exit key.
  Start with the [short guide](index.md).
- **Focused:** state changes, styles and an explicit failure path. See the
  [click-handler client](../examples/click_handlers/main.cpp) and the
  [declarative guide](declarative-ui.md).
- **Integrated:** multiple controls, model updates and owned application state.
  See the [gallery](examples.md) and [dashboard](../examples/dashboard/dashboard.hpp).

Each new guide should include a complete runnable source link, its headers and
build/run instructions; fragments must say which objects they assume.
Examples should cover successful use and at least one relevant failure.
Current complete sources and build instructions are indexed in [examples](examples.md).

## Planned features and milestones

**TBD** means planned, not available in this documentation baseline. A milestone
is an ordering target, not a release date or evidence that implementation passed.
The feature's guide must be updated alongside its implementation; the final
cookbook sweep is **TBD — M7 / U96**.

| Feature | Status / milestone | Task |
|---|---|---|
| Current-scene callback context and compact quit | Available; U125 published in PR27 (milestone M1) | U125 |
| Named reference lookup | **TBD — M1** | U126 |
| Scoped styling by Ref and its file convenience | **TBD — M1**; global `ui::style(ui::file(...))` already exists | U127 |
| Scene text/file loader with allowlisted registry | **TBD — M1** | U84, after U126/U127 |
| Remaining text-source/model-view contracts | **TBD — M2**; existing TextValue/Ref are usable | U12 |
| Automatic one-way binding and observation facade | **TBD — M2** | U13, after U12 |
| Derived state and binding expressions | **TBD — M2** | U14, after U13 |
| PSS v1 selectors, parts and inheritance | **TBD — M5** | U64 |
| Expanded theme tokens and computed dependency graph | **TBD — M5**; current typed LiveTheme parameters already exist | U65, after U64/U61 |
| Bounded runtime style expressions | **TBD — M5** | U128, after U84 |
| Canonical PSS themes, common defaults and generated embedded resources | **TBD — M5** | U65 |
| Import external theme formats into PSS | **TBD**; separate slice and schedule not assigned | U65 direction; formats are candidates |
| Code-provided `$name`, checked assignment and localized `this.text` sizing | **TBD — M5** | U65/U128 |
| `onLayout` and checked arranged-geometry reads through Ref | **TBD — M1** | U130 |
| `ui::async` for functions and coroutine lambdas with `co_await` | **TBD — M6**; early slice proposed, not scheduled | U83 |
| Game viewport/camera and bounded surface snapshot publication | **TBD — M6** | U75/U77 |
| Screen-space HUD, free placement and manual/auto switching | **TBD**; overlay foundation M2, surface M6; separate slice not assigned | U22/U75; cross-run persistence U80/M6 |
| Complete cookbook/gallery/documentation sweep | **TBD — M7** | U96 |

No proposed grammar on these rows is a callable API. Compile-time style design
has its own chapters and remains a separate review topic.

## Experimental compile-time alternatives

[Typed styles](compile-time-styles.md) and [compile-time PSS](compile-time-pss.md)
are already implemented, but form a separate experimental reading path.
Ordinary application examples start with a declarative scene and runtime PSS.
New native examples should separate application code from replay/check/stress
fixtures. Reorganizing the existing samples is **TBD — M1/U11 (EXAMPLE-001)**;
proposed target directories are not presented as existing runnable programs.

## Subject index

- **Binding:** [bindings guide](bindings-guide.md), [properties](property.md).
- **Cascade, CSS/QSS differences:** [styling](styling-guide.md), [layers](style-layers.md).
- **Computed values:** [typed live parameters](compile-time-styles.md); future
  general binding/style expressions are listed above.
- **Errors, reporting:** [diagnostics](diagnostics.md), [declarative runner](declarative-ui.md).
- **Events, parameters, propagation:** [bindings/events](bindings-guide.md), [input](input.md).
- **Files:** [style files](styling-guide.md); [scene files — TBD](scene-loading-guide.md).
- **Focus, keyboard phases:** [focus](focus.md), [input](input.md).
- **Lifetime, owning versus observing:** [bindings guide](bindings-guide.md), [managed controls](managed-controls.md).
- **PSS, selectors, states:** [styling guide](styling-guide.md), [syntax contract](styles.md).
- **Ref, snapshots, tokens:** [glossary](glossary.md).
- **Theme switching:** [themes guide](themes-guide.md).

[Glossary](glossary.md) defines the terms. The [reference map](reference-map.md)
provides the complete technical chapter list.
