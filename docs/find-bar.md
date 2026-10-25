# FindBar {#find-bar}

[Русская версия](ru/find-bar.md) · [LineEdit](line-edit.md) · [Control examples](control-examples.md)

`FindBar` combines a one-line query editor, Previous/Next/Clear buttons and a
summary of the accepted query's results. The document overload owns one bounded
UTF-8 document and searches it synchronously. The [model-backed overload](#find-bar-model)
searches an immutable model snapshot on a worker. The first program below uses
the document overload; a second loads that control from scene text.

## Ordinary C++ program

Build `portals_find_bar_example` with examples enabled, then run it in a
terminal. Type a literal query and use the built-in buttons or F3 and
Shift+F3 to move through results. Both navigation directions wrap over the
stored results. F10 exits through the standard runner.

[Complete source](../examples/find_bar/main.cpp)

\include find_bar/main.cpp

The sample keeps both documents in the program and changes the source through
the control. It does not read or write files. The Russian sample also makes
case sensitivity visible: `портал` and `Портал` are different queries.

## Loading the control from a scene resource {#find-bar-resource}

The resource version declares `FindBar` by its built-in name. C++ obtains the
typed reference and connects two ordinary button handlers to replace the
document and query. A resource does not declare executable callbacks.

[Complete resource program](examples/guide_find_bar_resource.cpp)

\include guide_find_bar_resource.cpp

In `CONFIGURATION` resources, `FindBar` accepts `document`, `text`,
`maxDocumentBytes`, `maxMatches`, `previousLabel`, `nextLabel` and `clearLabel`.
It also accepts the ordinary editor settings `editMode`, `readOnly` and nested
`limits` (`maxInputBytes`, `maxClusterBytes`, `maxGraphemes` and `maxCells`).
The defaults are an empty document and query, a 65,536-byte source limit, 4,096
stored matches, the captions “Previous”, “Next” and “Clear”, `IMMEDIATE` mode,
editable input and query limits of 1,024 input bytes, 1,024 cluster bytes,
1,024 graphemes and 2,048 display cells. The hard source, query-byte and stored
match ceilings are 1 MiB, 16 KiB and 65,536 respectively.

Both forms use the standard scene runner. No client-side control registration,
terminal emulator, replay loop or persistent storage is involved.

## Matching and result limits

Matching is exact, case-sensitive UTF-8 byte matching, with both ends of each
accepted result required to be boundaries in the source's grapheme segmentation.
The control does not normalize Unicode: canonically equivalent strings such as
`é` and `e` followed by a combining acute accent remain distinct. It does not
case-fold, use locale rules, or apply regular expressions. A query is one line
and follows the normal `LineEdit` input policy. Source text may contain line
breaks (including CRLF) and tabs. The preview applies the terminal text policy to
control clusters and shows placeholders where appropriate; it is not a multiline
or tab-expanded viewer.

Overlapping matches count. In `aaaa`, the query `aa` has three results, starting
at byte offsets 0, 1 and 2. An empty query and a query with no matches are normal
states with no selected match. A newly accepted query selects its first result.
Navigation wraps only through retained results.

The default source limit is 65,536 bytes and the default stored-result limit is
4,096. The control retains the first results and detects whether another valid
match exists; when so, it reports a truncated count and does not claim an exact
total. Hard ceilings are 1 MiB for source bytes, 16 KiB for query bytes and
65,536 stored matches. A configured limit above its hard ceiling is invalid.
Query byte, grapheme, cluster and display-cell limits come from the ordinary
editor configuration; its default byte limit is 1,024. The zero source limit
admits only an empty document, and the stored-match limit must be positive.

Source bytes are admitted before `FindBar` copies them. A scene resource parser
or external text resolver may already have allocated its input before the
control can apply its own limits. Invalid UTF-8, an input-limit violation and
invalid configuration are reported as errors; empty and no-match results are
not errors. The document-backed form is synchronous and bounded by these limits.
For worker search over a model, use the overload below. Neither path provides
regex, replacement, automatic file loading or large-file indexing.

## Accepted queries and editing modes

The default mode is `IMMEDIATE`: accepted editor values publish the
corresponding search results. `ON_COMPLETION` and `EXPLICIT` keep the prior
accepted results while the visible draft differs from the accepted query. The
summary identifies that it is showing prior results, and navigation continues
through those results; it does not confirm the draft implicitly. Ordinary
`LineEdit` confirmation, focus-loss, Escape and Undo behavior remains in force.
In `ON_COMPLETION`, focus loss can confirm the draft according to the editor's
existing rules. See [editing modes](line-edit.md).

Clear is an explicit user command. It clears the query and results immediately,
including in deferred modes. Undo restores the query through the editor's
normal history; with a deferred mode, the restored draft's results change only
when that mode confirms it. Read-only mode blocks user typing, history and the
Clear command. A deliberate programmatic text assignment remains governed by
the existing editor policy. Read-only mode still permits document replacement
and navigation. When the whole control is disabled, user input, Clear and
navigation are blocked and its children do not receive focus or input.
Programmatic document replacement remains available while disabled or hidden.
Programmatic query assignment follows the editor's existing policy.

## Replacing documents and retaining matches

Each accepted document replacement creates a new result identity, even when
the replacement bytes are equal to the previous document. Existing ranges
remain readable from their owning `FindMatch` snapshots, but they no longer
identify a result in the current search. Publishing a changed accepted query
also creates a new identity; editing only a deferred draft leaves the prior
accepted results current. Navigation changes the selected ordinal within the
same result identity. The checked `current(match)` observation tests result-set
identity, independently of the current selection.

`FindMatch` is a small copyable owning handle. Its `text()` and `context()` return
borrowed views into that handle's immutable snapshot. A view stays valid only
while that particular handle remains alive and unassigned; copying the handle
extends the data lifetime only through the copy. It does not keep the scene or
control alive. A valid handle reports half-open UTF-8 byte offsets, its
one-based position, the number of stored results and whether that list was
truncated. `context(maxBytes)` defaults to 256 bytes, includes the selected
match and ends on grapheme boundaries; if the match alone is larger than the
requested budget, the context is empty. The summary explains when a selected
match is too large for its preview.

## Checked commands and diagnostics

`Ref<FindBar>` provides short `next()`, `previous()`, `clear()` and
`document(text)` commands for ordinary event handlers. They return `bool` and
report technical failures through the existing scene diagnostic collector.
The checked alternatives `navigate(direction)` and `replaceDocument(text)`
return typed results without automatically reporting technical failures.
`clearQuery()` returns the editor's `EditorCommandResult`: it reports technical
failures once and retains the diagnostic receipt; call `.result()` for its
checked `TreeError`. Empty and no-match navigation return no selected match;
they are not diagnostic failures.
Read and mutation operations follow the existing managed-reference thread and
frame rules.

The selected callback is synchronous and receives an owning `FindMatch` value.
It does not run for the initial mount. Query validation, search preparation or
publication failure keeps the prior accepted results; a rejected query remains
visible as the editor draft. After a coherent result is published, a selected
callback exception is surfaced as `CALLBACK_FAILED` but does not undo the
published search state. A presentation failure after publication likewise keeps
the accepted query and results; its original typed error is retained and the
selected callback is not invoked. Failures before publication keep the last-good
accepted results. Replacement editing and persistent storage remain outside this control's contract.
The following model-backed path has its own asynchronous preparation and failures.

## Search an immutable model in the background {#find-bar-model}

Include `<portals/ui_model_find.hpp>` for the additive model-backed overload.
Pass the scene's `ManagedModel<T>`, a shared immutable `ModelSnapshot<T>`, or an
owning checked `RowCollection<T>`, followed by a worker-only projection returning
`string_view` borrowed from `const T&`. The ordinary collection adapter creates its
model during checked mount; callers do not need to store jobs or binding tokens.

[Complete native source](examples/guide_model_find.cpp):

\include{lineno} guide_model_find.cpp

Search runs on the existing background executor, with no synchronous UI fallback.
The projection, provider reads and reachable captures must be transitively safe for
worker access and deletion. Do not capture a UI ref, scene, mutable model or
`ModelFrame`. An immutable source pin owns rows for the worker; the same shared
prepare pin is used by the scene's views. A matching completion is installed only
after current pins are known, so a source change in the same dispatch cannot admit
an old result. Query changes revoke the old identity even when the queue is full;
the newest intent retries without another key. Deferred editing modes retain their
ordinary confirmation boundary.

Defaults in `FindBarConfig::search` are 100,000 source rows, 32 MiB of projected
UTF-8, 1 MiB per row, 16 KiB per query and 4,096 retained matches. The editor's own
query limits also apply. Limits are configurable, distinct from the document-only
`FindLimits`. Literal matching stays case-sensitive and grapheme-aligned, without
normalization or fuzzy matching. Several matches may belong to one model row;
`FindMatch::rowId()`, `sourceRevision()` and `queryGeneration()` identify their
source and request. Retained match handles own their text, not a control.

An empty query starts no worker job. While a query is pending the bar says
**Searching...**. `searchFailed` receives the current `FindSearchError`; stale
successes and errors cannot replace a newer result. Backend/transport failures use
the existing typed managed diagnostics. `document()` is invalid for this overload:
publish a new immutable model snapshot instead. Loaded scene FindBar remains the
bounded document form; arbitrary model projection is supplied in C++.

Preparation does not promise constant-time UI reclamation: releasing the last
source/result owner can destroy its domain payload on UI. The byte caps cover
projected/result data, not arbitrary transitive storage inside `T`. Use immutable
provider ownership with a suitable release policy for large domain payloads.
