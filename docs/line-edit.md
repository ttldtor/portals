# Unicode line editing

`LineEdit` owns a UTF-8 draft and exposes byte-boundary cursor and anchor positions.
Its `GraphemeText` segments editing into complete Unicode graphemes; terminal width
and horizontal scrolling use cells. This includes combining marks, ZWJ emoji and
CJK. Construct the widget, create a `Property<std::string>`, and retain the result
of `bindLineEdit` for as long as input and observation should remain connected.

```cpp
const auto field = tree.emplace<portals::LineEdit>(parent).value();
auto model = std::shared_ptr<portals::Property<std::string>>(
  portals::Property<std::string>::create(tree, parent, "", portals::Invalidation::LAYOUT).value());
auto binding = portals::bindLineEdit(
  tree, router, field, model,
  [](portals::LineEditResult result) {
    // A COMMITTED receipt may also report rejected observer notifications.
  }).value();

// Synchronize after draining events and before the next immutable frame.
binding.sync(router.focused().value()).value();
```

The tree, router and scheduler outlive the binding. The binding retains the model
and optional clipboard service. Destroy or move it on the UI owner between frames.
Widget text references borrowed through `tree.read` must not escape that callback.
Render commands may borrow widget text until the enclosing `tree.frame` returns;
the frame owner translates local commands and intersects the ancestor clip, as in
the control gallery. Paint never reads the model, invokes a service or changes
application data. Focus is a presentation snapshot supplied by the caller, while
the router remains the authority for input eligibility.

## Draft and model revisions

`LineEditOptions::editMode` selects when technically accepted text is validated
and published. The default is `EditMode::IMMEDIATE`: input, deletion, clipboard
cut/paste and fluent text changes publish valid changed candidates without Enter.

| Mode | Text change | Blur | Enter / programmatic confirmation |
|---|---|---|---|
| IMMEDIATE (default) | Validate and publish | Keep any rejected intermediate draft | Validate and confirm |
| ON_COMPLETION | Keep dirty draft | Validate and confirm once | Validate and confirm |
| EXPLICIT | Keep dirty draft | Keep draft | Validate and confirm |

Choose the previous behavior explicitly:
`LineEditOptions{.editMode = EditMode::EXPLICIT}` or
`ui::lineEdit(model, {.options = {.editMode = EditMode::EXPLICIT}})`.
Programmatic `router.invoke(field, {Action::ACTIVATE})` uses the same handler as
Enter in every mode, delivering its exact `LineEditResult` to the result callback.

Technical text rejection preserves the previous draft and selection. Semantic
validation false instead retains the new visible candidate with VALIDATION_FAILED
and no model publication: a temporary empty string or minus sign remains editable.
Validator/publication exceptions report typed FAILED outcomes and retain accepted
text. Confirmation checks the original model revision both before and after the
validator; CONFLICT never silently overwrites an external update.

Escape adopts the latest valid model in every mode. In IMMEDIATE it does not undo
earlier successfully published changes. Successful immediate publication makes
the draft clean and advances its baseline to the exact receipt's revision. A
rejected/conflicting candidate remains dirty until confirmation or Escape.

Blur is synchronous at the existing router focus transition, before the next
routed action; A→B→A before a frame still confirms A once. Model notifications
and `binding.sync` are not synthetic blur events. Focus presentation still
synchronizes before frames. Equal text does not cause another publication or
redraw; explicit confirmation can return an unchanged PropertyUpdate receipt.

An external update refreshes a clean editor. A dirty editor retains its draft
through external updates, queued notifications and explicit `sync`. Notification
callbacks read the current snapshot rather than applying their possibly obsolete
payload. Call `sync` before every frame because bounded notification delivery can
be rejected. A successful commit remains successful when notifications are lost:
`LineEditResult::commit` carries the exact `PropertyUpdate<std::string>` receipt.
Do not retry the commit merely to deliver notifications.

`LineEditResult::trigger` identifies INPUT (including programmatic activation),
FLUENT or BLUR independently of its status. Managed technical failures carry a
`lineEdit.input`, `lineEdit.fluent` or `lineEdit.blur` breadcrumb when context
allocation succeeds, and preserve their original typed cause and the editor's
identity. A blur failure propagated through focus recovery in `scene.prepare()`
is recorded once; unrelated prepare failures remain separate diagnostics. The
existing managed stop policy is unchanged.

A validator or clipboard service can reenter UI code. The editor checks node
lifetime after the callback, and Enter rechecks model revision after validation.
Result callbacks run last and may remove the current node or an ancestor, or
destroy the binding. Already committed application effects are never rolled back
when such a callback throws. Mutation through `sync` during a frame or an active
input operation returns `WOULD_BLOCK`. Wrong-thread synchronization is rejected.

## Input and clipboard

- Left/Right, Home/End move by grapheme boundaries; Shift extends selection.
- Backspace/Delete remove complete selected or neighboring graphemes.
- Ctrl+A selects all; Ctrl+C copies, Ctrl+X cuts, Ctrl+V pastes.
- The injected `Clipboard` service owns returned bytes and borrows copied bytes
  only for `write`. It must return promptly. A missing or failed service reports
  `CLIPBOARD_FAILED`; failed cuts never delete the selection.
- Paste remains data. Invalid UTF-8, CR, LF, U+2028 and U+2029 reject the entire
  operation with `INVALID_TEXT`. Other controls remain in logical storage and the
  renderer replaces unsupported clusters with `?`; raw ESC/C0 bytes never become
  terminal commands.
- `LineEditOptions::limits` bounds the entire draft in bytes, cluster bytes,
  graphemes and cells. Exceeding any limit rejects the complete change with
  `LIMIT_EXCEEDED`; nothing is silently truncated. External models must satisfy
  the same limits and single-line policy; invalid snapshots leave presentation
  unchanged and `sync` reports `INVALID_ARGUMENT`.

Default input storage is limited to 4096 bytes. Clipboard reads receive that cap
and their returned payload is checked again. Internal allocation failures report
`FAILED`/`NO_MEMORY`. Failure before accepting text preserves the old draft;
publication failure after acceptance keeps the new visible candidate without a
model commit. Argument construction before an
API call can throw. The software caret uses `LineEditOptions::cursor`, selection
uses its own style, and a one-cell viewport displays a blank caret when the
current wide glyph cannot fit. Scrolling never leaves half a glyph visible.
The built-in themes reverse the focused field colors for the caret, keeping it
visible over text and at the end of the draft in true-color and ANSI16 output.

Bounded [editor Undo/Redo](editor-history.md) is enabled by default and shares the
same admission, validation and model revision path. Mouse selection, IME
composition and native clipboard implementations remain separate work.

## Verification

`portals.line-edit` exercises real decoder, router, property, layout and rendering
through `FrameWriter` and `TestTerminal`'s independent VT oracle. It covers Unicode
bytes and selection, safe paste, limits, draft conflicts, notification rejection,
validator and clipboard reentry, removal, capture destruction and allocation
failure. `portals.line-edit.native` runs finite hidden ConPTY/PTY sessions and
checks native input, Unicode editing, native output and clean process exit.
Windows uses the existing terminal fixture's `WriteConsoleInputW` record injection
method; Linux drives UTF-8 through a PTY. No assertions are disabled and Windows
uses the shared no-dialog test process setup.

`portals.edit-modes` separately verifies the real immediate default, explicit
compatibility, completion blur, validator/resource reentry, revision conflicts,
clipboard policy, notification loss, scoped focus delivery, scene callback
lifetime and real allocation-budget failures. The relocated installed
`consumer.edit-modes` uses only public package headers to exercise all three modes.

`portals_line_edit --native-pipe` is an additional Windows host-translation
characterization. On the tested ConPTY host, combining/emoji input sent through
the pipe disappeared before `LineEdit` received semantic events; it therefore
fails the same strict expected-text check. The native-record test does not claim
to fix that separate host/input-adapter limitation or replace a real Windows
Terminal acceptance run.

See [text ownership and Unicode policy](text.md), [property notifications](property.md),
[focus and routing](focus.md), and [terminal ownership](terminal.md).
