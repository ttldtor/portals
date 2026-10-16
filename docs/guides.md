# User guides {#user-guides}

[Русская версия](ru/guides.md) · [Short guide](index.md) · [Documentation contents](reference-map.md) · [Glossary](glossary.md)

Start with a declarative scene, then choose the task you need.

Follow the [first application](first-application.md): a quick-note form in four steps.

## Guides by task

| Task | Guide |
|---|---|
| Build an application step by step | [First application](first-application.md) |
| Compose a scene | [Declarative UI](declarative-ui.md) |
| Start the application and parse options | [Application](application.md) |
| Parse command-line arguments | [Command-line startup](cli.md) |
| Apply styles | [Styles](styling-guide.md) |
| Change the palette | [Themes](themes-guide.md) |
| Connect events and state | [Bindings and events](bindings-guide.md) |
| Arrange controls | [Layout](layout.md) |
| Place controls in cells | [Grid and overlay](grid.md) |
| Change a control through a reference | [Control references](text-property.md) |
| Edit text | [LineEdit](line-edit.md) |
| Undo and redo edits | [Undo and redo](editor-history.md) |
| Create shared state | [Properties](property.md) |
| Supply view data | [Models](model.md) |
| Display a large list | [Virtual lists](virtual-list.md) |
| Display a large table | [Virtual tables](virtual-table.md) |
| Control scrolling | [Scrolling](scroll.md) |
| Plot values | [Charts](chart.md) |
| Handle keyboard input | [Input](input.md) |
| Report errors | [Diagnostics](diagnostics.md) |
| Wait without blocking the UI | [UI coroutines](ui-coroutines.md) |
| Integrate frame rendering | [Rendering](render.md) |
| Connect a CMake package | [Package setup](package.md) |

## Examples

The [short guide](index.md) contains a complete small form.
The [click-handler example](../examples/click_handlers/main.cpp) shows state
changes and event handlers. The [gallery and dashboard](examples.md) combine
controls, models and application state. Build and run instructions are on the examples page.

## Reference and terminology

[Glossary](glossary.md) · [Documentation contents](reference-map.md) · [Current limitations](limitations.md)

[Scene and recipe reference](declarative-reference.md) describes the exact scene API.

For exact style rules, see [PSS syntax](styles.md) and [style layers](style-layers.md).
Experimental [compile-time styles](compile-time-styles.md) and
[compile-time PSS](compile-time-pss.md) have separate chapters.

## Planned capabilities

Scene loading from strings or files is **TBD — M1**; automatic binding is
**TBD — M2**; expanded PSS and theme parameters are **TBD — M5**.
See [planned features and milestones](planned-features.md) for the full list
and [scene loading](scene-loading-guide.md) for its current status.

[Dependencies and licenses](dependencies.md).
