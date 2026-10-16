# Documentation contents {#reference-map}

[Русская версия](ru/reference-map.md) · [Short guide](index.md) · [Task guides](guides.md) · [Glossary](glossary.md)

Follow the [first application](first-application.md) to build a note form in four
steps. For a particular change, choose a [task guide](guides.md). The sections
below collect the detailed guides and API contracts.

## Start here

[Short guide](index.md) · [First application](first-application.md) · [Task guides](guides.md) · [Glossary](glossary.md)

[Runnable examples](examples.md), [frames, gauges and sizing](compact-primitives.md),
and the [editor history example](editor-history-demo.md) provide complete programs.
[Text formatting and columns](compact-examples.md) explains the shorter text and table API.
[Package setup](package.md) covers SDK installation and toolchain compatibility;
[building Portals and its examples](getting-started.md) is for working with the library sources.
Read [current limitations](limitations.md) for qualified platforms and runtime boundaries.

## Application API

- [Declarative composition](declarative-ui.md), [scene and recipe reference](declarative-reference.md), [application runner](application.md), [managed controls](managed-controls.md).
- [Command-line startup](cli.md): parse arguments before opening the terminal and read process snapshots.
- [Bindings and events](bindings-guide.md), [properties](property.md), [control references](text-property.md).
- [Input](input.md), [focus and routing](focus.md), [diagnostics](diagnostics.md).

## Styles and themes

[Styling guide](styling-guide.md) · [Themes guide](themes-guide.md) · [PSS syntax](styles.md) · [Style layers](style-layers.md)

Use style layers when palette changes must preserve application and user overrides.

## Controls and data

- [Basic controls](controls.md), [LineEdit](line-edit.md), [text history](text-history.md), [editor undo and redo](editor-history.md).
- [Layout](layout.md), [grid and overlay](grid.md), [scrolling](scroll.md), [charts](chart.md).
- [Immutable models](model.md), [bounded queries](model-queries.md), [layout-driven view preparation](virtual-preparation.md).
- [Virtual lists](virtual-list.md), [virtual tables](virtual-table.md).
- [UI coroutines](ui-coroutines.md), [scheduler](scheduler.md).

## Library internals

- [Architecture and event-flow diagrams](architecture.md), [widget ownership](widget-tree.md), [geometry](geometry.md).
- [UTF-8 and cell width](text.md), [cell buffer](cell-buffer.md), [rendering](render.md), [frame diff and output](diff.md).
- [Terminal session](terminal.md), [terminal probe and observed capabilities](terminal-probe.md).
- [Legacy compatibility](legacy.md): the supported path for the existing streaming MarketDepthModelSample.

## API reference

In generated HTML, use **API reference** to find types, concepts, namespaces,
members and headers. For example, [ui::Scene](../include/portals/ui_scene.hpp)
and [WidgetTree](../include/portals/widget.hpp) link to type contracts in HTML
and to their public headers in the repository. Ordinary application code starts
with `ui.hpp`, `managed.hpp` and `application.hpp`.

Experimental alternatives have separate contracts:
[compile-time scenes](compile-time-scenes.md), [typed compile-time styles](compile-time-styles.md)
and [compile-time PSS](compile-time-pss.md). Applications can use declarative
composition and runtime PSS without these alternatives.

## Plans and development

[Planned features and milestones](planned-features.md) · [Scene loading — TBD](scene-loading-guide.md)

- [Contributor guide](development.md), [developer tools](developer-tools.md), [testing](testing.md), [fuzzing](../fuzz/README.md).
- [Managed API measurements](managed-performance.md): reproducible costs and known scaling limits. Results describe the measured workloads.
- [API design scenarios](api-design.md): exploratory material; check current contracts before using a proposal.
- [Releasing](releasing.md), [release notes](../ReleaseNotes.md), [dependencies and licenses](dependencies.md).
- Dependency provenance: [utf8proc](../vendor/utf8proc/README.md), [Textual palette attribution](../vendor/textual/README.md), [test-only doctest](../vendor/doctest/README.md).

## Reading on GitHub and as HTML

Relative Markdown links work in a checkout and on GitHub. Complete example
sections link to real source files. Doxygen `\include` commands expand into
numbered listings in HTML; on GitHub, use the accompanying source link. Labels
such as `{#examples}` are Doxygen identifiers.

Markdown describes the selected source branch. The [published HTML](https://ttldtor.github.io/portals/)
follows the latest stable release; PR checks provide a preview artifact.
HTML adds structured navigation, search and API links to these same guides.
Its generated directory also works offline. Language links at the top of each
translated page lead to the same topic. Links marked **English** lead to material
that has no Russian translation yet. See [translation maintenance](development.md#documentation-translations)
for source revisions and update rules.
