# Loading scenes from text and files {#scene-loading-guide}

**TBD — M1 / U84**, after named reference lookup U126 and scoped styling U127.
This runtime loader is planned. The current API constructs scene descriptions
in C++; [compile-time scenes](compile-time-scenes.md) are C++ constant-evaluated
structures, not a parser for an external scene file.

Do not confuse a stylesheet file with a scene file. The existing
`scene | ui::style(ui::file(path))` loads PSS declarations into a scene;
it does not create controls or bind actions from that file.

## Planned loader boundary

The accepted direction is text/file input, an allowlisted typed registry,
named references and existing controls/i18n resolution. It uses the existing
scene runtime. General runtime style expressions are a separate
**TBD — M5 / U128** package, not a prerequisite for loader v0.

## Syntax reference

**TBD — U84 design review.** The concrete scene grammar, property value types,
allowed nesting, source locations and action/model registration syntax are not
yet an implemented contract. No example here should be copied as working code
until that contract is approved and implemented.

The guide will define:

- Text input versus native file paths, encoding and source labels.
- Control/layout declarations, ids and reference lookup.
- Allowed property types and explicit resource/i18n resolution.
- Connecting named actions/models supplied by C++ application code.
- Unknown controls/properties, invalid values and duplicate ids.
- Ownership, size/depth limits, atomic failure and cleanup.
- File errors and the exact reload policy; no watcher behavior is assumed.

## Planned examples

All three are **TBD — U84**, with final cookbook integration in M7/U96:

1. One label and one button loaded from an in-memory scene string.
2. A form file with named references, application-provided actions and PSS.
3. An integrated form with model binding, i18n and checked malformed-file failure.

Until then use [declarative C++ composition](declarative-ui.md) and the existing
[runnable examples](examples.md). Their syntax is already part of the library.
