# Indexed live suggestions {#live-search}

[Русская версия](ru/live-search.md) · [Immutable models](model.md) · [Virtual lists](virtual-list.md) · [Document search](find-bar.md)

Use live suggestions to choose a command, document or instrument while typing.
An ordinary `LineEdit` supplies the query; an ordinary `VirtualList<SearchItem>`
shows bounded ranked results. Unlike `FindBar`, this composition does not count
matches or navigate occurrences inside a document.

## Complete native program

Build `portals_suggestions_example` with examples enabled. Type a command prefix
or a small typo, Tab to the results, select with arrows and confirm with Enter.
The selection label changes; the query and its Undo history remain independent.
F10 closes the application through the standard runner.

[Complete source](../examples/suggestions/main.cpp)

\include suggestions/main.cpp

Include `<portals/ui_suggestions.hpp>` for this optional model recipe.
`ui::suggestions(query.draftText(), source, projection)` follows visible input.
Use `committedText()` when results should follow accepted model values instead.
The source may be a managed model, an immutable owning snapshot or a
`RowCollection<T>` (including its checked construction result). Stable row IDs
identify domain rows, rather than positions in a changing result list.

## Worker preparation and ownership

The projection returns a `std::string_view` borrowed from the immutable row.
It runs on a worker; captures, snapshot reads and their deleters must be safe
there. Never capture UI refs or mutable model storage. The worker builds and
queries an index; the UI receives prevalidated display text, whole-grapheme
highlight ranges and a bounded top-K result. No UI formatter scans the corpus.

Changing the query reuses the same source index. Its exact key includes source
owner/pin identity, revision, projection session, normalization policy and width
policy. New source snapshots require a new index. Typing during a build replaces
the pending query without restarting the same build. Old completions and old
selection commands cannot silently act on a replacement source.

Large index storage belongs to the background executor. UI leases retain only
light control state. Retirement and idle eviction release index payload on a
worker; final shutdown may release remaining payload after the UI session and
worker join. A borrowed executor must provide `workerStorage()` support; there
is no fallback that builds or destroys a large index on the UI thread.

## Matching and finite work

The index canonically composes Unicode and applies full case folding, retaining
a mapping to complete original graphemes. It does not strip accents or apply
compatibility normalization. Full-label and whitespace-token prefixes are
ranked deterministically. `FUZZY_PREFIX` permits one insertion, deletion or
substitution for queries of at least three normalized graphemes; transpositions
are not treated as a single edit. `PREFIX` disables that tolerance.

`SearchOptions` bounds source rows, projected bytes, label/query lengths,
resident index storage, build scratch, query scratch, query work and highlights.
Defaults allow 100,000 rows, 32 MiB of projected text and 32 results; `topK` is
configurable from 1 to 256. Resident and build-scratch limits are 256 MiB each;
the default executor's 512 MiB storage budget is shared across all its indexes,
not reserved afresh per control. Query scratch has a separate default limit of
8 MiB per active query (up to 16 MiB with the default two workers). Source owners,
bounded result copies and thread stacks are separate; these are not process-RSS
limits. Idle entries may be evicted. Exceeding a work
or memory budget reports an error rather than presenting an unproven partial
ranking as an exact top-K. See the API fields for all byte/grapheme limits.

## Selection, status and errors

`SuggestionsConfig.status` optionally connects an ordinary label to status.
`activated` receives an owning `SearchSelection` after a current, focused-list
activation. Its bounded display text survives source replacement. `current()`
checks whether the selection still refers to the current source/query; it does
not rebase an old row or keep the whole source index alive. The application
chooses what selection means: navigate, execute a command or change another field.

`failed` observes the newest `SearchError` on the UI. Invalid input, unavailable
storage, cancellation and capacity failures retain the query. Empty input clears
the result list. The existing scheduler handles notification recovery and bounded
capacity; no additional polling timer or UI event loop is introduced. Unrelated
UI work, source acquisition and application callbacks remain the application's
responsibility and must not block the UI thread.
