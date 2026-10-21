# Planned editor contracts {#editor-contracts}

[Русская версия](ru/editor-contracts.md)

**Status: PLANNED.** This page specifies the proposed multiline editor contract.
It does not document an implemented `Document`, `TextArea`, language-provider API,
or an adopted syntax dependency. The initial implementation direction and its
qualification limits are described below. Semantic language, context and execution environment
form a separate deferred gate, G0S.

The proposal preserves C++23, the current UI runtime and renderer, and the
existing [LineEdit](line-edit.md) and [Undo/Redo](editor-history.md) contracts.
Names below identify concepts; they are not callable declarations.

## Document and view ownership

A document owns UTF-8 source text, newline metadata, a stable identity, its
`documentRevision`, and one shared undo/redo history. A view retains an owning
document handle and owns its caret, selection, viewport, focus and presentation
caches. Closing a view does not close another view of the same document.
Language annotations and publication state do not own the source text.

All document mutations, view resolution and notification delivery run on the UI
owner. Workers receive immutable owning snapshots; they never access a widget,
binding, mutable document, or borrowed UI memory. A worker snapshot may outlive
both the requesting view and document. Releasing it cannot invoke UI callbacks.

An accepted transaction atomically installs text, newline metadata, history and
mapped view positions before notifications. `documentRevision` advances on each
accepted source transaction, including undo, redo and external baseline adoption;
it never returns to an earlier value when earlier bytes are restored. Equal
ordinary replacements with equal bytes and newline metadata are no-ops. An
explicit new baseline, including equal
bytes from a new external value revision, advances the revision and resets
history. Identity and generation exhaustion reject the operation before mutation.

Model publication has its own baseline/revision and result. Publishing the same
document revision successfully neither edits the document nor resets history.
Provider result publication has a separate generation; applying tokens does not
advance `documentRevision`. No counter substitutes for these separate meanings.

## Transactions, external values and shared undo

An edit contains non-overlapping, half-open source ranges in one captured document
revision and owning replacement bytes. Ranges are validated before any change;
the transaction is all-or-nothing and enters one undo group when history is
enabled, possibly extending the current typing/deletion group. Paste, cut and
completion are explicit group boundaries. Adjacent typing/deletion may group only
in the same live initiating view; navigation, selection, focus changes, external
changes and edits from another view close the group. A new accepted edit after
undo removes redo; a no-op ordinary edit preserves it.

Empty ranges require an additional check: an insertion anchor must not lie in
the closed interval of any other edit in the same transaction. This rejects
multiple insertions at the same offset and insertions inside or at either end
of another replacement with a typed invalid-operation result, atomically.
Callers must explicitly coalesce intended ordered insertions into one owning
replacement before submission. Adjacent non-empty replacements remain legal;
they are interpreted together in source order against the captured revision.
Their shared source boundary maps to the output boundary between their two
replacement strings. History preparation coalesces generated inverse insertions
under the same rules and retains the checked inverse mapping for undo.

By default, an explicit external value is a **new baseline**, not a recorded user
edit: accept the whole value and reset both history stacks. Initialization also
uses a new baseline; ordinary programmatic edit/append records a transaction by
default (`RECORD`), while initialization/external baseline uses `RESET_BASELINE`. An
explicit per-operation override may select the other policy. A dirty document must report a
conflict when a newer model baseline is discovered; it cannot silently overwrite
either side. Resolving that conflict by explicit baseline adoption is deliberate
and atomic. The later binding API must expose this choice without reusing
LineEdit's confirmation keys as multiline behavior.

One document has one draft/history and one model bridge/commit policy shared by
all views; views cannot choose conflicting `EditMode` policies for the same
document. Confirmation publishes the current draft and preserves history. For
the proposed `ON_COMPLETION` behavior, blur of the initiating view performs one
commit attempt even when focus moves to another view of that document; the next
view observes the same draft and successful baseline. An unchanged clean draft
does not publish again. Reentrant focus events cannot repeat that attempt.
Explicit cancel deliberately adopts the latest model value as a new baseline
for all views and resets shared history; failure to obtain or prepare that value
preserves the current draft. These are planned multiline rules; existing
LineEdit behavior is unchanged.

Before calling the model publisher, the bridge owns a publication attempt with
a unique attempt identity, captured `documentRevision`, exact value (including
the relevant newline/export metadata), and expected model baseline/revision.
The publisher must condition its write on that expected baseline; an unrelated
external revision produces a typed conflict instead of a blind overwrite.
Only one attempt is active per document. A reentrant confirmation of a newer
draft waits for that attempt to settle rather than recursively publishing
against its unsettled baseline.

A successful receipt settles only the captured attempt/value. The current draft
becomes clean only when its document revision is still the captured revision
and the successful model baseline is still applicable. A newer reentrant edit
remains dirty until its own publication or explicit baseline resolution, even
if its bytes happen to match, and retains its own history, selection and revision.
Settlement cannot alter the state of a newer explicit baseline adoption and
never restores a snapshot into the current
document. If an incompatible external baseline is observed before settlement,
report a conflict and preserve the newer draft/model state; do not install the
attempted value over that baseline. A known successful model-write receipt is
reported separately from such a settlement conflict and cannot be rolled back.

Failure before the publisher runs changes no document or model state for that
attempt. Failure or a thrown callback after reentry preserves all independently
accepted edits and never marks a newer draft clean or resets its history or
selection. Without an authoritative write receipt, report a typed technical
publication failure with an unconfirmed outcome and obtain the latest baseline
before an explicit retry; do not infer that no model write occurred. A callback
failure after a known successful receipt is an observation failure for that
attempt, not permission to publish its value again. Exact result types remain
open.

Each history group owns before/after text and newline metadata, checked forward
and inverse position mappings for its edits, the original
initiating view's lifetime identity, and that view's before/after caret and
selection. It does not retain a raw view pointer or keep a closed view alive.
Undo/redo can be requested from any attached view and restores the shared
document. It restores the initiating view's saved caret/selection only if that
same lifetime is still attached to that document. Every other live view maps its
positions through the inverse/forward edit; this includes the requesting view
when it is not the initiator. Undo never moves focus to the initiator and never
restores another view's viewport. A dead initiator is simply absent.

Positions carry left/right insertion affinity. For replacement `[a,b)` with
replacement length `n`, positions before `a` remain; positions after `b` shift
by `n-(b-a)`; a position at `b` maps to `a+n` for a non-empty removed range.
Positions inside the removed range, including `a`, collapse to `a` with left
affinity or `a+n` with right affinity. At a pure insertion, affinity chooses the
side. A collapsed caret uses right affinity; a non-empty selection uses left
affinity at its lower endpoint and right affinity at its upper endpoint while
retaining active/anchor direction. After mapping, endpoints snap outward to
valid extended grapheme cluster (EGC) boundaries under the same affinity.
The initiating edit supplies its intended resulting selection explicitly.
Other viewports preserve their mapped top source anchor, then clamp scroll to
valid geometry; no edit implies a focus change.

## Source text, newlines and coordinates

The proposed canonical source uses UTF-8 with LF line separators, without Unicode
normalization. Import recognizes LF, CRLF and lone CR and records an owning
separator kind for every canonical LF. This preserves mixed files exactly on
unchanged export. A terminal newline is a real final separator followed by an
empty logical line; empty input and input ending in a newline remain distinct.

Default export preserves retained separator kinds. New typed separators use the
document's configurable preferred newline: the most frequent imported kind,
first occurrence on a tie, and LF for a new document. Raw pasted/imported ranges
retain their explicit separator kinds; programmatic canonical replacements use
the preferred kind unless the caller supplies checked metadata. Deleting a
separator deletes its metadata; undo/redo restores it. An explicit normalized
export may select LF or CRLF without modifying the document. Changing preferred
newline affects future insertions only. A separate explicit source transaction
is required to rewrite existing separator metadata and is undoable.

Tabs remain U+0009 in source. The proposed default is literal-tab insertion and
four-cell tab stops; spaces-on-Tab is configurable. Existing tabs are never
silently converted. Tab stops are computed from logical line start before
viewport clipping. Soft wrapping is off by default; if enabled, visual row is
another coordinate and does not become a logical source line.

| Coordinate | Meaning and conversion rule |
|---|---|
| UTF-8 byte offset | Offset into canonical source, with explicit document revision; ranges are half-open. |
| Unicode scalar boundary | Valid decoded UTF-8 boundary; never the middle of a multibyte sequence. |
| EGC boundary/index | Editing and selection unit using the project's supported segmentation tables; byte length and display width can differ. |
| Logical position | Zero-based logical line plus an explicitly named UTF-8-byte, UTF-16-unit or EGC column; a bare “column” is insufficient. |
| UTF-16 service position | Zero-based line plus UTF-16 code units within that line; a surrogate pair counts as two units. |
| Visual position | Terminal-cell column/visual row after tab expansion, width policy and optional wrap; not a service offset. |
| Imported byte offset | Original serialized source offset, translated through newline metadata; CRLF is two imported bytes and one canonical LF. |

Conversions are checked against the captured snapshot, line bounds, encoding,
integer range and requested boundary kind. Out-of-range positions and the middle
of a UTF-8 sequence, UTF-16 surrogate pair or required EGC are rejected, never
silently clamped. Visual hit testing inside a tab or wide EGC uses explicit
left/right affinity and yields an EGC boundary. Provider token ranges may use
scalar boundaries; rendering can cover the containing EGC, but edits must still
pass EGC validation. The adapter explicitly declares UTF-8 or UTF-16 service
encoding; no implicit cast connects coordinate types.

Changing width policy, tab width, viewport, theme or wrapping invalidates the
relevant layout caches; it does not edit source, advance `documentRevision`, or
create undo history. The implementation must pin its supported Unicode version;
this proposal does not upgrade the existing tables. See
[Unicode text segmentation](https://www.unicode.org/reports/tr29/) and the
[LSP position contract](https://microsoft.github.io/language-server-protocol/specifications/lsp/3.17/specification/#position).

## Markdown regions and reversible mapping

A language region has a document identity/revision, stable-within-generation
region identity, mapping generation, language identifier and context generation.
Fence delimiters and info strings belong to Markdown; the embedded provider sees
the fenced body. Alias matching uses an explicit registry; unknown aliases use
plain text. Changing a fence, nesting, prefix, deindent or language invalidates
the old region identity/mapping before provider results can apply.

The adapter owns a piecewise map from embedded source to host source. Each
source-backed segment records both ranges and coordinate conversion data;
stripped indentation, blockquote/list prefixes and synthetic characters are
explicit gaps. A single numeric offset cannot represent these discontinuities.
Highlighting maps source-backed portions separately and never colors stripped
prefixes as embedded text.

For the first implementation, an embedded edit is admissible only if its entire
range and insertion boundaries map uniquely into one contiguous source-backed
segment. Reject ranges that cross removed prefixes, deindent gaps, synthetic or
unmapped text, ambiguous endpoints, or region boundaries. In a transformed region,
also reject inserted newlines that would require synthesizing host prefixes.
Direct source editing remains available. Multiple completion edits must all pass
these checks and remain non-overlapping in host coordinates before **any** edit
is applied; a rejected additional edit rejects the entire completion. Return a
typed mapping failure rather than guessing a host insertion. A future richer
inverse map requires its own demonstrated contract and fixtures.

## Asynchronous providers and callback lifetime

Syntax, semantic tokens and completion are independent capabilities. Each request
owns its source snapshot, region map, options and context snapshot or an immutable
owning context handle. Its envelope captures document identity/revision, region
identity/mapping generation, context generation, provider/session generation and
request generation. A completion request additionally captures a generation-safe
view lifetime token and the original caret/selection plus navigation generation.
Owning results repeat the envelope and declare their coordinate encoding.

Workers publish bounded owning messages through the existing mailbox/routing
path. They do not create a second event loop/runtime, capture raw widgets, or
deliver UI callbacks directly. Worker cancellation is advisory; UI admission is
not. Explicit cancel immediately and irrevocably revokes that request's right
to publish, including any derived completion proposal, even without a successor
request. Enqueued or racing replies cannot revive that right. Retrying always
requires a new request generation.

On the UI owner, resolve the document and, for completion, the view lifetime;
then compare every captured guard. A view must still show the captured document.
Navigation, including moving away and back to the same caret, advances both
request and navigation generations, invalidating the completion request.
Closing/remounting a view, changing text/context/region or
issuing a newer request rejects the old result. The initial implementation does
not rebase stale results. Syntax/semantic publication is document-scoped and
does not need a live requesting view, but still requires live document and all
document/region/context/provider/request guards. Closing the last owner leaves
only a disposable worker snapshot, never an actionable document.

Every request has a single-use UI admission state. For every result kind,
including syntax, first check that the request remains pending and unrevoked
alongside its captured guards. Mark an admitted reply consumed before publishing
its result or invoking any callback, so duplicate/reentrant deliveries receive
a typed consumed/stale result. A revoked request receives a typed cancelled/stale
result. Processing failure does not reopen a consumed request; an explicit retry
uses a new generation. A consumed completion reply may create a separately live,
single-use proposal governed by the same captured guards. Reply consumption does
not authorize a later acceptance after cancellation, navigation or invalidation.

Completion acceptance validates the live proposal and guards again immediately
before committing one transaction, and consumes that proposal before mutation
or callback delivery. It invokes no application validation callback, notification or
other arbitrary callback between final guard validation and state installation.
After installation, deliver
owning observations through the current UI callback ordering. A callback may
reenter or remove the view/document; resolve lifetime again before subsequent
delivery and do not repeat an edit to retry a failed notification. Stale results
are discarded with a typed status available to the application; transient
provider failures retain usable text and fallback syntax and allow an explicit
retry with a new request generation. Queue saturation must report rejection or
coalesce replaceable work; it cannot block input or grow without bound.

## Errors, budgets and visible work

Read-only views reject user mutation, cut, completion acceptance and undo/redo;
copy, navigation and selection remain available. Another writable view may edit
the same document and its changes are mapped into the read-only view. Explicit
programmatic document edits remain possible unless a separate document-level
write policy forbids them. A request/result alone never grants write authority.

Prepare UTF-8 validation, EGC boundaries, newline metadata, all range mappings,
history admission and allocation before acceptance. Invalid text/coordinates,
invalid operations, stale identity/revision, consumed/revoked requests,
read-only access, ambiguous mapping, limits, history
capacity, generation exhaustion and OOM have distinct typed failure categories.
Before acceptance, a failure leaves text, metadata, revision, all view positions,
history and redo unchanged. Never truncate a whole edit or silently drop its
history record. Complete oldest groups may be evicted only as part of a successful
admission; explicitly disabled history accepts unrecorded edits. Publication or
callback failure after acceptance reports the admitted edit separately and does
not roll it back or apply it twice. Exact C++ result types remain open.

The following are **proposed configurable defaults and evaluation budgets**, not
measurements, implemented limits or a performance promise. G0 requires a storage
accounting model and standalone provider evidence; it does not require the future
Document/TextArea implementation. Actual edit/OOM/view-render guarantees and UI
timing targets belong to acceptance of the later TextArea/syntax implementation.
The one-line editor's limits are unchanged.

| Setting | Proposal | Reason / behavior at the bound |
|---|---|---|
| Canonical document bytes | 1 MiB | Bounded first multiline workload; reject oversized admission. |
| Logical lines | 65,536 | Bound indexing separately from byte size, including final empty line. |
| One logical line | 256 KiB UTF-8 | Exercise long-line behavior without an unbounded line scan. |
| History | 128 groups, 32 MiB retained storage | Two 1 MiB full snapshots cannot fit the current one-line 1 MiB history budget; account for metadata, entry overhead and retained capacities. Reject an oversized whole group. |
| Pending provider work | 1 active + 1 replaceable newest request per document/capability | Bound burst work; cancellation does not authorize unbounded backlog. |
| One provider result | 4 MiB including owned ranges/strings | Bound validation/publication allocation; oversized results fail as a whole. |
| Completion list | 256 candidates within result byte budget | Bound popup/model work; truncation requires an explicit incomplete-list flag. |
| Typing syntax debounce | 50 ms | Coalesce bursts; explicit completion is immediate. |
| UI edit preparation p95 | ≤ 8 ms for the declared evaluation workload | Later TextArea acceptance target using a 60 fps / 16.7 ms evaluation frame; this is not the Portals default frame profile. |
| Incremental syntax p95 | ≤ 50 ms on the declared workload | Evaluation target for responsiveness, independently of debounce. |
| Syntax worker scratch | ≤ 32 MiB above owned document/result storage | Evaluation budget; measure peak native and WASM memory separately. |
| Added compressed WASM | ≤ 2 MiB for syntax runtime and all required grammars | Candidate selection budget relative to the same syntax-free build. |

Rendering submits visible lines/runs to the existing renderer; it creates no
widget per token. Hidden views skip visible layout/render preparation and never
request continuous redraw. They still receive document-position mapping and
cache invalidation needed for correctness. Document-level provider work may
continue for an active consumer; hiding a view alone does not edit the document.
Rope/piece-table storage, delta history and a new renderer range API require a
measured need rather than being prerequisites of this contract.

## Provider decision and remaining gates

The required comparison covers a state-carrying line lexer and Tree-sitter for
Pascal, Markdown and C++: edit/recovery correctness, invalidation extent,
latency, retained/peak memory and total native/WASM size under the same pinned
fixtures/toolchain. Tree-sitter's documented incremental trees and included
ranges are useful capabilities, not Portals measurements; see its
[advanced parsing guide](https://tree-sitter.github.io/tree-sitter/using-parsers/3-advanced-parsing.html).
Markdown's split block/inline grammar and its documented WASM integration caveat
must be evaluated; see the [upstream README](https://github.com/tree-sitter-grammars/tree-sitter-markdown/blob/a0a00f817d02412bd92c54d316f164d827b57b5c/README.md).

**Initial direction: a dependency-free incremental line-state lexer**, with
plain-text fallback for unsupported regions. This is a development decision,
not an implemented tokenizer. Tree-sitter remains a deferred candidate; no runtime
or grammar dependency is added by this contract.

Tree-sitter's pinned runtime documents that its default allocator aborts on
allocation failure. Returning null from a custom allocator does not establish a
recoverable boundary: parser creation and subtree copying include unchecked
uses of allocated memory. This conflicts with the proposed typed-failure contract.
Reconsideration requires demonstrated bounded storage, safe failure containment
and a typed diagnostic boundary, including scanner allocations. The relevant
[allocator contract](https://github.com/tree-sitter/tree-sitter/blob/7f877d3c080f7f8c7764ee1f68002fc52636cacb/lib/include/tree_sitter/api.h#L1498)
and [implementation](https://github.com/tree-sitter/tree-sitter/blob/7f877d3c080f7f8c7764ee1f68002fc52636cacb/lib/src/alloc.c)
are pinned to the evaluated revision. No crash or out-of-memory execution is
claimed by this source inspection.

Unknown or failed syntax regions render as plain text; absent semantic results
retain current syntax. Old-revision token spans never apply to changed text.
Provider absence or failure must not block editing.

### Bounded comparison

The standalone comparison used the runtime revision linked above, Pascal grammar
`042119eca2e18a60e56317fb06ee3ba5c32cb447`, Markdown grammar
`a0a00f817d02412bd92c54d316f164d827b57b5c`, and C++ grammar
`c009222808634c1014f82438d4883753516a2c24`. MSVC 19.51 Release and
Emscripten 6.0.12 with Node 24.21.0 used optimization level 2. Each platform
ran three separate latency processes, with 3 warmups and 15 samples per case:
63 cases covering 1 KiB, 64 KiB and 1 MiB inputs, cold parsing, ordinary edits,
and state-invalidating edits. Each process also passed 12 boundary fixtures.

The table shows the median of the three per-run p95 values for an ordinary
fixed-width edit in a 1 MiB input, in milliseconds:

| Language | Native line-state prototype | Native Tree-sitter | WASM line-state prototype | WASM Tree-sitter |
|---|---:|---:|---:|---:|
| Pascal | 0.754 | 11.274 | 0.275 | 6.884 |
| Markdown | 0.698 | 284.747 | 0.552 | 206.113 |
| C++ | 0.157 | 10.436 | 0.190 | 5.219 |

At 15 samples, nearest-rank p95 is the largest sample; these are descriptive
observations, not stable tail-latency estimates. The line-state prototype
computes states and fingerprints, not syntax tokens. Equality against each
engine's own fresh result passed; it does not establish equivalent language
features or grammatical correctness. Edits were fixed-width and on one line.
Timing excludes text replacement, coordinate conversion, parser creation, UI
work and destruction. Production token fixtures, changing line counts and lengths,
failure handling and end-to-end input latency remain implementation checks.

Separate instrumented runs observed maximum runtime-requested live payload of
174,589,568 bytes native and 167,038,844 bytes WASM. The hook omits the C++ harness
and unproven scanner allocations, and its allocate-copy-free reallocation changes
transient memory use. These values neither measure process peak RSS nor validate
the proposed 32 MiB scratch budget. Instrumented latency is excluded above.
The temporary combined runtime/grammar/harness WASM was 4,116,577 bytes
(901,646 gzip bytes); the lexer comparison module was 207,883 bytes
(71,676 gzip bytes). These are standalone artifacts, not changes to the Portals
application size or additional shipped modules.

The runtime and grammar root licenses are MIT. The runtime's
[Unicode data notice](https://github.com/tree-sitter/tree-sitter/blob/7f877d3c080f7f8c7764ee1f68002fc52636cacb/lib/src/unicode/LICENSE)
is separate and must be retained if those sources are distributed. This inspection
does not adopt or distribute these dependencies.

### Contract evidence and implementation gates

A standalone reference model exercised 76 traces comprising 227 steps, including
transaction inversion, reentrant publication, stale replies and cancellation.
All 46 bounded model projections matched their expected results; 32 complete
model scenarios passed and 14 retain unexecuted production requirements. Five
storage-accounting cases passed using declared capacities and overheads, not
measured C++ allocations. Four deliberately faulty variants were detected.
This evidence checks the proposed rules. It does not test the future editor's
Unicode segmentation, actual allocation failures, callbacks, worker destruction,
visual layout or rendering. Independent contract review covered ownership,
reentrancy, request admission and the provider-failure boundary.

**G0S: DEFERRED.** The minimum semantic language, project/context inputs and
execution environment remain unselected. Recovery is to select and validate a
real backend/context/environment before semantic implementation starts. This
blocks the semantic-provider task, PRTLS-161, while syntax-only work may proceed
after the base contract gate. The base evidence above permits syntax-only
implementation to proceed; it does not stabilize new public API signatures.
The later implementation must exercise these rules as runtime regressions and
validate its actual allocations, view work and UI timing. Those production checks
remain unexecuted.
