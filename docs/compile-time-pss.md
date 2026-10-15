# PSS strings at compile time and runtime {#compile_time_pss}

Experimental compile-time chapter. For ordinary application code, start with
[runtime PSS and declarative UI](styling-guide.md). This API is already
implemented; experimental status does not mean TBD.

Include `<portals/style_pss.hpp>` for the bounded C++23 string frontend and
owning source metadata. Both frontends use the same PSS v0 parser and the
existing [style cascade](styles.md). Materialization and mounting still happen
at runtime; this is not another renderer, scheduler or reactive system.

## Construct a scene through a factory

This complete client delegates checked construction, setup, reporting, running
and cleanup to `runApp`. The setup receives the stationary `Scene` by reference;
`ui::quit` uses the entered scene context, so the callback does not capture the
facade by address. The `ui::style(...)` [configuration stage](glossary.md#glossary-ui-stage)
enables layering before any live layer is attached.

~~~cpp
#include <portals/ui_style.hpp>

namespace ui = portals::ui;
namespace sc = portals::style::ct;

inline constexpr auto STYLES = sc::pss(R"pss(
  #message { min-width: 24; }
  #append { bold: true; }
)pss", "native-demo.pss");

/** @return Zero on success, one after a reported construction, setup, input or cleanup error. */
int main() {
  return ui::runApp(
    [] {
      ui::Ref<portals::LineEdit> message;

      return ui::column(
        ui::label("Portals | F10: quit"),
        ui::lineEdit(message, "Hello").id("message"),
        ui::button("Append", [message] {
          message.text() += portals::text(" {}", "Portals");
        }).id("append"));
    },
    [](ui::Scene &scene) {
      scene
        | ui::theme(portals::modernTheme());
      scene
        | ui::style(STYLES, {.name = "demo"})
        | ui::reportOnError()
        | ui::onKey(ui::key(portals::InputKey::F10), ui::quit);
    });
}
~~~

For an already-built ordinary scene, the compact equivalent is
`scene | ui::style(STYLES) | ui::runApp()`. The method
`scene.style(source, options)`, the free function
`ui::style(scene, ui::style(source, options))`, and the pipe form all dispatch
through the same owning stage. A named `Scene` is kept stationary by the lvalue
method and pipe overloads; temporary chains use the consuming overload.

Style-stage construction (including parsing bytes or copying a document)
happens before the receiving `Scene` call. Its allocation or argument-construction
exceptions can therefore occur before that facade checked error boundary.
`runApp(factory, setup)` invokes the description factory inside its checked
boundary and maps factory/setup/run failures into the normal report and exit
status. Construction of the callable objects and arguments passed to `runApp`
still happens before invocation and is outside that boundary. The factory
returns a scene description, never a `Scene`.

## Scalar rules stored as data

Short selectors retain the existing conjunction semantics:
`sc::all()`, `sc::id<"action">()`, `sc::type<"Button">()` and
`sc::className<"primary">()`. Use the existing `selector<Type, Id, Classes...>(state)`
for combinations. `fg` and `bg` are short declaration helpers.

```cpp
inline constexpr auto TYPED = sc::compactSheet(std::array{
  sc::flatRule(sc::all(), sc::bg(sc::rgb(24, 28, 36))),
  sc::flatRule(sc::id<"action">(), sc::fg(sc::rgb(255, 210, 70)),
               sc::bold(true), sc::minWidth(std::int32_t{8}))
}, "application.typed");
```

`flatRule(parameters, selector, declarations...)` evaluates typed parameter
defaults and produces a scalar snapshot. It preserves name and type checks.
The array frontend stores rule counts as data: its public result type contains
numeric capacities, rather than every rule and expression type. It owns one
shared text arena. Intermediate selector views refer only to template-static
names; the final descriptor copies them. Default-constructed `FlatRule` is a
universal empty rule.

Snapshots have no live parameter expressions. Use the existing
[`LiveTheme`](compile-time-styles.md) for reactive parameters; it continues
to coexist with constant PSS application/user overrides in the same engine.
Variables, color expressions and extended selectors in string PSS are not part
of v0. Repeated text declarations use last-wins semantics; the old typed rule
validation continues to reject duplicate declaration destinations.

## Runtime parsing and provenance

`parsePssDocument(bytes, sourceName)` returns `expected<PssDocument, PssError>`.
It copies selectors, the source identity and all declaration occurrences,
including overwritten values. Caller buffers may be destroyed after the call.
The parser performs no file I/O. `sourceName` is a caller-supplied identity,
not an instruction to open a file.

`sc::materializeDocument(description)` returns the same owning document through
a typed allocation boundary. `sc::materialize(description)` returns only
`StyleSheet`, deliberately discards metadata and may throw on allocation, as
the existing typed materializer does.

`PssDocument::sheet` holds effective source-order rules. `origins` links each
rule to its selector location and declaration range. `declarations` retains
every occurrence, its property, scalar value and location; `effectiveDeclaration`
selects the final occurrence for each property. `originKind` distinguishes
original PSS byte coordinates from synthetic typed ordinals, which are not C++
file locations. Offsets are zero-based; lines and byte columns are one-based,
with LF advancing the line. An accepted leading BOM remains part of the offsets.

Installing `document.sheet` through the low-level whole-sheet API projects away
this provenance. `ui::style(document)` retains the owning document with its
application or user layer, available through `ManagedScene::styleSources()`.
A full inspector explaining the winning rule for every property is subsequent
work. Metadata-only changes introduce no periodic redraw or per-frame trace
allocations.

## Input policies and limits

The new document and compile-time frontends validate UTF-8 over the complete
extent, reject embedded NUL, and accept one leading UTF-8 BOM. Identifiers and
property/state names remain ASCII. `parsePss(bytes)` preserves its established
byte admission, including arbitrary comment bytes and rejection of a leading
BOM; it returns `expected<StyleSheet, StyleError>` unchanged.

| Resource | Compile-time frontend | Runtime document |
| --- | --- | --- |
| Input bytes | 16 KiB | 1 MiB |
| Rules | 256 | 1024 |
| Global class predicates | 1024 | Logical document budget |
| Declaration occurrences | 1536 | Logical document budget |
| Shared owning text | 16 KiB | Logical document budget |
| Source identity | 512 bytes | 4096 bytes |
| Descriptor/document payload | 128 KiB descriptor bound | 4 MiB logical payload |

Logical runtime payload includes owned text, rule/class records and all origin
records. It is not a cap on allocator bookkeeping, vector capacity or transient
copies. Named admission failures are `INVALID_ARGUMENT`; allocation failures
are `NO_MEMORY`. No partial document escapes. The legacy `parsePss` adapter does
not acquire the new source identity or document payload limits.

## Compiler diagnostics

The convenience literal overload retains the full array extent, including NUL.
For a structured diagnostic carrying code, offset, line and column, use a
captureless provider returning an explicit-length `string_view`:

```cpp
using namespace std::literals;
// Intentionally invalid: an unknown property at offset 4, line 1, column 5.
constexpr auto INVALID = sc::pss([] {
  return "* { missing: true; }"sv;
});
```

The first assertion reports a fixed category such as `Unknown PSS property`;
the bounded `PssFailure` carrier gives coordinates and the subject. GCC and
MSVC format this differently. MSVC renders subject characters as numeric values;
the runtime `PssError::subjectView()` exposes the readable owning subject.
Direct literal diagnostics give a named failure category and the C++ invocation
location; C++23 cannot turn ordinary function arguments into dynamic assertion
text or coordinate template arguments. The provider type avoids embedding the
whole stylesheet in a template argument. Raw-pointer providers are rejected:
they would lose bytes after an embedded NUL. A caller-created already-truncated
`string_view` cannot recover those lost bytes.

The subject owns at most 64 ASCII bytes with an explicit truncation flag;
non-ASCII bytes use a safe placeholder. Runtime errors do not retain a borrowed
source identity. Low-level callers supply that context; the scene loading
boundary retains a bounded owning label in `ManagedError::styleSource`.

## Additional examples

The [typed client](../tests/package_consumer/ct_pss.cpp) and
[metadata ownership test](../tests/package_consumer/ct_pss_metadata.cpp)
cover additional scenarios and checks.
