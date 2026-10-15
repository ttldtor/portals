# Three-mode editor history demo

[Back to samples](examples.md).

`portals_editor_history` is a compact interactive example of the installed public
API ([source](../examples/editor_history/editor.hpp)). It owns a lazy `ui::Scene` with three editors and read-only views of
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
the ordinary layout.

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
  Redo returns `field.redo()` through the same compact command adapter.
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

[Developer testing and headless replay](testing.md).

<details>
<summary>Reference: errors and ownership</summary>

**Checked errors and ownership**

Factory and configure callbacks return typed results to the scene collector.
The native entry point uses `demo.scene() | ui::reportOnError() |
ui::runApp(argc, argv, ui::lyra::cli(...))`. Lyra preflight completes before
mounting or terminal acquisition. The standard runner restores the terminal and
reports failures; typed setup, input and cleanup failures return a nonzero status.
Recoverable errors retained by the collector do not by themselves change a
successful run to a failure exit status.

Undo/Redo callbacks return `EditorCommandResult` intact, preserving collector
provenance. Append uses `field.text() += "!"`; the text proxy records failures in
the same collector. The last-result display explains editor and notification
outcomes without retrying a committed mutation. A failure updating that display
returns its original typed error.

The stationary `Demo` outlives callbacks borrowing it. Its `scene()` accessor
supports additional application bindings; `rows()` exposes observing editor and
model handles after preparation. Controls and model storage belong to the scene.
The finite test harness wraps this actual demo and checks diagnostics separately;
it does not duplicate the interface or alter the native entry point.
See [editor history](editor-history.md), [LineEdit](line-edit.md),
[text properties](text-property.md) and [diagnostics](diagnostics.md).

</details>
