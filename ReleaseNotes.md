# Release notes

Versions are listed newest first. Entries describe implemented changes;
planned work is not included. An **Unreleased** heading has no publication date.

## Unreleased

- [Added] `ui::runApp(scene)` and the equivalent `scene | ui::runApp()` exit-code
  adapter, orderly `scene.quit()`, and an independent `reportOnError` modifier
  for structured final diagnostics after terminal restoration and cleanup attempts.
  Typed `scene.run()` remains available; gallery and custom meter use the adapter.

- [Added] Complete owning gallery and custom meter examples with equivalent method/pipe
  composition, preserved editor/shortcut behavior, typed failures and cleanup, and
  before/after replay checks across themes and color modes in the installed consumer.

- [Added] Owning lazy scenes with prepare/configure/run methods and small pipe stages,
  internal-state text editors, typed setup/run failures and explicit managed cleanup
  after native open failure. Preparation uses the existing runtime without terminal I/O.

- [Fixed] Scheduler creation reports MSVC Debug container allocation failures,
  and shutdown releases callbacks and queued snapshots without allocating,
  including reentrant cleanup under memory pressure.

- [Added] Ref-first UI recipes, compact selector ids, void or typed click actions
  with routed parameters, checked late button registration, and composable short
  key handlers using the existing managed input lifecycle.

- [Added] Interactive editor history example comparing IMMEDIATE, ON_COMPLETION
  and EXPLICIT drafts with read-only published model rows, compact Undo/Redo
  buttons and fluent append. Finite headless checks exercise VT input and the
  relocated installed package.
- [Fixed] Windows terminal flush returns promptly on cancellation under ConPTY
  pressure while retaining uncertain output until transport cleanup.

- [Added] Bounded LineEdit Undo/Redo with Unicode cursor and selection restoration,
  configurable grouping, limits and shortcuts, three publication modes, explicit
  programmatic history policies, and fluent editor commands with typed diagnostics.

- [Added] Specialized owning text history with staged Undo/Redo, grapheme-boundary
  cursor and selection restoration, separate typing/deletion groups, configurable
  operation and logical memory limits, and typed preparation failures. Editor
  integration is separate from this low-level component.
- [Changed] LineEdit defaults to immediate model publication after semantic
  validation. ON_COMPLETION confirms on blur/Enter; EXPLICIT preserves opt-in
  draft/Enter behavior. Input, clipboard and fluent changes share one policy;
  rejected intermediate text remains visible without publishing an invalid value.
- [Added] Optional scoped focus callbacks on existing EventRouter registrations,
  delivered synchronously without changing RoutedEvent or creating another runtime.
  Loss errors preserve best-effort gain; retired targets cannot receive subsequent
  input. LineEdit results identify input/fluent/blur triggers, and managed blur
  failures retain one diagnostic through preparation.
- [Added] Typed `ui::Ref<T>` text properties with `=` and `+=`, owning read-only
  concatenation with `+`, explicit resource resolution and scene diagnostics.
  Captions and LineEdit drafts keep their existing ownership, thread-affinity,
  conflict and text-validation contracts; rejected text mutations preserve text.
- [Added] Bounded scene-owned managed diagnostics with owning causal context,
  typed error details, `onError` notifications and inspect/clear/drain operations.
  Overflow, context-allocation failures and observer failures remain explicit;
  the existing runner and low-level error contracts are preserved.
- [Added] Owning `text` and `i18n` argument values with compile-time checked
  formatting and explicit resource resolution, preserving text-validation
  errors and the receiving display policy.

## v0.2.0 (2026-10-10)

- [Added] C++23 retained terminal UI core with UI-thread ownership, typed
  errors, bounded scheduling, event routing, focus and an application runner.
- [Added] Managed control lifecycle and owning `ui` descriptions for forms,
  layouts and custom widgets. Scenes manage bindings and preparation while
  the low-level tree and control APIs remain available.
- [Added] Properties and immutable model snapshots, virtual lists and tables,
  scrolling, Canvas and charts. Managed virtual views prepare from captured
  layout geometry and share pinned model snapshots within a frame.
- [Added] UTF-8 grapheme text and explicit terminal cell-width policies,
  including wide characters and combining sequences.
- [Added] Styles, selectable theme palettes and bounded causal diagnostics
  for input, scheduling and frame processing.
- [Added] Checked `formatText`, reusable `cellFormat`, typed table accessors
  and `tableColumns` recipes with automatic table-local column identities
  for fixed schemas. Explicit identities remain available for dynamic schemas.
- [Added] Optional `portals::aggregate` target for positional aggregate columns
  using standalone Boost.PFR headers; consumers do not need the full Boost
  distribution or C++26 reflection.
- [Added] `ui::grid` composition and constexpr `keyPressed` helpers.
- [Added] Minimal `portals::legacy` streaming compatibility for the original
  dxFeed `MarketDepthModelSample`, including its historical umbrella-header
  aliases. This is a limited migration boundary, not the complete 0.1.x API.
  See [compatibility and cleanup requirements](docs/legacy.md).
- [Added] Relocatable installed CMake package, vendored dependencies for offline
  source builds, headless tests, opt-in fuzz targets and installed-package
  consumer checks.
- [Changed] Gallery, dashboard and custom-widget examples use managed APIs;
  the gallery also retains an explicit low-level scene fixture. Documentation
  now has a reading path, complete examples, API contracts and offline Mermaid
  diagrams.
- [Fixed] Text-field caret visibility and orderly Windows terminal screen
  restoration in the example applications. After cancelled output, native cleanup
  waits for the console host before restoring the caller's screen and cursor.
- [Fixed] MSVC builds interpret project sources and string literals as UTF-8,
  independent of the Windows host code page.
- [Changed] Naming checks omit statistics about suppressed diagnostics while
  retaining project diagnostics and failure exit codes.
- [Added] Tag-triggered and manual release automation, version-specific release
  descriptions, release candidates, verified bundles and GitHub Pages deployment
  through Actions. Standalone PowerShell and Python commands prepare release PRs;
  merging a release PR automatically starts tag creation and publication.
  See [release process](docs/releasing.md).

### Known limitations

The managed and declarative APIs remain experimental; version 0.2.0 does not
promise a stable API or ABI. Native backends target Windows and POSIX/Linux;
macOS support has not been established.

With delayed range delivery, sustained scrolling can keep a virtual table
showing loading placeholders until the key is released. Continuous-scroll
loading improvements remain pending.

See [current limitations](docs/limitations.md) for platform, ownership,
shutdown and delivery boundaries.
