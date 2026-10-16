# Bounded model queries {#model_queries}

[Русская версия](ru/model-queries.md)

Include `<portals/model_query.hpp>` for an experimental, explicit query layer
over immutable indexed snapshots. It keeps the existing model and scheduler.
It does not introduce a query language, database, implicit I/O, or another
reactive runtime.

## Source, criteria and output page

`Query<T, Q>` owns the exact source snapshot, domain criteria `Q`, and a
`RowRange` within the filtered and sorted result. `Q` and reachable worker data
must own their storage, remain immutable while shared, and permit destruction
on the last owner's thread. Do not put borrowed UI references into criteria.

`transformSnapshot` performs a local filter, stable sort and page operation.
Pass a predicate `bool(const T&, const Q&)` and a strict weak ordering
`bool(const T&, const T&, const Q&)`. Equal keys retain source order. Output
rows preserve their full `RowId`, including generation. Local evaluation
requires copy-constructible rows because the output page owns contiguous
reordered values. It never calls a provider to load pending source rows.

The result is `expected<QueryPage<T>, QueryError>`: an owning page at the
requested source revision and the filtered count before paging, or a typed
evaluation failure. Callback exceptions, cancellation, allocation failure,
pending source and budget exhaustion are distinct outcomes.

| Default limit | Value | Meaning |
| --- | --- | --- |
| `maxInFlight` | 4 | Reservations, including cancelled tokens still retained by workers |
| `maxOutputRows` | 256 | Requested and returned page size |
| `maxSourceRows` | 65536 | Rows examined by local evaluation |
| `maxComparisons` | 1048576 | Comparator calls in one local evaluation |

All limits must be positive. The default page is `{0, 256}`. These bounds do
not bound callback duration or total process memory; providers must also keep
their own work bounded. A large indexed source can use a custom provider rather
than a full local scan.

## Apply on the existing UI scheduler

`QueryController<T, Q>::create(tree, context, source, provider, options)` creates
one long-lived existing `Model<T>` and bounded completion endpoints. The tree
must outlive the controller. Create, submit, pin, retry and close on its UI
owner thread; a worker may complete its owning request token.

`MemoryQueryProvider` uses the local transformation. A custom `QueryProvider`
receives `QueryRequest` tokens and may produce delayed results. A superseded
request is cooperatively cancelled. Its reservation remains charged until
all worker and queued owners release it. Late or stale completion cannot
replace a newer accepted query.

Successful submission or notification means queue acceptance, not immediate
application. Dispatch the existing scheduler and call `retryPublication()`
to recover retained notifications and observe application. A rejected
notification or failed model allocation preserves the pending result for
retry. Check typed return values and `lastError()` / `lastQueryError()`.
The currently applied model remains available while a query is pending.

`pin()` returns the ordinary immutable model frame. `snapshot()` supplies a
`QuerySnapshot<T>` with provenance. Its `sourceRevision()` identifies original
data; its model revision identifies the accepted projection. Different filter,
sort or page results can share a source revision and still need different
projection revisions. Never compare these two revision domains as timestamps.

The initial model presents the original source. After a query, its row count
is the returned page size. `totalRows()` records the filtered count before
paging. This is explicit paging, not an automatically pageable managed virtual
view. `openRange()` remains the existing low-level model consumer.

## Complete installed consumer

The [installed-package consumer](https://github.com/ttldtor/portals/blob/main/tests/package_consumer/query.cpp) filters
prices, sorts descending, requests two rows, dispatches the scheduler, checks
source and projection provenance and full row identities, then closes.
The same file contains its complete immutable source implementation.

Close cancels work and disconnects endpoints without waiting for worker joins.
Owning tokens can outlive the controller, but must not capture its UI object.
This slice does not provide automatic history rebasing, a managed-model DSL
bridge, SQLite, arbitrary query parsing or external executor integration.
