# Loading scenes from text and files {#scene-loading-guide}

[Русская версия](ru/scene-loading-guide.md)

`ui::scene(source)` copies UTF-8 resource text into a lazy
[scene](glossary.md#glossary-scene), `ui::Scene`. Parsing and construction happen at
`prepareChecked()` or preparation inside `run`/`runApp`, before a terminal opens. Include
`<portals/ui.hpp>` or `<portals/ui_scene_resource.hpp>` and link `portals::core`
(C++23). The loaded controls use the same named references, event handlers,
models, PSS and cleanup as controls declared in C++.

## A complete form

Build `portals_scene_resource` with `PORTALS_BUILD_EXAMPLES=ON`. Add Hello
appends text; Exit or F10 closes the scene. The native runner restores the
terminal and reports failures.

Sources: [form definition](../examples/scene_resource/form.hpp) and
[application entry point](../examples/scene_resource/main.cpp).

\include{lineno} scene_resource/form.hpp
\include{lineno} scene_resource/main.cpp

A plain string always means source text. Use an explicit owning file source:

```cpp
auto scene = ui::scene(ui::SceneResourceFile{"form.scene"});
auto message = scene.get<LineEdit>("message");
```

The file is read once at prepare; relative paths use the then-current working
directory. There is no automatic reload or file watcher. PSS remains a separate
stage: `scene | ui::style(ui::file("form.pss"))`.

## Resource syntax

```text
VBox#form {
  gap = 1
  Label { text = "Message" }
  LineEdit#message { ""; width: 32 }
  HBox { Button#add { "Add" } CheckBox#enabled { "Enabled"; checked = true } }
}
```

One root node is required: `Type`, optional `#id`, then properties and children
inside braces. Types and ids are case-sensitive ASCII identifiers starting with
a letter or underscore; ids also allow `-` after the first character. Ids are
unique throughout the resource and use existing typed/dynamic `scene.get` lookup.
Properties use `name = value` or `name: value`. Whitespace, commas and semicolons
separate entries; `//` introduces a line comment outside strings.

Double-quoted strings support these escape sequences:

```text
\"  \\  \n  \r  \t
```

Raw line breaks and other ASCII control bytes inside strings are rejected. Source UTF-8 is
checked even in comments. A positional string abbreviates `text = "..."`.
Repeating a property or using both text forms is an error.

| Type | Text | Children | Additional properties |
|---|---|---|---|
| `VBox`, `HBox` | No | Yes | `gap`: nonnegative 32-bit cell count, default zero |
| `Label`, `LineEdit`, `Button` | Yes | No | — |
| `CheckBox` | Yes | No | `checked`: `true` or `false`, default `false` |
| Registered custom type | Yes | Opt-in during registration | Fixed sizes for `SizedWidget` types |

All builtin types accept nonnegative 32-bit `width`/`height` in cells and positive
unsigned 32-bit `flex`. They apply existing recipe operations in width/height/flex
order. Text defaults to empty. General expressions, bindings, actions, style
declarations and arbitrary constructor arguments are rejected. Connect actions
and models through existing C++ APIs after lookup.

## Localized text and custom types

`text = i18n("key")` or positional `i18n("key")` uses the explicitly supplied
existing `TextResolver`. All text is resolved and checked under the scene width
policy before any factory runs:

```cpp
ui::SceneResourceOptions resource;
resource.resolver = [](std::string_view key)
  -> std::expected<std::string, TextResourceError> {
  if (key == "add") {
    return "Add";
  }
  return std::unexpected(TextResourceError::MISSING_RESOURCE);
};
auto scene = ui::scene("Button#add{i18n(\"add\")}", std::move(resource));
```

There is no implicit global locale or automatic translation refresh. Missing
resources preserve `ManagedError::resource`; invalid text preserves `text`.
Explicitly borrowed resolver services must outlive prepare.

Register custom types with a concrete public `Widget` subtype and an owning
copyable factory returning `Description<T>` or exactly
`expected<Description<T>, ManagedError>`:

```cpp
ui::SceneResourceOptions resource;

resource.control<Meter>("Meter", [](ui::SceneResourceArguments arguments) {
  return ui::custom<Meter>(std::move(arguments.text));
});

auto scene = ui::scene("Meter#level{\"CPU\" width=20}", std::move(resource));
```

Here `Meter` is an application-defined `SizedWidget` constructible from an owning
string. `SceneResourceArguments` owns text/id. The loader applies id and common
sizes after the factory recipe. A third `true` argument to `add` admits children.
Names cannot replace a builtin or earlier registration. Factories run once per
node, children before parents, after complete grammar/schema/id/text validation.
No global registry, dynamic-library lookup or arbitrary evaluator is involved.


`control<T>(name, factory, children=false)` returns these options for chaining.
It collects the first registration failure without a manual `expected` check;
later fluent registrations are skipped. `prepareChecked()` or `runApp()` reports
the typed failure through the existing error path before reading a file, parsing,
resolving text, invoking factories or opening the terminal. Registration failures
have no resource-source position (`line` and `column` are zero); default reporting
labels them as registration failures instead of printing a misleading location.

Internal allocation failures become `TreeError::NO_MEMORY`; other exceptions from
factory copying become `CALLBACK_FAILED`. Constructing the options, caller-side
arguments/captures and the scene can still throw before this adapter is entered.
For explicit recovery, `resource.registry.add<T>()` retains its checked `expected`
API; a handled error from that API does not poison the fluent options.
A later checked registration does not clear an already latched fluent error;
construct corrected options to replace that failed configuration.

```cpp
auto options = ui::SceneResourceOptions{}
  .control<Meter>("CpuMeter", cpuFactory)
  .control<Meter>("MemoryMeter", memoryFactory);
```

Here the factories are application-defined copyable callables with the signature
shown above. Resources remain optional: ordinary `ui::scene(ui::vbox(...))` needs
no registry. This adapter does not add custom resource properties or dynamic code
loading.

`ui::resource(source, options)` returns the same owning lazy `Description<Widget>`
used by the string/file `scene` overloads. It can be a child of a C++ description
or the result of an existing scene factory. Create managed models in that factory,
capture their observing handles in the registered factories, and return `resource`.
These handles refer to scene-owned models without keeping them alive.
This uses the enclosing mount transaction and creates no additional scene owner.

## Bounds, failures and lifetime

`SceneResourceOptions::limits` defaults to 1 MiB source, 4096 nodes, depth 64,
64 KiB per decoded string or resolved text, and 16 properties per node. Every
limit must be greater than zero; the hard depth ceiling is 64 because parsing uses the native
stack. Larger configured depths return `INVALID_LIMIT`; a source exceeding its
configured depth returns `DEPTH_LIMIT`. Resource recipes mount iteratively in one
existing transaction. Existing managed/tree/text/state limits also apply.

Unknown types/properties, repeated ids/properties, invalid values, expressions,
UTF-8 and parser limits fail before any factory or resource node is created.
Recipes mount in one existing transaction. Failure removes the attempted subtree
and leaves observing slots unpublished. External factory effects and separately
created scene state are outside subtree rollback; the failing lazy Scene performs
its ordinary full cleanup.

`ManagedError::sceneResource` retains a typed category, zero-based byte offset,
one-based line and byte column. It also retains the source text or file path
in shared storage, so the diagnostic does not borrow the caller's buffer.
This immutable shared context is attached best-effort; if its allocation fails,
the field stays empty and the original typed cause survives. Copying a
`ManagedError` allocates no memory and remains `noexcept`.
File failures also retain an OS `error_code`. Original factory/tree/text causes
and independent cleanup errors survive. Default stderr reporting prints the category
and position without echoing untrusted source bytes. Constructing `ui::Scene` can
throw on allocation; checked preparation converts allocation and callback
exceptions to typed errors.

The lazy [recipe](glossary.md#glossary-recipe) owns the resource text, registry
and resolver; it retains no views into temporary source buffers. Prepare the
scene on its UI thread. A nested lifecycle operation returns `WOULD_BLOCK`.
Mounted callbacks and named refs retain
their existing checked lifetime rules. See [text properties](text-property.md),
[style layers](style-layers.md) and [managed controls](managed-controls.md).
