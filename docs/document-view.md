# DocumentView {#document-view}

[Русская версия](ru/document-view.md) · [LogView](log-view.md) · [Scroll controls](scroll-controls.md)

Use `DocumentView` to read a text document without loading its entire contents or a complete logical line. It reads and decodes bounded windows on the application's background executor. The UI paints the last immutable prepared window and remains available for input while a new window is being prepared.

@image html document-view.gif "Scroll a document, switch sources and return to the remembered position."

## Complete application {#document-view-example}

Build `portals_document_view_example` with `PORTALS_BUILD_EXAMPLES=ON` and run it from the Portals source directory, where `README.md` and `ReleaseNotes.md` are present. The buttons switch the same view between both files, refresh its current source, and return to the beginning. Switching back remembers a byte position for each retained source descriptor. F10 or Ctrl+C closes the application through the standard runner.

[Complete source](../examples/document_view/main.cpp)

\include document_view/main.cpp

`ui::viewer("README.md")` is the shortest file-backed form. Keep a `ui::Ref<DocumentView>` when handlers need to replace its source or change its reading position. `ui::documentFile(path)` owns the UTF-8 path and performs no file I/O during recipe construction. Keep the same descriptor when switching back to its remembered position.

## Reading and navigation {#document-view-navigation}

| Input | Result |
|---|---|
| Up / Down, mouse wheel | Move through prepared visual rows. |
| PageUp / PageDown | Move by a viewport with the configured overlap. |
| Home | Request byte zero. |
| End | Request the final available screen; preparation can remain pending while the source is scanned. |
| Left / Right with wrapping disabled | Move the horizontal cell window. |
| Vertical scrollbar | Request a byte position when an exact byte extent is known. |
| F5 | Refresh the current source. |

The scrollbar represents bytes, rather than an invented exact number of lines. With an unknown extent, the view hides the global position bar. The footer shows an original byte offset and a byte percentage when the extent is known. Line numbers are optional and only show counts proven by scanning. Wrapped fragments retain the same logical line number. The fixed gutter uses an overflow marker when a number exceeds its displayed precision.

A seekable source can prepare a distant cold seek from a bounded local window without scanning the entire preceding prefix. The encoding decision is reused only when it was proven at the source origin; the requested original-byte anchor is retained. Logical line numbers and totals remain unknown until proven, and reaching local EOF does not establish the document-wide line total. At or beyond a known EOF, a nonempty source shows its available final row or fragment while the requested offset remains at EOF or one byte past it; a terminal line feed can produce an empty final row. An empty source remains empty. An empty read at an arbitrary distant offset does not establish EOF. The last prepared frame remains visible during preparation. A source which returns no bytes without final EOF enters a waiting state; call `reload()` or press F5 when more data becomes available. This control does not poll the file for live append; use [file-backed LogView](log-view.md#log-view-file) for follow-tail behavior.

## Sources and commands {#document-view-sources}

Files are one source family. `ui::document(provider, budget)` accepts a worker-safe `ByteDocumentProvider`; `ui::document(sharedText)` projects existing immutable storage without copying the entire input. A custom provider declares seeking and immutability capabilities and can leave its extent unknown. It must return an exact revision with each accepted read. Replacing a source discards incompatible prepared data; results from cancelled or superseded work cannot replace the active binding.

| Operation | Collected form | Checked form |
|---|---|---|
| Replace source | `reader.model(source)` | `reader.setModel(source)` |
| Request original byte position | `reader.byte(offset)` | `reader.seekByte(offset)` |
| Refresh at the current anchor | `reader.reload()` | `reader.refresh()` |
| Observe cached status without I/O | `reader.currentStatus()` | `reader.status()` |

Collected commands report failures through the receiving scene's existing error collector. Checked commands return `std::expected` and leave error handling to the caller. Admission means the operation was accepted; the requested data may still be preparing. Use these commands on the owning UI thread, for example from a button or key handler. `runApp` owns the background service and terminal cleanup.

The first version is read-only. It does not provide text selection, clipboard export, editing, search or automatic semantic rebasing over arbitrary external rewrites.

## Encodings and resource settings {#document-view-settings}

The default UTF-8 hint applies after an explicit encoding or a recognized BOM. Supported decoders are UTF-8, UTF-16LE/BE, UTF-32LE/BE, Windows-1251 and Windows-1252. Select one with `.encoding(DocumentEncoding::UTF16LE)`. `.wrap(false)` keeps a horizontal cell window; `.lineNumbers()` enables the gutter. Neither setting changes the original byte anchor.

Replacement decoding is the default. Low-level `DocumentViewConfig::decoder` also supports strict decoding; a strict failure reports a typed preparation error. Terminal control characters are rendered as visible escapes. Ordinary multi-scalar grapheme clusters, including those crossing page boundaries, are preserved. Local restart uses up to 4 KiB of grapheme precontext, in addition to independent decoder lookbehind. An oversized cluster or unprovable bounded grapheme context (for example, a regional-indicator run longer than 4 KiB) is shown as a visible dotted-circle continuation and sets `fragmentedCluster`. For a leading regional-indicator run, parity remains unproven until a non-regional-indicator resets it, including after a cached near seek. This bounded local projection does not limit logical line length.

The native resource type is `DocumentView`. In the CONFIGURATION grammar its viewer-specific properties are `file` (a literal UTF-8 path), `lineNumbers`, `wrap` and `encoding`. The ordinary size, frame and scrollbar properties use the same semantics as their C++ modifiers. They are not accepted as viewer properties on unrelated controls.


[Complete resource program](examples/guide_document_view_resource.cpp)

\include guide_document_view_resource.cpp

## Styling reference {#document-view-styling-reference}

The type selector is `DocumentView`. Text, fill and footer use the normal, `:focused`, or `:disabled` appearance. Built-in themes retain the document surface while focused chrome carries the accent; an explicit `DocumentView:focused` rule can override the text surface. `foreground`, `background`, `bold`, `italic` and `underline` use the ordinary [style cascade](styling-guide.md).

Frame patterns, title decoration, arrows, tracks and individual thumb parts use the complete [frame and scrollbar styling reference](scroll-controls.md#scrollbar-styling-reference). `.framed("rounded", "Document")` supplies a title; the one-argument `.framed("rounded")` selects a pattern, as for other controls.

## Resource limits {#document-view-limits}

Limits bound retained capacity and work, rather than document size. The default source root reserves at most 8 MiB; applications can share and configure a `SourceBudget`. `DocumentViewConfig` defaults to 16,384 actual viewport cells, 64 KiB of prepared payload, and 256 KiB of admitted source bytes per worker turn. Source pages default to 64 KiB. A viewport exceeding its configured preparation cap reports `PAGE_TOO_LARGE`; it is not silently truncated.

Pinned pages, immutable scanner snapshots, prepared output and cancelled work retain their credits until their actual owners finish. Provider-owned domain storage is outside the SDK's retained-page ledger. Process memory, source credits, cold-seek latency and terminal presentation time are different measurements; none is a file-size guarantee.

The view retains up to 16 in-session source bookmarks. Slots are reused cyclically for additional sources; a source without a retained bookmark opens at byte zero. These bookmarks are not persisted across application runs.
