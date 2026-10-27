# Styles and PSS v0

[Русская версия](ru/styles.md)

`<portals/style.hpp>` provides typed declarations and an optional PSS parser.
Both use `resolveStyle`; a stylesheet never contains actions or model updates.
All strings and rules are owned. Parsing and standalone resolution perform no I/O.
`StyleEngine` borrows one `WidgetTree`, which must outlive it, and is used only
on that tree's UI owner thread. The engine's metadata registry observes checked
node identities; removing a widget does not keep it alive.

## Default appearances and theme roles

Manual defaults use terminal-default foreground and background. Focus requests
bold without underline; disabled controls request italic without a fixed gray.
This fallback does not install a theme or guess the terminal palette. Explicit
manual colors and underline remain available through the existing style options.

A managed scene paints its resolved normal style behind its controls across the
terminal viewport. Theme backgrounds therefore cover layout gaps and unused
space automatically. Use a `Scene` selector to override the scene surface through
the ordinary stylesheet layers. Without theme or style declarations, the surface
keeps terminal-default colors. Ordinary widgets and layouts remain transparent
unless their own painting fills an area.

Built-in themes derive presentation from their `ThemeTokens`:

| Role | Controls | Theme mapping |
| --- | --- | --- |
| Focused action or input | Button, CheckBox, ThreeState, LineEdit, Password, ScrollBar, KeyHints action, FindBar navigation | Theme background as foreground; accent as fill; bold |
| Focused frame line, title, title ends | Border, framed ancestors, dialog panels, ScrollArea/LogView built-in frames | Accent as foreground; theme background as fill; bold |
| Selected row or range | VirtualList, VirtualTable, LineEdit, Password | Foreground on selection fill; bold |
| Disabled | Controls with disabled presentation | Muted foreground on theme background |
| Header | Table headings | Heading foreground on theme background; bold |

Passive labels and indicators do not become focus targets because a focus style
exists. Frame-part rules affect frame glyphs without imposing a separate palette
on the control content. Changing the active theme changes the frame colors too.
The Modern and Turbo PSS files provide the same rules as their typed factories.
`style::ct::themeFromTokens` also derives frame parts from its live `accent` and
`background` parameters, matching the runtime factory.

Selection is independent of keyboard focus. Without a theme, its fallback is
bold and italic with terminal-default colors, distinct from bold focused text;
an explicitly empty selected style stays
empty. Built-in themes explicitly select upright text (`italic: false`) for selected
rows and ranges, overriding this unthemed fallback through the ordinary cascade.
An application/user PSS layer can explicitly request `italic: true` again.
The host terminal and font determine how those decorations appear.
The editor software caret retains its intentional underline independently
of control focus. Chart and search-specific highlights keep their semantic roles.

Raw constructors, fluent factories and compile-time descriptions use the same
manual defaults. Theme and application/user PSS precedence is unchanged. Built-in
themes do not force underline off: an explicit manual underline survives unless
a higher-priority declaration overrides it.

## Typical use

```cpp
portals::StyleEngine styles(tree);
auto bound = styles.bind(button, {"Button", "save", {"primary"}});
if (!bound) {
  // Report bound.error() through the application's diagnostic owner.
  return;
}
auto installed = styles.replace(portals::modernTheme());
if (!installed) {
  // The preceding sheet and every bound widget are still unchanged.
  return;
}
```

Binding metadata is explicit. `Button`, `CheckBox`, `LineEdit`, `List`, and
`Table` are useful application names, not a closed type registry or RTTI names.
Custom widgets use the same mechanism. One engine owns a node's theme at a
time; assigning the same node to several engines is unsupported.

Managed Label, Button, CheckBox, LineEdit and KeyHints use those type names
automatically; `ui::plainButton` uses `Button`. Their automatic targets set
`StyleTarget::control = true`, which also lets a `Control` selector match.
This is an additional selector match, with the selector's ordinary specificity;
it adds no inheritance or implicit priority. Custom Control subclasses retain
the default `Control` name rather than an RTTI-derived name.

Explicit low-level `StyleTarget` values default `control` to false. Their type
matching remains exact unless this flag is enabled; the example above therefore
matches `Button` rules but does not opt into `Control` rules. Assigning a complete
target through `scene.style(node, target)` replaces metadata, including the flag.
Recipe `.style(target)` follows that contract with its existing ID modifier rules;
`.id()` changes the ID while preserving the other fields.

The low-level `parsePssDocument(bytes, sourceName)` parser performs no file I/O.
For a native UI scene, `ui::file(path)` creates an owning native-path source;
`ui::style(ui::file(path))` reads and parses it when that stage is consumed by
the `Scene`. This is an explicit file-loading boundary. There is no file watcher,
include directive, or hot reload. `themes/modern.pss` and `themes/turbo.pss` exactly
match `modernTheme()` and `turboTheme()`. `themeFromTokens(ThemeTokens)` customizes
the same typed palette.

## Native scene styling

Include `<portals/ui_style.hpp>` for the native facade. `ui::style(source, options)` accepts
strict PSS bytes, a mutable `PssDocument` (validated and deep-copied), a checked `sc::CompactSheet`, or an explicit `ui::FileSource`.
The stage owns its source and parsed data; caller buffers and documents need
only remain alive through stage construction. `ui::file(path)` owns a copy of
the native filesystem path and defers reading until scene consumption.

The same operation is available as `scene.style(source, options)`,
`ui::style(scene, ui::style(source, options))`, or
`scene | ui::style(source, options)`. These forms use the same stage and checked
`Scene` boundary. For named scenes the lvalue method and pipe preserve the owner;
the free function takes the owner and stage explicitly.

~~~cpp
scene.style(STYLES, {.name = "demo"});
ui::style(scene, ui::style(STYLES, {.name = "demo"}));
scene
  | ui::style(STYLES, {.name = "demo"});
~~~

`StyleOptions` defaults to the `APPLICATION` tier. Choose `USER` explicitly when that
precedence is intended. A source stage has one shared identity: reusing it
checks the original layer handle, retains its position, and reports a typed
stale error if that layer was removed. Supplying a nonempty name opts into
tier-and-name upsert, so a separately constructed stage with the same tier and
name replaces that named layer while preserving its position.

Install style layers before attaching a `ui::liveTheme(...)` stage. Layer
activation retains the current theme as `THEME`; application and user overrides
remain above it. `LiveTheme<S>` keeps its existing typed parameter model and
`LiveThemeRef<S>` is a weak observer returned by the stage's `ref()` method.
Copies of a live stage share one identity bound to one `Scene`. A failed
first attachment may be retried; after successful attachment the identity cannot retarget to another `Scene`. Cleanup closes its
weak references and releases the `Scene`-owned controller. After an external
theme handoff makes its lease stale, the old stage does not reinstall itself;
create a fresh live stage for a new binding.

PSS provenance retains source byte coordinates (zero-based offsets, one-based
lines and byte columns) with the shared source identity. Error labels retain
only a bounded valid UTF-8 prefix; invalid UTF-8 or NUL labels are empty.
Snapshots are available through `ManagedScene::styleSources()` as per-layer
metadata. A full rule/property inspector remains future work. Metadata-only
updates whose computed values are equal do not request redraw. Document and
layer limits are logical payload budgets, not process RSS limits.

Typed `StyleSheet` values and `sc::LiveTheme` remain available alongside the
string frontend. PSS v0 adds no variables, file includes, cascade extensions,
or CSS virtual machine.

## Built-in palettes

Alongside Modern and Turbo, `nordTheme()`, `gruvboxTheme()`, `tokyoNightTheme()`,
and `catppuccinLatteTheme()` return ordinary typed stylesheets. Catppuccin Latte
has a light background; the other three have dark backgrounds. Install any of
them with `StyleEngine::replace` or `ManagedScene::theme` between frames.
All factories may allocate and throw before installation starts.

These four palettes adapt RGB values from Textual's
[pinned theme definitions](https://github.com/Textualize/textual/blob/06dbeef4bb70fb718236aa418ed658ef4667a126/src/textual/theme.py).
The mapping deliberately fits the existing six `ThemeTokens`:

| Portals token | Textual source field | Presentation role |
| --- | --- | --- |
| `foreground` | `foreground` | Normal and selected text |
| `background` | `background` | Normal fill; focused text; caret fill |
| `accent` | `primary` | Focused fill and caret text |
| `muted` | `secondary` | Disabled text |
| `selection` | `panel` | Selected row or editor range fill |
| `heading` | `accent` | Header text |

Focus, selection, and headers request bold; disabled content and the caret do
not. The caret reverses the focused field colors. Palettes change presentation
only: they add no layout metrics, input behavior, or runtime dependency.

This is a palette adaptation, not a reproduction of Textual's complete theme
system. Textual's derived colors, alpha selection colors, surfaces, and
widget-specific variables are not imported. The source commit and complete MIT
notice are retained in [vendor/textual](../vendor/textual/README.md).

## Grammar and cascade

```css
/* ASCII identifiers are case sensitive. */
* { foreground: #dce1e8; background: #181c24; }
Button.primary#save { min-width: 20; bold: true; }
Button.primary#save:focused { background: #4bb4dc; }
Table:selected { background: #2d4b6e; }
Table:header { foreground: #4bb4dc; bold: true; }
```

A selector is an optional type (or `*`), followed immediately by any number of
`.class` predicates, at most one `#id`, and at most one `:state`. Their order
may vary. Every predicate must match. A selector with no type can start with
`.`, `#`, or `:`. Spaces separate rules and declarations; they do not create
descendant selectors. Identifiers start with an ASCII letter or underscore,
then contain letters, digits, underscores or hyphens. Comments use `/* ... */`.
There are no lists, combinators, escapes, variables, inheritance, units or
browser CSS compatibility promises. A semicolon is required after each value.

| Property | Value | Effect |
| --- | --- | --- |
| `foreground`, `background` | `#RRGGBB` | Paint |
| `bold`, `italic`, `underline` | `true` or `false` | Paint |
| `min-width` | Integer from 0 to INT32_MAX, in cells | Layout and paint |
| `frame-line-foreground`, `frame-title-foreground`, `frame-ends-foreground` (also `-background`) | `#RRGGBB` | Frame part paint |
| `frame-line-bold`, `frame-title-bold`, `frame-ends-bold` (also `-italic`, `-underline`) | `true` or `false` | Frame part paint |

`min-width` is allowed only without a state predicate: changing focus must not
silently alter geometry. It sets a hard allocation minimum; zero adds no
minimum. A clipped viewport does not shrink allocation below the bound. If the
minimum conflicts with a widget's size policy, typed style staging rejects the
update and preserves the last valid style. Font size,
text width policy, border glyph selection and row heights are not PSS v0 tokens.

States are `normal`, `focused`, `disabled`, `selected`, `header`, and `caret`.
They select presentation variants, not behavioral flags. Controls use their
existing synchronized focus/eligibility. List/table selection remains a stable
row identity; the editor keeps its own selection and caret. Generic widgets
can read the complete `ComputedStyle` and choose the appropriate variant.
A rule without a state matches every variant; `:normal` matches only normal.

Each declaration cascades independently. Specificity compares `(has id,
number of classes plus state predicates, has type)` lexicographically. Later
rules win ties. Missing declarations preserve lower-priority values. Every
variant starts from the widget's current manual options, supplied through
`Widget::styleDefaults()`. Thus a rule setting only a foreground preserves a
manually specified underline or background. The theme wins for fields it does
specify. An unbound widget continues using its ordinary options. After changing
manual options on a bound widget, call `styles.refresh()` before the next frame;
that recomputes the defaults beneath the active sheet. Replacing with an empty
sheet restores the latest defaults. Destroying the engine leaves the last
computed styles installed; it does not mutate the tree from a destructor.

## Atomicity and errors

`replace`, `replacePss`, `bind`, and `refresh` first validate the boundary and
stage every bound widget. They reject wrong-thread calls and active tree frames.
PSS failure reports one-based line and byte column, plus a static message.
Typed invalid metrics/states, exhausted rule/registry budgets, stale explicit
targets and allocation failures return `StyleError` with a `TreeError` cause.
By-value argument construction and theme factory allocation can throw before
entry. No partial sheet or partial widget update is published.

For example, applying `min-width: 30` to a widget with exact recipe width 20
fails during typed style staging and preserves its last valid style. This claim
describes that checked style-update path; it does not promise that every error
in a hierarchical style setup is detected before publication.

Custom `styleDefaults()` hooks run during staging under a tree frame borrow.
They must not perform I/O, mutate domain state, retain references, destroy the
engine/tree, or recursively invoke extension hooks. Tree mutation and recursive
style installation are rejected. A throwing hook aborts the complete transaction;
`std::bad_alloc` becomes `NO_MEMORY`, other exceptions become `CALLBACK_FAILED`.
After staging, node liveness is checked again. Commit performs one coalesced
invalidation, nonthrowing value assignments and swaps; it invokes no extension.
Stale registrations from prior removals are pruned on the next successful call.

Every committed operation advances the engine revision and installs that same
revision on all bound widgets, even when computed values compare equal. Equal
values request no frame. Paint-only differences request `PAINT`; a changed
minimum width requests `LAYOUT`, which includes painting. Replacement during a
frame returns `WOULD_BLOCK` and retains its prior revision. The application may
retry between frames; there is no hidden queue. Paint reads the installed values
without invoking custom defaults again. `MAX_PSS_BYTES`, `MAX_STYLE_RULES`, and
the configurable engine `maxBindings` bound retained source/registry work.

## ASCII and sixteen colors

Built-in button/checkbox decorations and table/editor scaffolding use ASCII,
so all built-in themes need no icon font. Application text still follows the existing
Unicode width policy; styling does not transliterate user data.

```cpp
portals::FrameWriterLimits limits;
limits.colorMode = portals::ColorMode::ANSI16;
portals::FrameWriter writer(output, limits);
```

`ANSI16` quantizes RGB to the nearest conventional 16-color palette entry by
squared RGB distance, choosing the lower index on ties. The existing encoder
writes real SGR 30–37/90–97 foreground and 40–47/100–107 background codes.
It does not emit RGB SGR in this mode. Physical colors may differ when the user
customizes the terminal palette. Text and bold/italic/underline requests remain
unchanged; terminal support determines how attributes look.
Nearby colors can quantize to the same entry, so selection or disabled colors
may lose distinctions available in true color. Bold selection and header
requests remain, and the focused editor caret uses contrasting encoded colors
in all six built-in palettes.

The writer keeps its color mode for its entire lifetime, including partial
output and subsequent diffs. Direct `encodeDiff(previous, next, cap, mode)`
callers must use a consistent mode for both frames; changing mode requires
`previous == nullptr` for a full repaint. The default remains `TRUE_COLOR`.
The terminal owner chooses capabilities explicitly; there is no environment
probe, implicit negotiation or extra renderer.

The headless `portals.style` test checks parser diagnostics, compound cascade,
typed/PSS parity, failed staging, coherent revisions, real control/table/editor
paint, metric invalidation, unchanged actions, all four adapted palette state
variants, caret contrast in all six themes, and partial ANSI16 writer output.
Native visual appearance remains a separate platform check.

### Allocation boundaries and value transfers

Style targets, selectors, rules and sheets remain aggregate values suitable for
ordinary C++ initialization. They deliberately suppress implicit `noexcept`
moves: the MSVC checked standard library may allocate iterator proxies while
moving a container. Transfers therefore copy their owned strings/vectors and
may throw. Their explicit empty storage constructors also permit allocation
failure to propagate. The parser and engine catch internal copies, including
construction of the returned `expected`; argument construction by the caller
and standalone theme factories remain ordinary throwing C++ operations.

Style updates may copy the current metadata/rules while staging, so their cost
is proportional to bound widgets and rules (plus matching class strings).
They belong between frames. The final commit only swaps storage and assigns
fixed-size computed values. A failure sweep in the regression test injects an
allocation failure at each budget up to the first successful commit and checks
that every preceding failure leaves revision, styles and invalidation intact.
