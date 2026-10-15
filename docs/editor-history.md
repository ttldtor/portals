# Editor Undo and Redo

Undo/Redo restores **one editor’s text and selection**. Completion (`EditMode::ON_COMPLETION`) determines when its draft reaches the model; it does not maintain a list of earlier values. Completion suggestions or previously entered commands are not supplied by this API. `ui::lineEdit` creates the binding and history automatically; ordinary fields do not need a separate `TextHistory`. Type, then use Ctrl+Z or Undo; F10 exits.

Every bound [`LineEdit`](../include/portals/line_edit.hpp) has its own bounded text history. History is enabled by
default: 128 combined Undo/Redo groups and 1 MiB of retained snapshot storage.
`LineEditOptions::history` configures these bounds and typing/deletion grouping.
There is no application command bus or scene-wide transaction history.

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::Ref<LineEdit> name;

  auto scene = ui::scene(ui::vbox(
    ui::lineEdit(name, "Ada"),
    ui::button("Undo",
               [name] {
                 return name.undo();
               }),
    ui::button("Redo",
               [name] {
                 return name.redo();
               })));

  return scene
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

Input, deletion, cut/paste and fluent changes all use the existing editor binding.
Undo restores owning text plus cursor and selection byte offsets at complete
grapheme boundaries. Combining marks, ZWJ emoji and wide characters retain their
bytes; the restored text uses the editor's captured width policy and limits.

By default Undo also publishes the restored value immediately. Deferred modes keep the restored draft until the same confirmation point as typing; see [confirmation modes](line-edit.md).

[Build the standalone examples](examples.md).

<details>
<summary>Reference: recording, revisions and command results</summary>

**Recording and publication**

Plain consecutive typing, backward deletion and forward deletion form separate
groups of their respective kinds. Selection replacement, paste, cut and recorded
programmatic mutations are separate operations. Navigation, selection, focus
loss, Enter, Escape and `stopGrouping()` close the current group. Confirmation
closes grouping and preserves history. An equal ordinary mutation preserves Redo.
Empty Undo/Redo closes grouping without callbacks, model notification or redraw.

`ProgrammaticChange::RECORD` is the default. The trailing option
`LineEditOptions::programmaticChange` changes the default for fluent mutations.
`field.text(ProgrammaticChange::RESET_BASELINE)` captures an override by value for
every mutation of that proxy, including `+=`, `append` and owning `+` assignment.
RESET clears both stacks after accepting the baseline even when its bytes are
equal. Equal RESET does not publish the model or request a redraw.

The draft, cursor/selection and history are accepted together before validation
or model callbacks. Allocation, text validation, history budget or tree failure
before acceptance preserves all three and existing Redo. An oversized whole
group is never truncated: input reports `LIMIT_EXCEEDED`, while a rejected fluent
mutation reports `WOULD_BLOCK`. Old complete groups are evicted when necessary.
The [text history component](text-history.md) describes retained byte accounting.

Undo/Redo then follows the same `EditMode` as other accepted edits:

| Mode | Restored draft | Model |
|---|---|---|
| IMMEDIATE (default) | Visible immediately | Validate and publish immediately |
| ON_COMPLETION | Visible immediately | Publish on blur, Enter or activation |
| EXPLICIT | Visible immediately | Publish on Enter or activation |

Semantic validation false retains a visible reversible draft and reports
`VALIDATION_FAILED`; the invalid value is not published. A validator throw or
publication failure also leaves the admitted draft/history intact, returning
a typed technical command failure and delivering its detailed `LineEditResult`.
Do not retry Undo merely to deliver a rejected model notification. Its successful
`PropertyUpdate` receipt remains authoritative.

**Model revisions and availability**

A successful editor publication advances its own baseline; it does not reset
history as an external change. A clean external model revision adopts the latest
value and clears history, including a new revision with equal bytes. If Undo/Redo
first discovers this clean revision, it adopts/reset and finishes as a no-op
instead of applying an old operation.

A dirty external conflict preserves the local draft/history. Ordinary draft
editing remains possible, but effective history availability is false. Undo/Redo
returns `WOULD_BLOCK` with a `CONFLICT` outcome before changing text, and cannot
overwrite the external model. Escape adopts latest and resets history; an explicit
RESET chooses a new baseline. No automatic rebasing is performed. In IMMEDIATE,
Escape does not undo already published model changes.

Both `LineEditBinding` and `ui::Ref<LineEdit>` provide `undo()`, `redo()`,
`stopGrouping()`, `canUndo()`, `canRedo()` and checked `historyAvailability()`.
The boolean queries return false on errors and never report, adopt or invalidate.
The checked query returns `HistoryAvailability{canUndo, canRedo}` or the original
affinity/lifetime/frame/reentry error. Hidden/disabled ancestry, read-only, empty
or disabled history and external revision mismatch all make both commands
unavailable. Disabled/read-only commands return `WOULD_BLOCK` before acceptance.

`LineEditOptions::readOnly` defaults to false. Read-only blocks typing, deletion,
cut and paste without calling clipboard or validator callbacks. Selection,
navigation and Copy remain available. Explicit programmatic text mutations remain
allowed and follow RECORD/RESET plus the configured publication mode.

**Keyboard shortcuts**

Defaults are Ctrl+Z for Undo, Ctrl+Y and Ctrl+Shift+Z for Redo. Set the owning
`LineEditOptions::historyShortcuts` values to remap them; `nullopt` disables an
individual shortcut. Modifiers match exactly, ASCII character case is normalized,
PRESS/REPEAT are accepted and RELEASE is ignored. A history shortcut takes
priority over overlapping fixed Ctrl+A/C/X/V bindings. Even an empty matched
history command is handled locally without bubbling or a result callback.

Supported identities are CHARACTER with a valid nonzero Unicode scalar, INSERT,
PAGE_UP, PAGE_DOWN and F1 through F12. Non-character identities require scalar
zero. Enter, Escape, Tab, arrows, Home/End, Backspace/Delete and invalid enum/scalar
values are rejected at binding with `INVALID_ARGUMENT`: the existing router
translates/reserves those keys before editor KeyEvent delivery. Undo/Redo overlap
is rejected. No new RoutedEvent alternative or Action is introduced. Ctrl+Shift+Z
requires an input adapter that reports Shift distinctly; Ctrl+Y remains available
with VT input.

**Checked commands and diagnostics**

Low-level binding commands return `expected<void, TreeError>`. Ref commands return
`EditorCommandResult`: an owning checked result carrying a private receipt of
exactly its own failure's actual reporting to a weak scene collector. It does not
keep widgets alive. Use `if (field.undo())` or inspect `.result()` explicitly.

Both `ui::button` and late `scene.onClick(button, callback)` accept
`std::function<EditorCommandResult()>`, supporting `return field.undo()` directly.
Their adapters do not record the exact returned failure again when it was already
recorded in the same live collector. Independent callback failures, exceptions,
another command or another collector remain distinct even with the same cause.
Overflow and observer failures retain the existing collector semantics.

There is no implicit conversion to expected. Extracting `.result()` deliberately
loses provenance: returning `field.undo().result()` through an expected callback
uses its normal reporting path and may record the failure again. An absent or
expired Ref scene returns `CLOSED`; a foreign thread returns `WRONG_THREAD`
without reading mutable scene fields or reporting unsafely. Other technical
errors preserve their typed causes.

Callbacks admitted before node/binding removal retain existing shared binding
state. User callbacks run only after coherent admission, and scene access after
callbacks first checks weak lifetime. History contains owning text/selection,
never borrowed widget storage. By-value argument and resolver construction may
throw before entry; internal allocation failures are typed.

[Developer testing](testing.md).

See [LineEdit](line-edit.md), [text properties](text-property.md),
[diagnostics](diagnostics.md) and [focus](focus.md).

</details>
