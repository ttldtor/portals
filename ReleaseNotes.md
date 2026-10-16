# Release notes

Versions are listed newest first. Entries describe implemented changes;
planned work is not included. An **Unreleased** heading has no publication date.

## Unreleased

- [Added] Developer tools report observed Ninja translation-unit dependencies and
  public include relationships, separating link records, incomplete builds and
  unknown impact instead of presenting them as complete rebuild coverage.
- [Added] A local contract check planner distinguishes task preflight from full
  integration and milestone checks. Runtime exclusions remain unqualified; existing
  hosted compiler, package and documentation gates are unchanged.

## v0.3.0 (2026-10-15)

- [Added] Optional `portals::lyra` and `ui::lyra::cli` adapt native Lyra declarations
  to lazy command-line startup, with help/version before terminal acquisition and
  owning typed CLI diagnostics. Lyra 1.8.0 headers, BSL-1.0 notice and provenance
  manifest ship in the offline SDK. Read-only argument/environment snapshots and
  borrowed `ui::opts` supply launch data without a second application runtime.
- [Changed] The gallery is an ordinary `gallery::form(ui::opts(options))` function
  returning a lazy owning scene. Its native Lyra schema fills theme/color options
  before writer settings or mounting; internal editor/checkbox state and standard
  quit/theme handlers replace the stationary public wrapper and manual argv loop.

- [Added] Owning scene factories and configuration callbacks can opt into the
  existing UI-affine `Application` alongside `ManagedScene`; compatible one-argument
  callbacks keep priority. `Scene::onStop` owns one shutdown callback, runs it once
  before nodes/models retire, and preserves typed failures while attempting cleanup.

- [Changed] Public gallery, dashboard, custom-widget and editor-history examples
  use the owning declarative scene and standard `runApp` exit policy. Finite replay,
  low-level comparison and stress fixtures live under tests; their `--check` and
  `--stress` modes are no longer options of the interactive public executables.
  The installed consumer and offline bundles include the separate finite harnesses.
  Public introductory styles use runtime PSS strings; compile-time styles remain
  optional experimental material.

- [Added] Scoped current-scene `ui::quit()` and bare `ui::quit` key/click commands.
  Weak UI execution context follows setup and owned callbacks, restores across
  nested scenes and exceptions, and is suspended during independent cleanup.
  No current live scene is a safe no-op; explicit checked `SceneRef::quit` remains.
  Existing explicit quit actions and typed pointers remain supported; untyped
  `&ui::quit` now requires overload selection.

- [Added] Scoped short text-write setup failures preserve the original typed cause, operation
  and source before diagnostic allocation or observer reentry. The current void
  callback finishes; later setup and terminal entry are skipped, without rollback.
  Exact short-result provenance avoids duplicate reports in the same collector;
  manual recoverable reports and runtime text/editor behavior remain unchanged.

- [Added] Owning multi-stroke shortcuts with shared-prefix matching in the existing
  managed input path. `Ctrl+K+K` shares modifiers; commas allow independent stroke
  modifiers. Explicit event selections apply to every stroke. Pending commands
  cancel on Escape, focus or eligibility changes, with one configurable scoped
  timeout that never invokes delayed commands or requests idle redraw. Registration
  remains bounded and atomic, with typed ambiguity and reserved-cancel errors.

- [Added] Owning `ui::press`, `repeat`, `release`, `pressRepeat` and `keyActions`
  shortcut selections for text, typed chords and alternative groups. Selections
  admit exact native event phases, preserve typed construction errors and use
  the existing checked registration and callback adapters. The VT decoder still
  emits PRESS only; selectors do not synthesize backend repeat or release events.

- [Added] Short semantic key aliases, modifier chords and owning alternative
  bindings, including case-insensitive runtime shortcut strings. New commands
  default to press-only with configurable repeat and parser budgets; the legacy
  KeyEvent API retains its behavior. Modified text decoded by terminals uses
  the same input router, with typed syntax diagnostics and atomic registration.

- [Added] Deferred `ui::onClick(ref, callback)` pipe stages with scene-owned FIFO
  handlers, read-only click metadata, semantic modifier queries and explicit
  `stop()`/`stopPropagation()`. Handler counts are bounded and configurable;
  removal, hide, disable and cleanup stop further delivery safely.

- [Added] `ui::hbox`/`ui::vbox` aliases and a complete minimal click-handler
  example, compiled through the installed-package consumer. The usage guide
  documents callback lifetime, errors and the current explicit scene quit command.

- [Fixed] Click-chain allocation and retirement preserve checked out-of-memory
  behavior with MSVC Debug container proxies.

- [Added] A shared PSS v0 parser for C++23 constant strings and runtime owning
  documents, with bounded data descriptors, short typed selectors and source
  metadata for every declaration. Existing typed live themes and the legacy
  parser remain available.

- [Added] Native Scene styling from C++23 PSS descriptors, runtime bytes,
  owning documents and explicit native file paths, with shared source metadata,
  named layer upsert and checked runApp factory/setup integration.

- [Added] Opt-in DEFAULTS/THEME/APPLICATION/USER style layers with property-wise
  precedence, atomic updates and aggregate logical payload budgets. Theme switches
  and live theme parameters preserve overrides; stale theme controllers report
  typed errors. Legacy whole-sheet replacement remains available.

- [Fixed] Constant scene factories preserve immediate temporary expressions on
  MSVC 19.44 by using explicit typed defaults and union-free internal style storage.
  Public style values and compile-time validation remain unchanged.

- [Added] Bounded dashboard page caching, prefetch and a backend job queue that
  keep pending data loads progressing during continuous keyboard navigation.
- [Added] Owning model queries with separate query generations, in-memory
  projection and deferred providers using the existing scheduler.
- [Added] C++23 constant scene descriptions with structural checks and runtime
  attachment of references, bindings and callbacks through the retained core.
- [Added] Constant style rules with typed live parameters and atomic batch
  updates through the existing style cascade.
- [Added] UI-affine coroutine tasks for timers and provider replies, with typed
  failures, cancellation and late-reply handling on the existing scheduler.

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
