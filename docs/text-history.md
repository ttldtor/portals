# Text history

`TextHistory` is a specialized owning history for UTF-8 text and selection. This low-level component stages text edits and traversal before publication. Editor bindings, shortcuts, model publication and effective availability are separate integration responsibilities; see [LineEdit](line-edit.md) for the current editor contract.

## Values and ownership

Each `TextHistorySnapshot` owns `bytes`, `cursor` and `anchor`. Offsets are byte positions at extended grapheme boundaries, including the end of the text. Preparation validates both edit snapshots using the `WidthPolicy` and `TextLimits` captured by the constructor; restoration validates its target again. Original bytes are preserved without normalization.

Retained entries own before/after snapshots. They contain no editor, model, widget, callback or external buffer references. Immutable entries are shared internally so preparation can copy entry handles without copying every retained string. `PreparedTextHistory` is an owning, move-only handle; destroying it abandons preparation without changing the history. Its move operations are `noexcept`.

The caller must synchronize access. `TextHistory` neither enforces UI-thread affinity nor decides whether an editor is read-only, disabled or in conflict.

## Configuration and bounds

Construction is `explicit TextHistory(TextHistoryOptions options = {}, WidthPolicy policy = {}, TextLimits limits = {})`. These settings are captured at construction; this API has no configuration setter.

| Option | Default | Meaning |
| --- | --- | --- |
| `enabled` | `true` | Retain edits |
| `maxOperations` | `128` | Combined Undo and Redo groups |
| `maxBytes` | `1048576` | Combined logical record budget in bytes |
| `groupTyping` | `true` | Merge eligible adjacent typing |
| `groupDeletion` | `true` | Merge eligible adjacent deletions of the same direction |

The named defaults are `DEFAULT_TEXT_HISTORY_OPERATIONS` and `DEFAULT_TEXT_HISTORY_MAX_BYTES`. A group counts as one operation, regardless of how many edits it combines. `operationCount()` and `retainedBytes()` report both Undo and Redo storage.

The byte charge for each retained entry is `sizeof(Entry) + before.bytes.capacity() + after.bytes.capacity()`. It excludes container storage, shared ownership control blocks, allocator bookkeeping, the current editor text and temporary preparation allocations. Capacities depend on the standard library. This is a logical record budget, not an allocator measurement or a total RSS limit; outstanding prepared values can also keep older shared entries alive.

Preparation evicts whole oldest groups in its prospective state until both limits hold. It never truncates an operation. If a single record cannot fit a finite nonzero byte budget, preparation returns `WOULD_BLOCK`. If only a merged record is too large, preparation retries the new edit as a separate record. A failed preparation leaves the live history and Redo branch intact.

With `enabled == false`, `maxOperations == 0` or `maxBytes == 0`, valid edits prepare successfully without retaining records or rejecting an oversized record. Committing such an edit installs an empty history. Text validation and allocation failures remain possible.

## Grouping, traversal and reset

Only `TYPING`, `BACKSPACE` and `FORWARD_DELETE` can merge, subject to their grouping options. A merge requires the same category, an open group, collapsed selections in both snapshots, and exact agreement between the preceding after snapshot and the new before snapshot: bytes, cursor and anchor. Opposite deletion directions do not merge. `PASTE`, `CUT`, `REPLACEMENT` and `PROGRAMMATIC` are separate operations.

There is no timer-based grouping. The editor calls `stopGrouping()` at its editing boundaries, such as navigation, selection or focus changes. `TextHistory` cannot infer those events. Committing a prepared Undo or Redo closes the group. An empty traversal does not change history; an editor must explicitly close its group for an empty command.

Under enabled retention, equal before/after bytes are an ordinary history no-op: `changed()` is false and Redo is preserved, even if the validated target selection differs. A successful new text edit after Undo discards prospective Redo. `prepareResetBaseline()` clears every group even when baseline bytes are unchanged; its `changed()` is true. Disabled or zero-limit retention takes precedence over ordinary equal-text no-op handling.

`prepareUndo()` and `prepareRedo()` restore the corresponding owning snapshot without recording a second edit. They return `EMPTY` when that direction has no operation. `canUndo()` and `canRedo()` describe local history position only; `TextHistory` does not translate `EMPTY` into an editor-level successful no-op.

## Prepare, check and accept

`prepareEdit(before, after, kind)`, `prepareUndo()`, `prepareRedo()` and `prepareResetBaseline(baseline)` return `std::expected<PreparedTextHistory, TextHistoryError>`. Preparation performs allocation, segmentation and prospective history changes before publication. It does not mutate the live history.

The caller accepts a prepared result in one uninterrupted, synchronized interval:

1. Check external editor/model eligibility and revisions, then call `history.checkPrepared(prepared)` before replacing any text.
2. Read the target `cursor()` and `anchor()` before the handle is consumed.
3. Call `prepared.takeText(existingDestination)` and install the target selection in already existing editor storage.
4. Call `history.commit(std::move(prepared))` without intervening callbacks or history mutations. Only then run validation/publication callbacks required by the editor layer.

`takeText(GraphemeText &destination) noexcept` uses move assignment into an existing value. A second transfer is a no-op. `commit(PreparedTextHistory &&prepared) noexcept` rechecks validity and consumes staging on success. Checking, transfer and commit do not allocate or invoke callbacks. Constructor-time creation of the destination is outside this acceptance interval.

`checkPrepared()` and a rejected `commit()` leave history and staging unchanged. If the caller mutates history after checking but before committing, commit can return `STALE`; it does not roll back text already transferred by the caller. The uninterrupted acceptance interval is therefore part of the caller's contract.

## Failures and freshness

Construction and caller-side snapshot construction may throw. The prepare methods catch `std::bad_alloc` and `std::length_error` and return `NO_MEMORY`. Other technical results are `INVALID_TEXT`, `NOT_BOUNDARY`, `WOULD_BLOCK`, `EMPTY`, `STALE` and `EXHAUSTED`.

Prepared validity uses an owning identity token, history generation and grouping state. A prepared value from another owner or an outdated generation is `STALE`. A changed commit advances generation. `stopGrouping()` closes the group and advances generation when possible; the counter saturates rather than wrapping. A regular no-op commit does not advance it. Generation exhaustion is reported as `EXHAUSTED` for operations requiring advancement.

History generation is not a model revision. `TextHistory` has no model snapshot to inspect. When retained entries exist, an ordinary changed edit checks that its before bytes match the current history position, but this cannot detect an external model update to equal bytes. The editor integration must enforce model revision conflicts and adopt external baselines explicitly.

## Cost

Preparing a changed edit or traversal copies O(retained groups) shared entry handles. New or merged entries copy their own before/after strings; unchanged retained strings are not recopied. Snapshot validation and restoration segmentation still cost work proportional to the relevant text. Count limits bound the group traversal, not the size of temporary text allocations. Acceptance avoids allocations, but releasing old state can destroy entries and free storage; it is not a constant-time latency guarantee.

