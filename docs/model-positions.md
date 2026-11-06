# Keep a position independently of a view

[Русская версия](ru/model-positions.md) · [Model data](model.md)

A row number describes an order in one model revision. A `RowId` identifies a
record. Inserting another record before it changes its row number without changing
its identity. Use a model bookmark when a position must survive that distinction.

This page describes the provider-level navigation foundation in
`<portals/model_position.hpp>` and `<portals/model_position_finite.hpp>`.
Creating or resolving a bookmark does not change a control, selection, model, or
viewport. Automatic view restoration and shared UI cursors are separate integrations.

## What a bookmark retains

`ModelResource` identifies a logical resource and its compatible history, called
its lineage. Its identity uses the existing `SourceStamp` namespace. A document
reopened at the same path is not necessarily the same resource. Providers retain
the lineage only when they can relate the old and new positions.

`ModelPosition` distinguishes an item, a gap between items, and a text mark. It
carries a data revision and a coordinate hint; the coordinate alone does not prove
where the item is now. Item positions also carry the full `RowId`, including its
generation. Equal row IDs in different resources are unrelated.

`ModelBookmark` owns the position and optional immutable provider metadata. It
retains no view, source snapshot, model rows, or window pages. Copies share its
immutable metadata. The configurable default admission limit is 4096 bytes;
`storageBytes()` reports charged storage, not process resident memory.

An in-memory bookmark does not reopen a file or serialize its data. A provider
needs a portable identity and an export/import codec for restoration between
processes. `SourceCursor` remains a gap token for one immutable delivery session;
it is not a replacement for a resource bookmark.

## Resolve a position

`PositionResolver` accepts a `PositionRequest` and returns a qualified
`PositionReply`, or a typed `SourceError`. The request includes the current source
session, target revision, request sequence, and bookmark. Before applying an
asynchronous reply, use `positionReplyMatches` against the still-current request.
An old reply must not change a newer navigation request.

| Outcome | Meaning |
|---|---|
| `EXACT` | The same record or a proven mapped boundary is available. |
| `NEAREST` | The record was removed and a surviving neighbor was resolved. |
| `EMPTY` | A compatible source has no current record. |
| `PENDING` | Required bounded data is not ready. |
| `UNSUPPORTED` | This provider does not support the requested position kind or lookup. |
| `INCOMPATIBLE` | Resource identity, lineage, or revision proof cannot establish the position. |
| `UNAVAILABLE` | The provider supports navigation but the required history or storage is unavailable. |

The interface requires no total row count. A streaming provider can resolve its
own keys or boundaries without inventing a finite extent. Resolution that performs
I/O belongs on the existing source worker path. Returning `PENDING` must not block
the UI, and resolution must not scan an entire source on the UI thread.

## Adapt a finite immutable snapshot

`FinitePositionResolver<T>` uses a `ModelSnapshot<T>` and its bounded
`locateRow` implementation. It keeps a weak snapshot reference, so a bookmark or
resolver does not keep a closed source alive. The snapshot must meet the model's
worker-safe read and destruction requirements.

If the full row identity is still present, lookup preserves it across insertion
and reordering. If it is absent, nearest-neighbor restoration needs a complete
ordering proof. `PositionChangeMap` records provider-declared splices and moves
between two revisions. It validates bounds and arithmetic; the provider remains
responsible for declaring all changes truthfully. Its configurable default limit
is 1024 operations. Mapping work depends on the operation count, not the row count.

For a forward-affinity position, removal selects the following surviving item,
falling back to the preceding item at the end:

| Before | Change | Result |
|---|---|---|
| A B **C** D | Remove B | C remains current. |
| A B **C** D | Remove C | D is nearest. |
| A B C **D** | Remove D | C is nearest. |
| A B **C** D | Remove C and D | B is nearest. |
| A B **C** D | Remove all | No current item. |

Backward affinity prefers the other side. Gap positions use insertion affinity
without selecting a row. Without a compatible change proof, the adapter does not
silently clamp an old ordinal into a new revision. A provider without identity
lookup reports the missing capability rather than scanning the model.

The finite row adapter does not resolve text marks. A text provider must preserve
grapheme boundaries and define how edits move its marks. Editing, Undo/Redo,
selection synchronization, and bookmark persistence are not performed by this
foundation.

For shared current and reading positions, see [model navigation](model-navigation.md).
