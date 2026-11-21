# DocumentView {#document-view}

[Русская версия](ru/document-view.md) · [LogView](log-view.md) · [Scroll controls](scroll-controls.md)

Use `DocumentView` to read a text or hexadecimal document without loading its entire contents or a complete logical line. It reads and decodes bounded windows on the application's background executor. The UI paints the last immutable prepared window and remains available for input while a new window is being prepared.

@image html document-view.gif "Scroll a document, switch sources and return to the remembered position."

## Complete application {#document-view-example}

Build `portals_document_view_example` with `PORTALS_BUILD_EXAMPLES=ON` and run it from the Portals source directory, where `README.md` and `ReleaseNotes.md` are present. The buttons switch the same view between both files, refresh its current source, and return to the beginning. Switching back remembers a byte position for each retained source descriptor. F10 or Ctrl+C closes the application through the standard runner.

[Complete source](../examples/document_view/main.cpp)

\include document_view/main.cpp

`ui::viewer("README.md")` is the shortest file-backed form. Keep a `ui::Ref<DocumentView>` when handlers need to replace its source or change its reading position. `ui::documentFile(path)` owns the UTF-8 path and performs no file I/O during recipe construction. Keep the same descriptor when switching back to its remembered position.

## SQL and Markdown source colors {#document-view-syntax}

Build `portals_document_highlight_example` with `PORTALS_BUILD_EXAMPLES=ON` and run it from its executable directory. The build copies `orders.sql` and `notes.md` beside the executable. Both panes use the ordinary Viewer: Tab changes focus, wheel and PgUp/PgDn navigate, and F10 or Ctrl+C closes the application.

[Complete source](../examples/document_view/highlight.cpp)

\include document_view/highlight.cpp

\htmlonly
<details><summary>Bundled SQL and Markdown inputs</summary>
\endhtmlonly

\verbinclude document_view/orders.sql

\verbinclude document_view/notes.md

\htmlonly
</details>
\endhtmlonly

@image html document-view-highlight.gif "SQL and Markdown source colors with independent document navigation."

`.language("mysql")`, `.language("sql")` and `.language("markdown")` select bounded lexical profiles. Names are case-insensitive: `postgres`, `postgresql` and `sql` select PostgreSQL lexical rules; `md` aliases `markdown`. An unknown name selects plain text. The typed form accepts `DocumentLanguage`; `.language(DocumentLanguage::PLAIN)` disables syntax colors. HEX remains a raw-byte projection.

The Markdown profile colors source headings, punctuation, code spans and fenced regions, including SQL fences. It does not render Markdown as formatted HTML. The SQL profiles recognize lexical structure, not database schemas or query semantics. MySQL support includes bounded delimiter and SQL-mode handling; unsupported constructs and exhausted lexical policies leave readable text with an explicit syntax status.

Reading, decoding and lexical work run on workers. A cold seek first publishes plain text, then adds exact colors after restoring lexical context from a qualified checkpoint or the source origin. Distant exact highlighting can require substantial cumulative reading. Scrolling can supersede this optional replay. Source size does not determine resident memory: the view retains bounded windows and a fixed checkpoint cache, without a complete line or token index.

### Customize syntax roles

Built-in themes supply foreground colors for syntax roles. `DocumentView` retains its normal background when focused. Override individual roles through PSS; changing a palette repaints existing ranges without reading or lexing the source again.

```css
DocumentView {
  role-keyword-foreground: #88c0d0;
  role-keyword-bold: true;
  role-string-foreground: #ebcb8b;
  role-comment-foreground: #81a1c1;
}
```

| Role | Source meaning | Built-in theme value |
| --- | --- | --- |
| `keyword` | SQL keyword | Accent, bold |
| `number` | SQL number | Heading color |
| `string` | SQL literal or Markdown code | Accent |
| `comment` | SQL comment | Muted |
| `identifier` | SQL identifier | Foreground |
| `operator` | SQL punctuation/operator | Foreground |
| `heading` | Markdown ATX heading | Heading color, bold |
| `markup` | Markdown delimiters/MySQL client directive | Accent |

Each `role-<name>-` prefix supports `foreground`, `background`, `bold`, `italic` and `underline`. Background overrides are optional. Ordinary text inherits `body`; `gutter` and `position` retain their existing meanings. Disabled syntax uses the theme's muted foreground without adding italic text.

`document.language("md")` collects a rejected command in the scene; `document.setLanguage("md")` returns a checked result. Loaded configuration scenes accept `DocumentView { file="notes.md" language="markdown" }`.

`document.status()` reports `language`, `highlighting` and optional `highlightErrorOffset`. `PENDING` means the displayed text awaits proven lexical context; `READY` means all displayed clusters are classified. `UNSUPPORTED_PROFILE`, `RESOURCE_LIMIT`, `REPLAY_UNAVAILABLE` and `UNAVAILABLE` distinguish syntax limitations from the document's source error. The plain frame remains available when optional syntax preparation fails.

For a forward-only source, select the initial language in the description before mounting. Changing to another non-plain profile after attachment returns a checked unsupported-operation error. Disabling colors preserves accepted buffered bytes. History is bounded: reflow that needs bytes outside retained history is not an arbitrary rewind. A source span exceeding the retained-window budget can return `PAGE_TOO_LARGE` before the next unsafe read; this does not roll back earlier reads of that worker turn or promise automatic recovery of an irreversible source.

## Choose a file {#document-view-files}

Build `portals_document_files_example` with `PORTALS_BUILD_EXAMPLES=ON`. Enter a path in the field and choose **Open**, or press Ctrl+O. F2 selects text, F3 selects hexadecimal bytes, and F5 refreshes the current source. F10 or Ctrl+C closes the application. Opening a source starts background preparation; the UI stays available.

@image html document-view-files.gif "Open README, show a missing-file error, then open release notes. The ring marks an injected mouse click."

[Complete source](../examples/document_view/files.cpp) · [Build target](../examples/document_view/CMakeLists.txt) · [Windows manifest](../examples/document_view/windows-utf8.manifest)

\include document_view/files.cpp

The first positional argument supplies the initial path. Without it, the example uses `README.md` in the current working directory. From the executable directory on Windows, replace the following path with your document:

```powershell
./portals_document_files_example.exe --help
./portals_document_files_example.exe "C:\logs\session.txt"
```

On Linux, use the executable name without `.exe` and a native filesystem path. Help prints the purpose and options without opening the terminal. Exit status is 0 for an orderly exit or help, 2 for a command-line parse error, and 1 for an application failure. A path beginning with `-` can be a literal filename; a second positional path is a parse error.

Relative paths use the application's working directory. The field accepts the path itself; shell quotes belong to command-line invocation. UTF-8 names, spaces and native absolute paths are supported. Windows extended paths are passed to the filesystem; network paths also depend on the share, permissions and availability. The example does not expand environment variables or `~`. Its Windows manifest enables UTF-8 process arguments and long-path support for this executable.

Read failures appear inside the document frame and remain available in `DocumentViewStatus::error` through checked `reader.status()` access and the managed diagnostic channel. Correct the path and choose **Open** to try again. The caption uses the `body` style and does not become a document row, line number or progress value. Preparing the optional caption can fail under memory pressure; the original typed error still reaches diagnostics.

Each **Open** creates a source descriptor. Keep a descriptor when returning to its saved reading position; the [previous example](#document-view-example) shows retained sources.

## Reading and navigation {#document-view-navigation}

| Input | Result |
|---|---|
| Up / Down, mouse wheel | Move through prepared visual rows. |
| PageUp / PageDown | Move by a viewport with the configured overlap. |
| Home | Request byte zero. |
| End | Request the last full page; preparation can remain pending while the source is scanned. |
| Left / Right with wrapping disabled | Move the horizontal cell window. |
| Vertical scrollbar | Request a byte position when an exact byte extent is known. |
| F5 | Refresh the current source. |

The wheel moves one displayed row per normalized step by default. Rapid input accumulates while a window is loading. Pending movements in opposite directions can cancel each other; an attempt to move beyond a confirmed boundary does not create a debt against the next movement. Home, End, scrollbar seeks and source replacement establish a new navigation target. Low-level constructors accept `ScrollAreaOptions::wheelStep` for another step.

The scrollbar position is an approximate projection of the visible window across a known byte extent; it is not the footer coverage value. Intermediate thumb positions map across the range from byte zero to `extent - visibleSpan`. This range stays fixed during a drag, and arriving prepared windows do not move the held thumb away from the pointer. Dragging to the maximum endpoint requests the last full page, just like `End`. For a seekable source with a known exact extent, preparation first proves the encoding at the source origin, then searches backward for enough line boundaries and decodes the resulting suffix on a background worker. It retains a bounded row window without building a complete line index. Logical line numbers in a distant suffix remain unknown. A very long line without a preceding safe boundary can still require substantial background scanning. Sources without the required seeking or extent capabilities keep the cancellable forward path. Explicit `seekByte(offset)` commands keep the requested original-byte anchor and use bounded local seeks without scanning the entire preceding file.

End prepares intermediate chunks independently of the paint cadence; only the completed window needs the normal layout and rendering path. Wrapped printable ASCII is processed in spans while preserving the same rows and original byte positions. This bounds retained memory, not cumulative reading: tail preparation can read bytes more than once and may still scan substantial data when logical lines are very long. There is no constant-time EOF guarantee for arbitrary sources.

The footer reports approximate coverage through the original source bytes at the lower edge of the visible content. On the first screen, the percentage can already be greater than zero. It reaches `100.0%` only when the final original source byte is visible; decoder read-ahead and decoded UTF-8 size do not count as visible coverage. A clipped right edge of an unwrapped line does not report 100% until the source's final byte is visible. With an unknown extent, the footer shows `progress unknown` until a visible final byte proves the end; a temporarily waiting provider is not EOF. A terminal line feed does not create an extra empty display row, while actual blank rows in the document remain visible. The original byte anchor and any proven logical line number remain available through `reader.status()`. Line numbers are optional, disabled by default, and only show counts proven by scanning. Wrapped fragments retain the same logical line number. The 9-cell gutter displays up to seven digits: `?` marks an unproven line number and `+` marks a larger proven number; this display limit does not restrict 64-bit byte positions.

A seekable source can prepare a distant cold seek from a bounded local window without scanning the entire preceding prefix. The encoding decision is reused only when it was proven at the source origin; the requested original-byte anchor is retained. Logical line numbers and totals remain unknown until proven, and reaching local EOF does not establish the document-wide line total. At or beyond a known EOF, a nonempty source shows its available final row or fragment while the requested offset remains at EOF or one byte past it; a terminal line feed does not add an extra display row, while blank rows present in the source remain visible. An empty source remains empty. An empty read at an arbitrary distant offset does not establish EOF. The last prepared frame remains visible during preparation. A source which returns no bytes without final EOF enters a waiting state; call `reload()` or press F5 when more data becomes available. This control does not poll the file for live append; use [file-backed LogView](log-view.md#log-view-file) for follow-tail behavior.

@image html document-view-end.gif "Dragging to EOF keeps the final full screen. The marker indicates injected mouse input."

## Fast scrolling and paging {#document-view-fast-navigation}

Opt in to an eight-row fast step with `.fastScroll()`. Hold Ctrl with Up/Down, or Alt while scrolling the wheel. Ordinary arrows and wheel retain their one-row default. The fast step is an absolute number of visual rows: `.scrollStep(3).fastScroll(8)` selects steps of three and eight, rather than multiplying them. Pass other `key::Modifier` values as the second and third arguments to `fastScroll` to change its keyboard and wheel modifiers. Additional modifiers do not match.

PageUp/PageDown move by the useful viewport height minus `.pageOverlap(rows)`, with at least one row per page. The default overlap is one row. The useful height excludes the frame and footer. A page command uses the arranged geometry when accepted; resizing does not reinterpret movement already accepted. The fast step does not change page size.

@image html document-view-navigation.gif "Ordinary wheel, eight-row fast steps, paging with two-row overlap and the final full page. The caption describes approximate injected input."

[Complete navigation application](../examples/document_view/navigation.cpp), target `portals_document_navigation_example`:

\include document_view/navigation.cpp

Click inside the document to give it keyboard focus after using a button. The buttons and built-in input use the same navigation admission path. Command admission performs no source I/O. Pending row movements accumulate while preparation runs; opposite movements can cancel, and Home/End replace the relative target. Source replacement and cancellation keep their existing behavior.

A nonzero page command before usable geometry is arranged returns `WOULD_BLOCK` through the checked/collected error mechanism. Built-in PageUp/PageDown input is consumed without movement when the arranged body has zero width or height. Zero row/page commands still validate their reference and thread, then succeed without movement or redraw. Overflow is rejected before changing pending movement. Input respects disabled and read-only navigation settings; programmatic commands retain the same convention as `seekByte` and can move the view independently of those input restrictions.

The terminal may intercept shortcuts: [Windows Terminal uses Ctrl+wheel for font zoom](https://learn.microsoft.com/en-us/windows/terminal/tips-and-tricks). The separate default Alt wheel modifier avoids choosing that shortcut for fast scrolling; Portals cannot override a host shortcut that is not forwarded. Buttons and custom `ui::onKey` handlers can call the same Ref commands.

## Hexadecimal view {#document-view-hex}

Use `.hex()` to inspect the original bytes in the same viewer. Each row contains a hexadecimal byte offset, grouped hexadecimal bytes and a safe character column. The character column shows printable ASCII; other bytes become dots. It does not decode UTF text or execute control sequences. Text decoding options and optional line numbers remain stored for a return to text mode.

@image html document-view-hex.gif "Switch a UTF-16LE document between bytes and text, then use wheel, fast scrolling and End."

[Complete text/hex application](../examples/document_hex/main.cpp), target `portals_document_hex_example`:

\include document_hex/main.cpp

F2 selects text and F3 selects hexadecimal view. Both use the same source and requested byte position. The footer still measures the last visible byte against the source extent, so changing the representation can change its percentage without moving the requested anchor. A successful mode change cancels unresolved row movement from the previous representation, rather than interpreting text rows as hex rows. Assigning the existing mode again leaves pending movement intact. Rejected changes preserve the old mode, position, prepared frame, pending movement and palette.

The default is 16 bytes per row, groups of eight, uppercase digits and a visible character column. Fixed-width rows retain their configured byte count in a narrow window; the horizontal scrollbar exposes the remaining columns. `.hex({.bytesPerRow = 32, .autoFit = true})` permits fewer byte slots to fit the available width. Resizing or changing row options does not rewrite the requested byte anchor. Empty files and incomplete final rows do not acquire invented bytes.

| Option | Meaning | Default |
|---|---|---|
| `bytesPerRow` | Configured byte slots per row, 1–4096 | 16 |
| `groupSize` | Byte slots per group, 1–4096; a larger group than the row adds no internal gap | 8 |
| `uppercase` | Uppercase hexadecimal digits | `true` |
| `showAscii` | Safe character column | `true` |
| `autoFit` | Reduce the effective byte count to fit, up to `bytesPerRow` | `false` |

These are row/preparation limits, not file-size limits. Existing frame, work and source-memory budgets still apply. File-backed sources support random byte positions; a forward-only source cannot promise a switch or backward movement that requires bytes it has already discarded. Such unsupported operations return a typed error. Unknown extent stays unknown; the viewer does not scan an entire source to manufacture a percentage or row count.

In TEXT mode, changing HEX options only stores them for a later switch to HEX. It does not read the source, prepare another text frame or cancel pending text navigation.

For a forward-only source, choose the initial mode, `bytesPerRow` and `autoFit` in the description before mounting. Once the source is attached, runtime mode changes, and changes to these row settings while HEX is active, currently return a typed unsupported-operation error even when the required bytes remain cached. This is an implementation limit. Grouping, letter case and character-column visibility can change using retained rows. Fixed-grid width/height resizing remains supported. File-backed and other seekable sources support mode changes and all row options.

| Command | Collected form | Checked form |
|---|---|---|
| Select representation | `document.mode(DocumentViewMode::HEX)` | `document.setMode(DocumentViewMode::HEX)` |
| Change row options | `document.hexOptions(options)` | `document.setHexOptions(options)` |

Use `DocumentViewMode::TEXT` to return to text. `DocumentViewStatus::mode` reports the selected representation; hexadecimal rows are not reported as logical text lines.

Loaded scenes use `mode="text"` or `mode="hex"` and an optional `hex` block with the same five option names. Omitted properties keep their defaults; explicit `false` remains meaningful.

[Complete resource application](examples/guide_document_hex_resource.cpp)

\include guide_document_hex_resource.cpp

## Sources and commands {#document-view-sources}

Files are one source family. `ui::document(provider, budget)` accepts a worker-safe `ByteDocumentProvider`; `ui::document(sharedText)` projects existing immutable storage without copying the entire input. A custom provider declares seeking and immutability capabilities and can leave its extent unknown. It must return an exact revision with each accepted read. Replacing a source discards incompatible prepared data; results from cancelled or superseded work cannot replace the active binding.

| Operation | Collected form | Checked form |
|---|---|---|
| Move visual rows | `reader.scroll(rows)` | `reader.scrollRows(rows)` |
| Move pages | `reader.page(pages)` | `reader.scrollPages(pages)` |
| Beginning / final full page | `reader.home()` / `reader.end()` | `reader.goHome()` / `reader.goEnd()` |
| Replace source | `reader.model(source)` | `reader.setModel(source)` |
| Request original byte position | `reader.byte(offset)` | `reader.seekByte(offset)` |
| Refresh at the current anchor | `reader.reload()` | `reader.refresh()` |
| Show or hide line numbers | `reader.lineNumbers(enabled)` | `reader.setLineNumbers(enabled)` |
| Toggle line numbers | `reader.toggleLineNumbers()` | — |
| Observe cached status without I/O | `reader.currentStatus()` | `reader.status()` |

Collected commands report failures through the receiving scene's existing error collector. Checked commands return `std::expected` and leave error handling to the caller. Admission means the operation was accepted; the requested data may still be preparing. Use these commands on the owning UI thread, for example from a button or key handler. `runApp` owns the background service and terminal cleanup.

The first version is read-only. It does not provide text selection, clipboard export, editing, search or automatic semantic rebasing over arbitrary external rewrites.

## Encodings and resource settings {#document-view-settings}

The default UTF-8 hint applies after an explicit encoding or a recognized BOM. Supported decoders are UTF-8, UTF-16LE/BE, UTF-32LE/BE, Windows-1251 and Windows-1252. Select one with `.encoding(DocumentEncoding::UTF16LE)`. `.wrap(false)` keeps a horizontal cell window; `.lineNumbers()` enables the gutter. Neither setting changes the original byte anchor.

Replacement decoding is the default. Low-level `DocumentViewConfig::decoder` also supports strict decoding; a strict failure reports a typed preparation error. A local seek or fast EOF checks the selected decoded region, including the origin needed to determine its encoding; it does not certify an omitted prefix. Terminal control characters are rendered as visible escapes. Ordinary multi-scalar grapheme clusters, including those crossing page boundaries, are preserved. Local restart uses up to 4 KiB of grapheme precontext, in addition to independent decoder lookbehind. An oversized cluster or unprovable bounded grapheme context (for example, a regional-indicator run longer than 4 KiB) is shown as a visible dotted-circle continuation and sets `fragmentedCluster`. For a leading regional-indicator run, parity remains unproven until a non-regional-indicator resets it, including after a cached near seek. This bounded local projection does not limit logical line length.

The native resource type is `DocumentView`. In the CONFIGURATION grammar its viewer-specific properties are `file` (a literal UTF-8 path), `lineNumbers`, `wrap`, `encoding` and `language`, plus `fastScroll`, `fastKeyModifier` and `fastWheelModifier`. A positive `fastScroll` enables the absolute fast step; modifier names `CTRL`, `ALT` and `SHIFT` are case-insensitive. Existing `keyStep` and `wheelStep` together express the `scrollStep` shorthand, and `pageOverlap` sets page overlap. The ordinary size, frame and scrollbar properties use the same semantics as their C++ modifiers. They are not accepted as viewer properties on unrelated controls.


[Complete resource program](examples/guide_document_view_resource.cpp)

\include guide_document_view_resource.cpp

## Styling reference {#document-view-styling-reference}

The type selector is `DocumentView`. Text, fill and footer use the normal, `:focused`, or `:disabled` appearance. Built-in themes retain the document surface while focused chrome carries the accent; an explicit `DocumentView:focused` rule can override the text surface. `foreground`, `background`, `bold`, `italic` and `underline` use the ordinary [style cascade](styling-guide.md).

Frame patterns, title decoration, arrows, tracks and individual thumb parts use the complete [frame and scrollbar styling reference](scroll-controls.md#scrollbar-styling-reference). `.framed("rounded", "Document")` supplies a title; the one-argument `.framed("rounded")` selects a pattern, as for other controls.

DocumentView registers three base paint parts: `body` for document text,
`gutter` for line numbers, and `position` for the footer. The gutter is visible
when `.lineNumbers()` is enabled. Their five PSS properties are
`role-<part>-foreground`, `role-<part>-background`, `role-<part>-bold`,
`role-<part>-italic`, and `role-<part>-underline`; see the [style property
reference](styles.md#styles-semantic-role-properties) for values and cascade
rules. These parts use the existing theme/application/user cascade. The example
installs `nordTheme()` before its role layer, so omitted backgrounds inherit its
dark surface. For each presentation state, omitted role leaves inherit the
resolved `DocumentView` paint. Changing role paint updates presentation without
reloading the document source.


| Part | Foreground | Background | Bold | Italic | Underline |
| --- | --- | --- | --- | --- | --- |
| `body` | `role-body-foreground` | `role-body-background` | `role-body-bold` | `role-body-italic` | `role-body-underline` |
| `gutter` | `role-gutter-foreground` | `role-gutter-background` | `role-gutter-bold` | `role-gutter-italic` | `role-gutter-underline` |
| `position` | `role-position-foreground` | `role-position-background` | `role-position-bold` | `role-position-italic` | `role-position-underline` |
| `hex-zero` | `role-hex-zero-foreground` | `role-hex-zero-background` | `role-hex-zero-bold` | `role-hex-zero-italic` | `role-hex-zero-underline` |
| `hex-printable` | `role-hex-printable-foreground` | `role-hex-printable-background` | `role-hex-printable-bold` | `role-hex-printable-italic` | `role-hex-printable-underline` |
| `hex-control` | `role-hex-control-foreground` | `role-hex-control-background` | `role-hex-control-bold` | `role-hex-control-italic` | `role-hex-control-underline` |
| `hex-high-byte` | `role-hex-high-byte-foreground` | `role-hex-high-byte-background` | `role-hex-high-byte-bold` | `role-hex-high-byte-italic` | `role-hex-high-byte-underline` |

HEX adds `hex-zero` for zero bytes, `hex-printable` for graphic ASCII and space, `hex-control` for other ASCII controls, and `hex-high-byte` for bytes at or above 0x80. Offsets use `gutter`; separators and padding use `body`. HEX roles are registered only for that representation and use the same PSS/theme cascade.


Run this sample from the Portals source directory so `README.md` is available. Press F2 to show or hide the line-number gutter while reading. The source and reading anchor are preserved, including wheel input already accepted during loading. Setting the existing value again does not start another load or redraw.

[Complete application example](examples/guide_document_view_styles.cpp)

\include guide_document_view_styles.cpp

@image html document-view-styles.png "Light document text, gray line numbers and a blue position footer."
@image html document-view-styles.gif "Toggle line numbers, scroll one row per wheel step, and reach the visible end."

## Resource limits {#document-view-limits}

Limits bound retained capacity and work, rather than document size. The default source root reserves at most 8 MiB; applications can share and configure a `SourceBudget`. `DocumentViewConfig` defaults to 16,384 actual viewport cells, 64 KiB of prepared payload, and 256 KiB of admitted source bytes per worker turn. Source pages default to 64 KiB. A viewport exceeding its configured preparation cap reports `PAGE_TOO_LARGE`; it is not silently truncated.

Pinned pages, immutable scanner snapshots, prepared output and cancelled work retain their credits until their actual owners finish. Provider-owned domain storage is outside the SDK's retained-page ledger. Process memory, source credits, cold-seek latency and terminal presentation time are different measurements; none is a file-size guarantee.

The view retains up to 16 in-session source bookmarks. Slots are reused cyclically for additional sources; a source without a retained bookmark opens at byte zero. These bookmarks are not persisted across application runs.
