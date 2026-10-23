# Developing Portals {#development}

[Русская версия](ru/development.md)

This chapter is for library contributors. To write an application instead,
start with the [documentation guide](index.md).

## Choose a developer tool

The [tool catalog](developer-tools.md) lists entry points by task, with complete
parameter references for build/CI/documentation, measurements, packaging and
releases. It also identifies internal helpers and private workspace scripts.

## Verify changes

Use a task preflight to find failures early, then retain integration and milestone
qualification for the applicable scope. A local subset is never evidence that
the omitted runtime contracts passed.

| Level | Checks and evidence |
| --- | --- |
| Task | Explicit changed-contract regression/negative cases, format/static checks, relevant public consumer and docs; pure documentation uses strict docs/links rather than invented runtime targets |
| Integration | For runtime/API changes: MSVC and GCC Debug/Release presets, full configured CTest, installed/relocated and standalone consumers, naming/static checks, strict EN/RU docs; applicable sanitizer/lifetime checks |
| Milestone | Integration plus supported-platform native terminal smoke, comparable benchmarks and limitations, SDK qualification, delivered docs artifact/browser checks and independent review |

The [local check planner](developer-tools-build.md#tool-check-plan) provides an
auditable focus order from exact contracts and a real CTest inventory. Its bounded
pilot does not qualify runtime test exclusions: C++ changes remain full plans.
Only registered non-C++ Python fixtures may narrow task preflight. Added,
deleted, renamed, unknown or uncovered files fall back to full. Integration and
milestone obligations persist; unavailable checks stay NOT_RUN. The
existing PR policy and hosted MSVC/GCC/docs gates are unchanged.

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

The naming job caches downloaded Debian packages, not installed tools or check
results. It always refreshes signed APT metadata, validates restored archives
against its SHA256 values and installs the same toolchain. Missing, obsolete or
corrupt archives are downloaded again. The cache key includes the runner platform,
architecture and package policy fingerprint. Cache availability is optional;
all translation units are checked on both hits and misses. See the
[APT cache validator](developer-tools-build.md#tool-apt-cache).

`Warm naming package cache` prepares the default-branch cache shared with PRs.
It runs when its action, workflow or validator changes on `main`, or manually
through Actions with `main` selected. It installs only the toolchain and does not repeat the project
test matrix. Use manual dispatch to refresh an evicted cache or changed package
candidates. A cache created only in a PR is not shared with unrelated PRs.

### Documentation content

Keep the [short library guide](index.md), [documentation contents](reference-map.md) and `docs/DoxygenLayout.xml` in agreement.
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

<a id="documentation-translations"></a>
### Languages and translation updates

The English source is `docs/<topic>.md`; its Russian translation is
`docs/ru/<topic>.md`. The relative topic path is the logical page name.
Keep existing page IDs and links stable. Each translated page links to the same
topic in the other language. Mark a link to untranslated material with its
language, such as **English**; do not redirect it to an unrelated landing page.

The short guide provides the quick start and a few next steps. Keep the full
task catalog in [user guides](guides.md), and group detailed contracts in the
[documentation contents](reference-map.md). Keep shared links in the same order
in EN/RU, and register pages in `docs/DoxygenLayout.xml`. Its large groups can
follow each language's reading path. Preserve the tutorial's shared source files
under `docs/examples/`; check code once and links in each language.

The header language links open the same topic in another language. A section
is retained when both pages define corresponding explicit IDs; otherwise the
link opens the page at its beginning. Missing translations are labelled in the
selector. Old Russian page URLs redirect into `ru/`; their fragments are retained.
Markdown language links continue to work when browsing the repository on GitHub.

Record the source revision in an HTML comment near the translated page's title.
The [Russian first-application tutorial](ru/first-application.md) is the pilot:

```text
<!-- translation-source: docs/first-application.md
source-revision: d6ef4e75340772de254ef7a2d7514856e5f8e7f5
status: current
-->
```

| State | Meaning and action |
|---|---|
| `current` | The translation was checked against the recorded source revision. When the source changes, compare and update both pages. |
| `outdated` | Source changes remain untranslated. Mark the translation as outdated visibly, link to the newer source language, and retain its last reviewed revision. |
| Missing | There is no translated page. Link to the available language and label it explicitly. |
| Not tracked | The page has no source-revision comment yet. Do not infer that it is current; add the comment after reviewing it. |

Before changing a translated topic, compare its source with the recorded revision:

```powershell
git diff d6ef4e75340772de254ef7a2d7514856e5f8e7f5 -- docs/first-application.md
```

The command includes working-tree source changes. Review changed explanations,
commands, API names, ownership/error conditions, TBD labels and milestones.
Update the translation and recorded revision together. If the source contains
uncommitted changes, record its revision after committing that source; do not
mark the translation current against a commit that lacks those changes. A changed
source requires this review even when the translation comment still says `current`.
The marker records a review; it does not detect drift automatically.

Keep Markdown current for its selected branch. Stable HTML follows the latest
release, and PR HTML is a preview. Release notes and measurements retain their
version/date scope. Before adding another language, choose its locale directory
and unique page IDs, add reciprocal links and navigation, and register it in `docs/languages.json` and `docs/DoxygenLayout.xml`.
The build generates a separate site and search index for each language.
Keep English as the default at the root. Test language links and legacy URLs
with the fixtures under `tools/docs/`.

Build `portals_docs`, check links, and inspect both themes. Confirm the root HTML
`lang` and `xml:lang`, language round trips, glossary definitions, API type links
and search routes to the tutorial and relevant contracts. Each language has its own navigation and search index. English HTML is at the
site root; Russian HTML is under `ru/`. C++ API reference remains in English,
with an explicit link from the Russian site. These checks
also apply to the local offline HTML directory. Translation state is maintained
in the translated Markdown; no separate catalog or generated translation is required.

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
