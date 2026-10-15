# User guides {#user-guides}

[Русская версия](ru/guides.md)

Start with a declarative scene, then choose the task you need.

## Guides by task

| Task | Guide |
|---|---|
| Build and run a first application | [Short guide](index.md) |
| Declare controls and event handlers | [Declarative UI](declarative-ui.md) |
| Parse CLI before opening the terminal and read process snapshots | [Command-line startup](cli.md) |
| Write PSS or load a stylesheet | [Styles](styling-guide.md) |
| Choose or update a palette | [Themes](themes-guide.md) |
| Connect state, references and events | [Bindings and events](bindings-guide.md) |
| Arrange controls and focus | [Layout](layout.md), [grid and overlay](grid.md), [focus](focus.md) |
| Work with text and editing | [Text references](text-property.md), [LineEdit](line-edit.md), [Undo and Redo](editor-history.md) |
| Display large datasets | [Models](model.md), [virtual lists](virtual-list.md), [virtual tables](virtual-table.md) |
| Report errors | [Diagnostics](diagnostics.md), [application runner](application.md) |
| Await work on the UI thread | [UI coroutines](ui-coroutines.md) |
| Use Portals in an existing project | [Package setup](package.md) |

## Examples

The [short guide](index.md) contains a complete small form.
The [click-handler example](../examples/click_handlers/main.cpp) shows state
changes and event handlers. The [gallery and dashboard](examples.md) combine
controls, models and application state. Build and run instructions are on the examples page.

## Reference and terminology

[Glossary](glossary.md) · [Reference map](reference-map.md) · [Current limitations](limitations.md)

For exact style rules, see [PSS syntax](styles.md) and [style layers](style-layers.md).
Experimental [compile-time styles](compile-time-styles.md) and
[compile-time PSS](compile-time-pss.md) have separate chapters.

## Planned capabilities

Scene loading from strings or files is **TBD — M1**; automatic binding is
**TBD — M2**; expanded PSS and theme parameters are **TBD — M5**.
See [planned features and milestones](planned-features.md) for the full list
and [scene loading](scene-loading-guide.md) for its current status.

[Dependencies and licenses](dependencies.md).
