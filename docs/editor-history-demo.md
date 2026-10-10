# Three-mode editor history demo

`portals_editor_history` is a compact interactive example of the installed public
API. It mounts one ordinary `ManagedScene`, three editors and read-only views of
their published models. The models start at `base`; draft and model rows are
deliberately separate so deferred confirmation is visible.

Build and run from a normal configured source build:

```powershell
cmake --build build/debug --target portals_editor_history
& ./build/debug/examples/editor_history/portals_editor_history.exe
```

With a multi-configuration generator, pass `--config Debug` and use its `Debug`
executable subdirectory. On Unix run the executable without the `.exe` suffix.
The example needs approximately 80 columns and 24 rows; smaller terminals clip
the ordinary layout. No new renderer, reactive system or application command bus
is introduced.

## Try the confirmation policies

| Mode | Select | Visible draft | Published model |
|---|---|---|---|
| IMMEDIATE, real default | F5 | Changes as you type or Undo/Redo | Changes immediately |
| ON_COMPLETION | F6 | Changes immediately | Changes on blur or Enter |
| EXPLICIT, explicit option | F7 | Changes immediately and survives blur | Changes on Enter |

Each model row is another bound `LineEdit` with `readOnly=true` and history
disabled. You can select its text, but typing and Undo/Redo cannot change it.
Read-only permits Copy at the binding boundary; this example supplies no system
clipboard service. The main editable fields keep the ordinary history budgets
and shortcuts.

- Ctrl+Z undoes text; Ctrl+Y redoes it. The adjacent buttons use the same Ref
  commands. Undo uses `ui::button("Undo", [field] { return field.undo(); })`;
  Redo demonstrates late `scene.onClick(..., [field] { return field.redo(); })`.
- F3, while an editor is focused, or its **Append !** button executes
  `field.text() += "!"`. This is one separate reversible programmatic change.
- Enter confirms the focused editor. Tab or a click changes focus; this confirms
  ON_COMPLETION and preserves EXPLICIT's pending draft. Clicking a command button
  is itself a focus transition, so its blur behavior follows the selected mode.
- Escape adopts the published model and clears local history. In IMMEDIATE it
  does not undo text which has already reached the model; use Undo for that.
- F10 exits through the existing runner, which restores the terminal.

The last-result row describes draft changes, publication and cancellation in
plain language. No numeric status codes are required to use the example.
The example has no system clipboard adapter; it demonstrates the existing
read-only selection policy without adding a clipboard service.

## Headless verification and replay

```powershell
& ./build/debug/examples/editor_history/portals_editor_history.exe --check
& ./build/debug/examples/editor_history/portals_editor_history.exe --replay
& ./build/debug/examples/editor_history/portals_editor_history.exe --help
```

`--check` first exercises the real button bindings, late command adapter,
programmatic RECORD/RESET, Enter, Ctrl+Z, Escape and blocked read-only typing for
all three modes. It compares editor bytes, published model snapshots and the
actual prepared read-only model widgets. A RESET used by the check clears both
history stacks; it is intentionally not a user-facing load control.

It then runs the same scene through the existing `ReplayTerminal` and native VT
decoder. Twelve F1 checkpoints inspect the preceding prepared presentation after
typing, Ctrl+Z/Y, F3 append, blur, Enter and Escape. The final published values
must be `baseA!`, `baseB`, `baseC`. Output must have been rendered and the replay
transport must have closed. The check never opens a native terminal or waits for
a person to close a window. `--replay` runs that finite input and prints final
model values without assertions; `--help` creates no runtime.

CTest names are `portals.editor-history-check`, `portals.editor-history-replay`
and `portals.editor-history-help`. The existing relocated package consumer also
compiles this exact example and runs `consumer.example-editor-history` and
`consumer.example-editor-history-replay` using installed headers and archives.

## Checked errors and ownership

The local `checked(result, operation)` helper is an example-only fail-fast policy
for setup and finite checks. It prints the original typed phase/cause before
throwing with the operation name. It does not silently discard an error or serve
as the policy for event callbacks.

Undo/Redo callbacks return `EditorCommandResult` intact so the existing adapters
preserve exact collector provenance. Append uses a text proxy, which reports its
own failure to the same scene collector. The callback inspects and retains that
failure, returns no duplicate error through a second adapter, and lets the normal
managed runner stop policy decide continuation. The last-result callback returns
an independent failure if updating its status state fails. Successful model or
status receipts with notification failure are retained and reported after run;
the example does not retry a committed mutation to repair delivery.

After the terminal has been restored, `run()` checks its typed result and the
scene's owning diagnostic snapshot. It prints retained causes and operation
breadcrumbs, checks overflow/observer failures, and exits nonzero on any failure.
Scene ownership is released before its borrowed Application; callbacks retain
observing Refs and state handles, with no borrowed widget references.

See [editor history](editor-history.md), [LineEdit](line-edit.md),
[text properties](text-property.md) and [diagnostics](diagnostics.md).
