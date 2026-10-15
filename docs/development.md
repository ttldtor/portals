# Developing Portals {#development}

[Русская версия](ru/development.md)

This chapter is for library contributors. To write an application instead,
start with the [documentation guide](index.md).

## Choose a developer tool

The [tool catalog](developer-tools.md) lists entry points by task, with complete
parameter references for build/CI/documentation, measurements, packaging and
releases. It also identifies internal helpers and private workspace scripts.

## Verify changes

Use [headless testing](testing.md) for the normal test suite, naming checks
and terminal fixtures. [Fuzzing](../fuzz/README.md) describes the bounded input
and text targets and sanitizer setup. Automated checks must finish without
requiring someone to dismiss a window.

## Evaluate changes to the API

[Source comparison tool](compactness.md) and [quality measurements](quality.md)
are contributor references.

[Managed API measurements](managed-performance.md) records equivalent workloads,
raw samples and known scaling limits. [Design scenarios](api-design.md) are exploratory material, not the entry
point for current applications.

## Maintain the documentation

### Pull request checks

CI selects `docs` when every changed file is a recognized documentation input:
Markdown below `docs/`, images below `docs/images/`, `README.md`, `ReleaseNotes.md`,
or the documentation layout, stylesheet and Mermaid configuration. It builds
strict Doxygen HTML and checks local links and anchors in both modes.

All other changes select `full`, which also runs the naming checker and the
Windows/Linux build, test and standalone-consumer jobs. This includes headers,
included example sources, build scripts, CI tools and unknown paths. Deletions
and both paths of a rename participate in selection. An empty diff selects `full`.

`CI result` runs after all jobs, including when a dependency fails. It rejects
failed selection and any selected job that fails, is cancelled or is skipped.
Repository branch protection must require `CI result`; requiring only the
conditional build jobs does not enforce the documentation check. When adopting
this workflow, add that required check after its first run. PR checks do not
publish Pages or create releases. Markdown code blocks are rendered, but are
not automatically compiled by the documentation job.

Run the selection and gate regression tests locally with:

```powershell
python -m unittest discover -s tools/ci -p test_checks.py -v
```

### Documentation content

Keep the [short library guide](index.md), [reference map](reference-map.md) and `docs/DoxygenLayout.xml` in agreement.
The short guide describes current usage; release notes, design sketches and dated
measurements keep their historical scope and link back to current contracts.
Use relative Markdown links that resolve in the checkout. When embedding a
compiled example with a Doxygen command, also provide a source link for GitHub
readers. Build `portals_docs` and run `tools/docs/check_links.py` as described in
[getting started](getting-started.md). Inspect the generated navigation as well
as checking links: a valid link does not establish a useful reading order.

Use fenced `mermaid` blocks for flow and ownership diagrams. The same Markdown
renders on GitHub and in Doxygen; do not maintain a separate hand-aligned ASCII
copy. Give each diagram an `accTitle` and an `accDescr`, keep labels short, and
prefer vertical flow or several smaller diagrams to one very wide graph.

The documentation build requires Mermaid CLI and renders SVG locally. Its
shared theme is configured in `docs/mermaid.json`. Diagrams scale down to the page width. The stylesheet adapts their SVG palette
to the active light or dark theme.
Check the generated images as well as parser success: valid Mermaid can still
produce an unreadable layout. Existing architecture SVG illustrations remain
ordinary image assets.

## Licenses and provenance

Portals uses the Boost Software License 1.0 (`LICENSE.txt` in the repository root).
The [utf8proc provenance](../vendor/utf8proc/README.md) and
[license](../vendor/utf8proc/LICENSE.md) cover the private Unicode dependency.
The [Textual notice](../vendor/textual/README.md) identifies the adapted palette
colors and their MIT license. These notices also belong in distributed help
and packages.


User guides distinguish the implemented baseline from accepted direction and
proposed syntax. Mark every unavailable feature **TBD**, with its milestone and
owning task; an unscheduled importer or early slice keeps its schedule undecided.
Maintain English/Russian guide pairs together. Experimental compile-time APIs
have a separate reading path. New native examples show application code;
replay/check/stress fixtures remain test-owned. A candidate sketch must never be
presented as a compiling example or shipped API.
