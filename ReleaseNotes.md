# Release notes

Versions are listed newest first. Entries describe implemented changes;
planned work is not included. An **Unreleased** heading has no publication date.

## Unreleased

### Added

- Composite message dialogs and theme selectors support the canonical `ui::Ref<T>`
  syntax while retaining their existing handles, commands and observation lifetimes.
- Notification surfaces accept an observing `ui::Ref<Overlay>` as the first factory argument.

- `ui::runApp` accepts a factory returning an owning scene or a checked scene result,
  including pipe and command-line forms. Command-line help and errors are handled
  before invoking the factory or opening the terminal.

### Changed

- Online documentation now follows `main` independently of releases, with a development marker, source revision and UTC build date. Release HTML archives retain their versioned documentation.

### Fixed

- Message-dialog declarations preserve the caller's Win32 `MessageBox` macro and use consistent class names regardless of Windows header order.
- Unicode glyph declarations preserve the caller's Win32 `UNICODE` macro.

- Standard managed text controls now match their natural type selectors while preserving existing `Control` rules; explicit low-level targets retain exact matching unless they opt in. Rebuild the SDK and consumers together because `StyleTarget` gains a compatibility flag.
- Cold text lists retain their IDs, classes and custom type during preparation, including named references resolved before startup.
- HelpView content participates in managed styles and respects manual defaults; KeyHints paints its effective disabled appearance.
- Failed standalone HelpView mounting removes the entire partially constructed subtree.

### Documentation

- Refresh the HTML reference with an offline Doxygen Awesome theme, a light/dark appearance selector, and aligned language and sidebar navigation.
- Correct guide contracts and build instructions, synchronize English and Russian reading paths, and bundle complete example sources for offline use.
- Compile every complete documentation program in the ordinary test build, reusing existing guide targets without launching interactive terminals.


## v0.5.0 (2026-10-20)

- [Examples] The gallery and dashboard include the ready-made theme selector,
  with live previews, Apply and Cancel, while preserving their existing F2 shortcuts.

- [Added] A modal theme selector provides catalog navigation, live previews,
  checked cancellation and explicit recovery without replacing application/user
  styles. Optional post-commit delivery preserves independent callback and close
  errors; the view uses the existing dialogs and canonical shortcut routing.
- [Added] A checked theme-selection model previews catalog choices, atomically
  applies or cancels a preview and preserves application/user style overrides.
  External theme changes retire stale preview authority. A complete native guide
  demonstrates the model without introducing another event loop or theme registry.
- [Added] Annotated key bindings expose shared labels, availability and shortcuts
  through KeyHints and nonmodal HelpView. Weak observing handles support checked
  rebinding and removal without a separate keymap or manual presentation sync.
- [Added] Managed action handlers can return the complete checked ManagedError
  result while retaining its diagnostic receipt, original cause and first failure.
- [Docs] English and Russian guides now have separate navigation and search,
  paired language links, updated walkthroughs and a complete native quick-note example.

- [Added] Ready-made message, confirmation and unsaved-changes dialogs provide
  safe initial choices, owning localized captions and reusable observing handles.
  Callback and coroutine consumers share the existing UI loop and retain complete
  typed results; choosing Save requests an action without performing file I/O.

- [Added] Application-owned notifications with bounded queues, actions, persistent
  messages and expiry after first visible layout, using the existing Scheduler.
  Notification clicks preserve editor focus unless an action changes it explicitly.

- [Docs/Tests] A complete native nested settings form demonstrates editable fields,
  internal-state checkboxes and frame-edge scroll bars. Installed-package consumers
  check nested wheel ownership, focus reveal, clipping and capture retirement,
  and compile the same maintained guide.
- [Added] Retained modal and nonmodal dialogs share the existing tree and runner,
  with checked opening/dismissal, focus return, capture cancellation and body-anchor
  placement. A complete native settings guide and installed-consumer checks cover
  the public composition and error/cleanup boundaries.

- [Tests] Installed-package consumers check live editor words and graphemes in
  the same frame as an external model update, routed input, token identity,
  owning pins, failed-mount cleanup and idle redraw behavior. The complete
  native text-transform guide also compiles against the installed SDK.

- [Fixed] Browser gallery Debug startup uses an explicit configurable Wasm stack
  budget, with Debug stack overflow checks.

- [Added] Data-state presentation for ordinary content and prepared virtual lists/tables,
  with owning or read-only bound state, localized status labels, explicit retry and scoped animation.

- [Added] LineEdit supports an owning placeholder, immutable NORMAL/PASSWORD/NO_ECHO
  display policies and a checked undoable user Clear command. Protected modes suppress
  outbound copy/cut while allowing writable paste; plaintext models, getters and bounded
  history remain available to the application, without a zeroization guarantee.

- [Added] CheckBox supports explicit unchecked, checked and mixed states through
  typed Property/managed/declarative bindings. Boolean APIs retain their cycle;
  an explicit option enables a three-state input cycle. Mixed selection has a
  distinct text glyph, and aggregate helpers preserve partial update receipts.

- [Added] Scene-owned live caption/draft/committed text sources, bounded owning
  whitespace-token and grapheme transforms, and cold text-to-virtual-list recipes.
  Collection-bound lists preserve last good data on failure or origin retirement;
  changed tokens receive fresh IDs and missing selection clears on new bound lists.
  Explicit owning row collections also bind read-only managed sources to models.
- [Added] Ordinary scroll areas and standalone/shared scroll bars support exact
  offsets, wheel/key/track/thumb input, managed focus reveal, and reserved, overlay
  or frame-edge placement. Native sessions request checked held-button motion only
  for mounted scroll controls; scoped capture and scene cleanup preserve ownership.

- [Added] Read-only managed derived sources compute owning values from explicit
  state or earlier derived sources. Initial creation is atomic and checked;
  runtime failures preserve the last good output. Existing captions and managed
  observers accept the result and recover rejected notifications without polling.

- [Added] Scene-owned managed-state observers deliver the latest value through
  checked callbacks, with initial synchronization after reference publication,
  bounded dirty-preparation recovery and receiver-scoped cleanup.

- [Added] Labels and buttons accept owning text expressions and explicit shared
  scene-owned string state. Caption synchronization reuses managed dirty-frame
  preparation and preserves typed text failures. Read-only caption, draft and
  committed-text sources return owning snapshots without adding subscriptions;
  existing editor confirmation modes and undo history remain unchanged.

## v0.4.0 (2026-10-16)

- Add fluent `SceneResourceOptions::control<T>()` registration. Registration failures
  reach the existing `runApp` error handler before resource work or terminal open;
  checked `registry.add<T>()` remains available for explicit recovery.

- [Added] Lazy `ui::scene` and `ui::resource` accept owning scene text or an
  explicit file source. The bounded loader validates the complete grammar,
  allowlisted typed registry, IDs and localized text before managed mounting;
  file, parser, resolver and factory errors retain their typed cause and source
  position. Defaults cap source at 1 MiB, nodes at 4096, depth at 64, each text
  value at 64 KiB and properties per node at 16; depth cannot be configured
  above 64. PSS remains a separate existing stage.
- [Added] Managed `Ref` geometry reads return checked owning snapshots from the
  last successful final layout. Scene-owned `onLayout` observers deliver the
  initial and changed values on the existing scheduler outside tree frames;
  bounded queue recovery preserves the latest value, and EOF drains only these
  pending observations without bypassing the frame-rate cap.
- [Added] Style-layer benchmarks compare equivalent legacy and layered updates
  with scoped allocation/work counters; the quality adapter preserves raw
  samples and provenance for review. Counters cover documented allocation
  classes and phases, not total process memory or universal runtime cost.
- [Added] Focus scopes select their last eligible control or configured default
  on entry while preserving ordinary global Tab and Shift+Tab traversal.
- [Added] Focus-only activations restore the previous target after leaving a
  scope or hiding, disabling or removing it, including nested return order.
  These activations do not capture input or create a modal input shield.
- [Changed] Focus recovery retains the original managed blur diagnostic once,
  including when its callback removes the source control.

- [Added] Lazy named control lookup through `Scene::get` preserves exact widget
  identity across rename/removal and reports missing, duplicate and incompatible
  targets through the existing typed diagnostics and startup boundary.
- [Added] Exact-Ref PSS string/file stages reuse style layers and the existing
  resolver, including atomic metadata admission for unstyled targets. Theme
  replacement preserves application overrides without synthetic selector IDs.

- [Added] Owning range composition for `vbox`, `hbox`, `row` and `column`
  materializes consuming recipes immediately without retaining source views.
- [Added] Compact width/height/flex modifiers, a single-child border/frame,
  a determinate gauge and plain buttons reuse retained layout, painting and routing.
  Scoped PSS overrides support theme changes for these controls.
- [Changed] Layouts can constrain their children's content clip. The new optional
  virtual hook requires rebuilding C++ consumers together with the updated SDK.
- [Added] Offline architecture maps in English and Russian, an interactive viewer
  and a reproducible standard-library generator with source-archive checks.
- [Changed] The dashboard uses declarative size composition; a compact native form
  demonstrates a border, gauge, button actions, scoped styles and standard shutdown.

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
