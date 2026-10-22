# Unicode line editing

[Русская версия](ru/line-edit.md)

Create a field with [`ui::lineEdit`](../include/portals/ui.hpp) to connect input, a model and history automatically. In this application F2 copies the current draft to the label and appends `!`; Enter confirms the field, Escape restores the model, and F10 exits.

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::Ref<LineEdit> name;
  ui::Ref<Label> greeting;

  auto scene = ui::scene(
    ui::vbox(ui::lineEdit(name,
               "Ada",
               {.options = {.editMode = EditMode::EXPLICIT}}),
      ui::label(greeting, "Press F2 to read the draft")));

  return scene
    | ui::onKey("F2",
                [name, greeting] {
                  greeting.text() = name.text() + "!";
                })
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

The example reads the draft through `name.text()`, so F2 sees unconfirmed text too. To share a model between fields, pass the same `ManagedScene::state(...)` value to each; see [declarative composition](declarative-ui.md).

[Build the standalone examples](examples.md).

## Placeholder, protected display and Clear

Set `LineEditOptions::displayMode` and an optional owning `placeholder` when creating the field.
This complete native form uses an internal model and reports setup/runtime failures through
the ordinary runner. Clear returns its complete checked command receipt to the button adapter.
F10 exits and restores the terminal; Ctrl+Z restores a cleared value while history is enabled.

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::Ref<LineEdit> password;

  return ui::scene(ui::vbox(
    ui::lineEdit(password, "", {
      .options = {
        .displayMode = LineEditDisplayMode::PASSWORD,
        .placeholder = "Password"}}),
    ui::button("Clear", [password] {
      return password.clear();
    })))
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
}
```

[Installed-package build](package.md).
The same form is compiled as an installed consumer. No clipboard service is installed by
default; an application can supply one through `LineEditConfig::clipboard`.

| Display mode | Value, selection and caret | Intrinsic width | Ctrl+C / Ctrl+X |
| --- | --- | --- | --- |
| `NORMAL` (default) | Actual graphemes, with ordinary selection and scrolling | Displayed text plus a caret cell | Existing clipboard behavior |
| `PASSWORD` | One ASCII `*` per extended grapheme, including selection and caret overlays | Mask length plus a caret cell | Consumed without a service call or result callback |
| `NO_ECHO` | No value glyphs or selection; focused caret stays at the leftmost cell | One cell, independent of value length | Consumed without a service call or result callback |

Layout constraints still determine the allocated rectangle. PASSWORD exposes the grapheme
count, including one star for a combining sequence or an emoji ZWJ sequence regardless of its
actual terminal width. Cursor/anchor remain byte offsets in the original UTF-8 draft.
Protected copy/cut also leave history and invalidation unchanged. Inbound Ctrl+V and paste
remain enabled when the editor is writable. Read-only blocks paste, edits and user Clear;
disabled/hidden ancestry also blocks user commands. NORMAL still permits read-only copying.
Explicit programmatic `.text().set(...)`
keeps its existing behavior, including on a read-only editor.

An absent or empty placeholder shows nothing. A nonempty placeholder appears only while
the draft is empty, both focused and unfocused, using the existing appearance and grapheme
clipping. It never enters the model, validation, history, callbacks or clipboard. NO_ECHO
keeps its fixed one-cell intrinsic width even when showing a prompt. Checked recipe mounting
validates and prepares placeholder storage under the scene width policy and editor limits
before publishing an internal model. Invalid UTF-8, line separators or over-limit text fail
mounting; text failures preserve their typed payload, and allocation failures report NO_MEMORY.
The direct low-level `LineEdit` constructor uses its documented exception convention instead.

Both options are immutable after mount. Change display policy by removing the editor and
mounting a fresh binding, which starts fresh history. Destruction and remounting do not
promise to overwrite old storage.

PASSWORD/NO_ECHO govern presentation and outbound clipboard handling. The application can
still read plaintext through its model, `TextProperty::get`, text-source snapshots,
`LineEdit::text`, commit receipts and undo history. History remains bounded and enabled by
default (128 combined operations, 1 MiB of retained snapshots); `history.enabled = false`
disables Undo, including for Clear. Built-in paint/cache commands and diagnostic exports
omit the actual protected value; application callbacks/loggers remain the application's
responsibility. Keep reference names, scopes and placeholder text nonsecret. No secure
erase or zeroization is provided.

`Ref<LineEdit>::clear()` and low-level `LineEditBinding::clear()` create one separate user
edit containing the previous draft and selection. Clear ignores the programmatic setter's
RESET_BASELINE policy and follows the selected EditMode. An already-empty writable field is
a strict no-op. Read-only/disabled fields return WOULD_BLOCK even when empty. Clear never
adopts or overwrites a newer external model as a side effect.

Before draft acceptance, allocation/history/cache/invalidation failure preserves the old
draft, selection and open typing group. After acceptance, a technical publication failure
retains the matching empty draft/history/cache with dirty=true and the uncommitted model.
The checked command returns the exact technical error. VALIDATION_FAILED and CONFLICT are
semantic `LineEditResult` outcomes: the command receipt succeeds and the accepted draft
remains undoable. A nonempty clear delivers one outcome. Return the complete
`EditorCommandResult` from managed actions, as above, to keep report-once provenance;
extracting `.result()` discards that receipt. Successful model commits and arbitrary user
callback effects are not rolled back.

<details>
<summary>Reference: binding, publication and input</summary>

`LineEdit` owns a UTF-8 draft and exposes byte-boundary cursor and anchor positions.
Its `GraphemeText` groups editing into complete
[graphemes](glossary.md#glossary-text-units), such as a letter with its combining accent.
Terminal width and horizontal scrolling use cells. This includes emoji joined by
a zero-width joiner (ZWJ) and Chinese, Japanese and Korean (CJK) text. Construct the widget, create a `Property<std::string>`, and retain the result
of `bindLineEdit` for as long as input and observation should remain connected.

The tree, router and scheduler outlive the binding. The binding retains the model
and optional clipboard service. Destroy or move it on the UI owner between frames.
Widget text references borrowed through `tree.read` must not escape that callback.
Render commands may borrow widget text until the enclosing `tree.frame` returns;
the frame owner translates local commands and intersects the ancestor clip, as in
the control gallery. Paint never reads the model, invokes a service or changes
application data. Focus is a presentation snapshot supplied by the caller, while
the router remains the authority for input eligibility.

**Draft and model revisions**

`LineEditOptions::editMode` selects when technically accepted text is validated
and published. The default is `EditMode::IMMEDIATE`: input, deletion, clipboard
cut/paste and fluent text changes publish valid changed candidates without Enter.

| Mode | Text change | Blur | Enter / programmatic confirmation |
|---|---|---|---|
| IMMEDIATE (default) | Validate and publish | Keep any rejected intermediate draft | Validate and confirm |
| ON_COMPLETION | Keep dirty draft | Validate and confirm once | Validate and confirm |
| EXPLICIT | Keep dirty draft | Keep draft | Validate and confirm |

For confirmation only on Enter or activation, select:
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

**Input and clipboard**

- Left/Right, Home/End move by grapheme boundaries; Shift extends selection.
- Backspace/Delete remove complete selected or neighboring graphemes.
- Ctrl+A selects all; in NORMAL Ctrl+C copies and Ctrl+X cuts; Ctrl+V pastes in writable modes.
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

[Developer testing](testing.md).

See [text ownership and Unicode policy](text.md), [property notifications](property.md),
[focus and routing](focus.md), and [terminal ownership](terminal.md).

</details>
