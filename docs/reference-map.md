# Reference map {#reference-map}

Start with the [short library guide](index.md). This page is the full topic map;
start with declarative composition, then read the relevant contract when a task needs it.

The [online documentation and API reference](https://ttldtor.github.io/portals/)
is generated from these guides and public headers.

[User guide contents and subject index](guides.md) · [Glossary](glossary.md)

## Build an application

| Need | Read |
| --- | --- |
| Compose a form and manage its lifetime | [Declarative UI](declarative-ui.md), [managed controls](managed-controls.md), [application runner](application.md) |
| Choose and arrange controls | [Basic controls](controls.md), [LineEdit](line-edit.md), [Row and Column](layout.md), [Grid and Overlay](grid.md) |
| Apply colors and themes | [Styles and themes](styles.md) |
| Await timer and provider results with UI-owned coroutines | [UI coroutines](ui-coroutines.md) |
| Connect state and data | [Properties](property.md), [immutable models](model.md), [bounded queries](model-queries.md) |
| Display large datasets | [Layout-driven preparation](virtual-preparation.md), [virtual lists](virtual-list.md), [virtual tables](virtual-table.md), [scrolling](scroll.md), [charts](chart.md) |
| Write shorter checked text and typed columns | [Compact examples](compact-examples.md) |
| Change captions and editor drafts through observing references | [Text properties](text-property.md) |
| Explain an action that did nothing | [Diagnostics](diagnostics.md), [terminal probe](terminal-probe.md) |
| Keep an existing streaming MarketDepthModelSample building | [Minimal legacy compatibility](legacy.md) |

For theme switching that preserves application and user overrides, see
[style layers](style-layers.md).

## Understand the core

Read [architecture](architecture.md) first for component and event-flow
diagrams. These chapters describe the retained core and its low-level contracts.

- Ownership and events: [widget tree](widget-tree.md), [focus and routing](focus.md),
  [scheduler](scheduler.md).
- Text and coordinates: [UTF-8 and cell width](text.md), [geometry](geometry.md).
- Owning low-level edit history: [text history](text-history.md).
- Undo editor text and configure read-only or history shortcuts: [editor history](editor-history.md).
- Try three editor confirmation modes: [editor history demo](editor-history-demo.md).
- Input and output: [input decoder](input.md), [terminal session](terminal.md),
  [cell buffer](cell-buffer.md), [renderer](render.md), [diff and output](diff.md).

## API reference

The generated HTML site has a separate **API reference** menu for types,
concepts, namespaces, members and headers. In a checkout, public contracts
live under `include/portals`; start with `ui.hpp`, `managed.hpp` and
`application.hpp` for ordinary application code. See
[getting started](getting-started.md) to build
and open the complete reference locally.

## Experimental compile-time alternatives

These implemented APIs have their own review path; ordinary applications start
with declarative composition and runtime PSS.

- [Compile-time scenes](compile-time-scenes.md): constant-evaluated structure.
- [Typed compile-time styles](compile-time-styles.md): typed expressions and live parameters.
- [Compile-time PSS](compile-time-pss.md): the same bounded PSS v0 string grammar.

## Develop and evaluate Portals

The [contributor guide](development.md) collects tools, experiments, documentation
maintenance and dependency notices.

See [releasing](releasing.md) for tags, release candidates, artifacts and
documentation deployment through GitHub Actions.

- [Testing](testing.md) and [fuzzing](../fuzz/README.md): correctness tools.
- [Managed API measurements](managed-performance.md): reproducible costs and
  known scaling limits, not a promise of linear performance.
- [API design scenarios](api-design.md) and the
  [module experiment](../experiments/module-spike/README.md): design/experiment
  material, not the recommended first application or a stable API contract.
- Dependency notices: [utf8proc](../vendor/utf8proc/README.md) and
  [Textual palette attribution](../vendor/textual/README.md).
  [Private doctest dependency](../vendor/doctest/README.md) is used only by tests.

## Reading on GitHub and as HTML

For supplemental engineering measurements, see [source compactness](compactness.md)
and [cognitive complexity and performance evidence](quality.md). These reports
support review; they do not replace behavioral tests or decide API acceptance.

This index and its relative Markdown links work in a checkout and on GitHub.
Complete example sections link to the real, compiled source files. Their
Doxygen `\include` lines expand into numbered listings in generated HTML;
GitHub shows those lines literally, so use the source links there. Header
labels such as `{#examples}` are Doxygen identifiers, not part of a title.

The HTML site adds structured navigation and API reference to the same guides.
It is generated into a build directory, not maintained as a second copy.
GitHub Actions publishes the generated site to the documentation URL linked above.
The generated directory also works as a local, offline reference.
