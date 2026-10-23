# Loading scenes from text and files {#scene-loading-guide}

[Русская версия](ru/scene-loading-guide.md)

`ui::scene(source)` copies UTF-8 resource text into a lazy
[scene](glossary.md#glossary-scene), `ui::Scene`. Parsing and construction happen at
`prepareChecked()` or preparation inside `run`/`runApp`, before a terminal opens. The API is available through
`<portals/ui.hpp>` or `<portals/ui_scene_resource.hpp>`; the application links
to `portals::core` (C++23). The loaded controls use the same named references, event handlers,
models, PSS and cleanup as controls declared in C++.

## A complete form {#a-complete-form}

The complete example is available as the `portals_scene_resource` target when
`PORTALS_BUILD_EXAMPLES=ON`. Add Hello
appends text; Exit or F10 closes the scene. The native runner restores the
terminal and reports failures.

Sources: [form definition](../examples/scene_resource/form.hpp) and
[application entry point](../examples/scene_resource/main.cpp).

\include{lineno} scene_resource/form.hpp
\include{lineno} scene_resource/main.cpp

A plain string always means source text. You can load a file with an explicit
owning file source:

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
auto scene =
  ui::scene("Button#add{i18n(\"add\")}", std::move(resource));
```

There is no implicit global locale or automatic translation refresh. Missing
resources preserve `ManagedError::resource`; invalid text preserves `text`.
Explicitly borrowed resolver services must outlive prepare.

Register custom types with a concrete public `Widget` subtype and an owning
copyable factory returning `Description<T>` or exactly
`expected<Description<T>, ManagedError>`:

```cpp
ui::SceneResourceOptions resource;

resource.control<Meter>("Meter",
  [](ui::SceneResourceArguments arguments) {
    return ui::custom<Meter>(std::move(arguments.text));
  });

auto scene = ui::scene("Meter#level{\"CPU\" width=20}",
  std::move(resource));
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
one-based line and byte column. Its shared `source` retains the safe label
`<string resource>` for string input, or the file path for file input. It never
retains the document bytes in diagnostics, including parse and limit failures.
This replaces the previous string-source payload; offsets still refer to the
original input, which the caller must retain separately to display an excerpt.
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

## Named variants and initial defaults {#named-control-defaults}

Use an owning `ui::ControlContext` to share initial settings without sharing live
text, cursor, undo history, models or handlers. A named variant keeps its concrete
C++ type: `ShortEdit = LineEdit` is observed with `ui::Ref<LineEdit>`.

The complete native program declares the context once, applies a variant to two
editors and overrides one history limit:

\include named_controls/main.cpp

`Description::settings(ControlSettings)` is a sparse instance patch.
Omitted fields inherit; explicit `false`, `0` and values equal to built-in
defaults still override. The ordinary no-config `ui::lineEdit` overload inherits
context settings. An explicitly supplied legacy `LineEditConfig` retains its
complete options semantics; it is not guessed to be a sparse patch.
When an editor binds an existing managed model, that model supplies the initial
text; a context default does not overwrite its value. Caption factories with an
explicit text argument treat that argument as an instance override.

Resolution follows the concrete base towards the requested variant:
built-in settings, base defaults, variant definition, variant defaults, then
instance settings. `defaults(type, patch)` accepts one block per type.
`overlayDefaults(type, patch)` explicitly merges local defaults with inherited
ones; it creates a new snapshot. Earlier contexts and already prepared scenes do
not change. These are initial settings, not live theme or history reconfiguration.

### The same configuration in a resource

The original single-root grammar remains the default. Select
`SceneResourceFormat::CONFIGURATION` explicitly for definitions/defaults/scene
blocks. Block order and forward references do not affect resolution:

\include named_controls/resource.cpp

Pass `ui::SceneResourceFile{"form.scene"}` in place of the source string to read
the same document from a file at preparation. A nested resource inherits the
scene's context. Its local definitions/defaults affect only that resource's
subtree; `SceneResourceOptions::controls` can explicitly replace the inherited
snapshot. Duplicate defaults blocks in one document remain errors.

### Supported literals and boundaries

| Controls | Additional initial properties |
|---|---|
| Label, Button | `text` |
| LineEdit | `text`, `placeholder`, `editMode`, `readOnly`, `programmaticChange`, `displayMode`, `history` |
| Password | Empty `text`, public `placeholder`, `editMode`, `readOnly`, `displayMode`, `limits`, `securePolicy`, `pastePolicy` |
| CheckBox, ThreeState | `text`, `state`, `cycle` |
| VBox, HBox | `gap` |
| Sized controls | `width`, `height`, `flex` |

`Password#psw { text = "", placeHolder = "Password" }` creates an empty protected
editor. Nonempty text and `i18n(...)` value expressions are rejected before any
resource resolver or factory runs. Code supplies a protected value through
`PasswordConfig::initial`, a captured `ManagedState<PasswordValue>`, or checked
`PasswordRef::value(value)` after preparation. A custom registered Password factory
can capture that provider or model and apply the resource's public settings.
`securePolicy` accepts `maxBytes`, `storage`, `lockPages`, `excludeFromDumps` and
`encryptWhileIdle`; native guarded storage and idle encryption are mandatory.
`pastePolicy` defaults to `DISABLED`. Password has no ordinary text getter,
history, copy or cut. See [protected input](controls.md) for access and platform limits.

History accepts `enabled`, `maxOperations`, `maxBytes`, `groupTyping` and
`groupDeletion`. Text accepts strings and existing `i18n(...)` expressions.
Enums use the names documented by the C++ API. `ThreeState` fixes
`cycle = THREE_STATE`; an attempted downgrade is an error. IDs, classes, models,
callbacks and live undo entries are not inherited from a definition.

Type selectors match the requested name and its ancestors with ordinary type
specificity. Depth adds no priority; IDs, classes, layer ordering and source order
keep their existing meaning.

`ControlContextLimits` defaults to 64 custom schemas/definitions, 32 ancestors
(including intrinsic `CheckBox`, `Column` or `Row` identities),
8 structural levels and 65,536 effective scalar properties across a mount.
The inheritance limit cannot exceed the style system's hard ceiling of 32.
An explicit style type rename shares that ancestor budget; if it adds another
identity beyond the ceiling, mounting fails with a typed style error and rolls
back the mounted subtree.
Resource source/node/string/property limits still apply. Unknown bases, cycles,
built-in shadowing, invalid properties and invalid enum values are checked before
resource resolvers or factories run. Diagnostics retain the winning declaration's
source position and `type.property` path, including inherited values. Allocation
failure may omit diagnostic context while preserving the typed error.

This grammar does not evaluate C++, import other resources, watch files or bind
`$parameters`. Custom control constructors remain explicitly allowlisted through
the resource registry.

## Loaded frames {#loaded-frames}

Select `ui::SceneResourceOptions{.format = ui::SceneResourceFormat::CONFIGURATION}`
when passing these resource properties to `ui::scene` or `ui::resource`. Sized
controls accept an optional decoration using the same frame settings as C++
recipes. A compact preset expands into `frame.enabled=true` and `frame.pattern`:

```text
scene {
  VBox{frame="rounded" frame.title="Actions"
    LineEdit#message{""}
    Button#exit{"Exit"}
  }
}
```

`frame="single"`, `"plain"`, `"rounded"`, `"double"`, `"heavy"` and `"ascii"` are
supported. Compact and expanded assignments cannot duplicate a canonical leaf,
even when their spelling differs. The compact form consumes two property slots
in its containing object. Expanded settings retain the existing per-object limit;
nested groups are checked at their own level, and the resolved-leaf limit still
bounds the complete settings transaction. `maxProperties` defaults to 16.
Long dotted custom-frame declarations can exceed this limit; group related
settings or explicitly raise the resource limit.

Use `frame { enabled=true pattern=DOUBLE sides { left=false right=false } }`
for grouped settings, or `frame.sides.top=false` for one dotted assignment.
`frame.title` accepts literal text or `i18n(...)` and resolves through the scene's
existing text resolver. `frame.titleAlignment` accepts START/CENTER/END;
`frame.titleDecoration` accepts NONE/SPACED/SQUARE/ANGLE/ROUND. For example,
`frame="rounded" frame.title="Sections" frame.titleDecoration=SQUARE` uses the
same title opening as the C++ recipe. For owning custom ends use
`frame.customTitleDecoration { enabled=true prefix="{ " suffix=" }" }`.
Setting `enabled=false` removes the custom override. Omitted leaves preserve
the receiver's settings. Custom ends use the same byte/cell limits and typed
validation as C++. `frame.maxTitleBytes` defaults to 4096. `frame.glyphs` and
`frame.maxPaintCells` retain their existing selection and work-cap semantics.
A disabled frame cannot also receive frame-specific values.

For an owning custom source set `frame.customGlyphs.enabled=true`, followed by
`topLeft`, `topRight`, `bottomLeft`, `bottomRight`, `horizontal` and `vertical`
in that group. Each must be one printable grapheme occupying one cell, at most
64 bytes. Invalid text, enums, dimensions, conflicts and work limits use the
existing typed preparation diagnostics and rollback.

The source node keeps its type, ID and classes: `scene.get<Button>("exit")` still
observes the Button. Layout receives the actual outer frame, including resource
Grid cells. Common dimensions are outer dimensions. ScrollArea configures its
own frame. Default schemas for custom registered sized widgets include frame
properties; an explicit custom property allowlist remains exact and must include
any frame properties it permits. See [frame geometry](controls.md#control-frames)
for sizing, removal, focused styling and ON_FRAME scroll tracks.
