# Loading scenes from text and files {#scene-loading-guide}

[Русская версия](ru/scene-loading-guide.md)

**TBD — M1:** scene loading from strings and files is planned after named
reference lookup and scoped styles through `Ref`. Current scenes use
[declarative C++](declarative-ui.md). [Compile-time scenes](compile-time-scenes.md)
also use C++ descriptions.

A PSS file loaded through `scene | ui::style(ui::file(path))` styles existing
controls. Creating controls and connecting actions from a file belongs to
the planned scene loader.

## Planned contract

The loader will use the current scene runtime, an allowlisted typed control
registry, named references and existing control/localized-resource resolution.
General style expressions are **TBD — M5**; loader v0 can work without them.

Grammar and examples are **TBD — M1**. The contract still needs to define:

- value types, nesting and source locations;
- references, actions and models supplied by application code;
- unknown controls/properties, invalid values and duplicate IDs;
- ownership, size/depth bounds, atomic rejection and cleanup;
- file errors and reload policy. Automatic file watching is not currently planned.

## Future examples

**TBD — M1:** a label and button from a string; a file-based form with named
references, actions and PSS; a model-bound localized form with invalid-file handling.
The final cookbook review is **TBD — M7**.

Current runnable programs are listed in [examples](examples.md).
