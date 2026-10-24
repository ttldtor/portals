# Form fields {#form-fields}

[Русская версия](ru/form-fields.md) · [Control examples](control-examples.md) · [Editing modes](line-edit.md)

A `FormField` groups a label, an editor, help text and a readable validation
message. The scene owns the composition. Ordinary text fields create their
internal editor state; applications do not keep binding tokens or synchronize
the captions manually.

## Account settings example

The complete program edits a display name and an email address, demonstrates a
custom checkbox editor, checks the fields and restores initial text on request.
It keeps values for the current session; it does not write an account or file.
The email rule is a small example heuristic, not a complete email-address parser.

Build the `portals_form_field_example` target with examples enabled, then run
the resulting executable in a terminal. Tab moves between editors and buttons;
F10 closes the scene and the standard runner restores the terminal.

[Complete source](../examples/form_field/main.cpp)

\include form_field/main.cpp

## Validation and editing

An ordinary field uses the existing `LineEdit` editing policy. Required fields
reject an empty publication candidate before running a custom synchronous rule.
The rule returns an owning `FieldValidation`: acceptance or a rejection message.
It does not run during layout, painting or stylesheet resolution.

- `IMMEDIATE` is the default: editing checks candidates through the existing
  publication path. A rejected intermediate draft stays visible.
- `ON_COMPLETION` checks when editing completes through Enter or focus loss.
- `EXPLICIT` checks on explicit confirmation. Losing focus preserves the draft.

The editor retains its normal Undo/Redo, selection, Unicode and external-update
rules. A field does not introduce a form transaction or change Enter/Escape
semantics. See [the editor contract](line-edit.md).

## Refs and explicit checks

`ui::Ref<FormField>` observes the composition. Inside a managed callback,
`field.valid()` checks its current text draft and updates the message. It returns
false for a validation rejection; technical failures go to the existing scene
diagnostics. This operation does not publish the draft or add an Undo entry.
Use `validate()` when code needs to inspect the checked result explicitly.

`validation()` reads the most recent presentation without running a rule.
Deferred editing can retain its previous message until the next check.
`FieldValidation::pending(...)` is explicit presentation only: it does not disable
the editor, launch a task or provide an asynchronous completion endpoint.

## Custom editors and limits

The custom-editor overload accepts an ordinary `ui::Description<T>` and uses the
same label/help/message layout. It does not assume that a checkbox or another
custom editor has string validation semantics. Standard text operations require
a text editor; inappropriate operations report a checked error.

Callbacks remain C++ declarations when a field is loaded from a scene resource.
Asynchronous validation, stale-completion admission and transactional submit/reset
are outside this implementation. They are not implied by a pending message.

## Loaded scene

The resource variant declares two required text fields, including an explicit
editor. Standard `FormField` registration is built in. Captions, initial text,
help and required settings stay in the resource; the button action remains C++.
This program demonstrates required checks, not the email rule in the C++ form.

[Complete resource program](examples/guide_form_field_resource.cpp)

\include guide_form_field_resource.cpp
