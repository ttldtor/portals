# Styling a scene {#styling-guide}

[Русская версия](ru/styling-guide.md)

[PSS](glossary.md), Portals' stylesheet syntax, lets an application set colors,
text appearance and minimum widths. Controls, layout and actions are declared
through the C++ `ui` API.

## A styled scene

```cpp
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

int main() {
  using namespace portals;

  return ui::scene(ui::vbox(
    ui::label("F10: quit"),
    ui::button("Save").id("save")))
    | ui::theme(nordTheme())
    | ui::style(R"(
        Button#save { min-width: 12; bold: true; }
        Button#save:focused { background: #4bb4dc; foreground: #181c24; }
      )")
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

The rules target the button with `.id("save")`. F10 exits the application.
Managed buttons automatically have the `Button` style type, so this rule needs
no explicit metadata registration. Their automatic targets also match `Control`
selectors; [the style reference](styles.md) describes explicit target replacement.
The [short guide](index.md) describes build requirements, and
[package setup](package.md) explains how a client links the library.
The compact runner reports errors and attempts terminal restoration.
The factory overload in [declarative UI](declarative-ui.md) provides checked
[recipe](glossary.md#glossary-recipe) construction.

The setup fragments below assume an open, idle [scene](glossary.md#glossary-scene), `ui::Scene`, named `scene`,
`using namespace portals;`, and `<portals/ui_style.hpp>`.

## A simple style rule

```cpp
scene
  | ui::style("Button { bold: true; }");
```

`Button` is a [selector](glossary.md) for a control type.
`bold: true;` sets its text appearance.

`ui::style(...)` returns a configuration object called a
[UI stage](glossary.md#glossary-ui-stage), with type `ui::StyleStage`.
It [owns](glossary.md#glossary-ownership) the parsed stylesheet.
The original input string can be released after `ui::style(...)` returns.
Standard controls have style metadata automatically.
Custom controls must register metadata and paint their computed styles; see
[the style reference](styles.md).

This notation describes the structure of PSS v0. Square brackets mark optional parts:

```text
stylesheet  := rule ...
rule        := selector { property: value; ... }
selector    := [Type or *] [.class ...] [#id] [:state]
```

ID, class and state predicates may appear in any order. A selector accepts at
most one ID and one state; all predicates must match. A selector without a type
starts with `.`, `#` or `:`. Identifiers begin with an ASCII letter or underscore,
followed by letters, digits, underscores or hyphens. Comments use `/* ... */`.
IDs are case-sensitive. Every declaration ends with a semicolon.

| Property | Value | Effect |
|---|---|---|
| `foreground`, `background` | `#RRGGBB` | Repaint |
| `bold`, `italic`, `underline` | `true` or `false` | Repaint |
| `min-width` | Integer from 0 to `INT32_MAX`, in terminal cells | Layout and repaint |

`min-width` requires a selector without a state predicate and sets a hard
allocation minimum. Zero adds no minimum. A clipped viewport does not reduce
allocation below this bound. If a fixed recipe width of 20 conflicts with a
styled minimum of 30, typed style application fails before publishing that
style update; an invalid final policy in a cold recipe fails mount.

States are `normal`, `focused`, `disabled`, `selected`, `header` and `caret`.
A selector without a state applies to every presentation variant; `:normal`
applies only to normal presentation. PSS v0 has no `:hover` state.

## Replaceable groups of rules

Each `ui::style` installation creates or updates a [style layer](glossary.md).
Each layer belongs to a [tier](glossary.md), a precedence group;
the default is `APPLICATION`. A name identifies a layer within its tier:

```cpp
scene
  | ui::style("Button { min-width: 12; }", {.name = "layout"});
scene
  | ui::style("LineEdit { foreground: #ff5555; }",
    {.name = "validation"});
scene
  | ui::style("Button { background: #181c24; }", {.name = "branding"});

scene
  | ui::style("LineEdit { foreground: #55ff55; }",
    {.name = "validation"});
```

The last stage replaces the entire `validation` stylesheet. `layout` and
`branding` remain installed. The rules become active during installation.
Validation itself belongs in application code.

**Names are optional.** A name is useful when a later stage needs to replace a
specific group of rules. Independently constructed unnamed stages create
independent layers.
Reusing the same stage checks its existing layer identity.

The layer key is `(tier, name)`. `APPLICATION/validation` and `USER/validation`
are independent. Replacing a layer preserves its position; a rejected update
preserves its previous stylesheet. `.id("save")` identifies a control for
selectors. Lower-level removal and layer handles are covered in
[the layer reference](style-layers.md).

## How rule precedence works

Each property resolves independently through these tiers:

```text
widget defaults < DEFAULTS < THEME < APPLICATION < USER
```

A higher tier takes priority. Within a tier, selector specificity compares
`(has ID, class count + state count, has type)`. Equal specificity is resolved
by layer installation order and then rule order; later declarations win.
Replacing a named layer keeps that order.

An omitted property retains its lower-priority value. Explicit `false` can
override `true`. Properties have no parent-to-child inheritance in PSS v0.
Application and user layers must be installed before a live theme controller
is attached.

## Stylesheet files

```cpp
scene
  | ui::style(ui::file("themes/modern.pss"), {.name = "palette-file"});
```

`ui::style(ui::file(...))` returns an object that stores its own copy of the path.
The file is read when that object is applied to the scene.
Plain string arguments contain PSS text. File loading currently has no includes,
automatic reload or file watcher. `parsePssDocument(bytes, sourceName)` only parses bytes.

Style-source stages accept `APPLICATION` and `USER`, with `APPLICATION` as
the default. Theme replacement is available through `ui::theme(StyleSheet)`;
details are in [the themes guide](themes-guide.md).

## How PSS differs from CSS and QSS

PSS v0 supports the selectors, states and properties listed above.
[QSS](https://doc.qt.io/qt-6/stylesheet-syntax.html) also provides descendant,
child, property and subcontrol selectors. Those features, selector lists,
chained states, inheritance and length units are absent from PSS v0.
Text color uses `foreground`; width uses terminal cells.

[CSS custom properties](https://www.w3.org/TR/css-variables/) use `--name`
and `var(...)`. Runtime PSS v0 has no variables or functions.
Existing typed C++ parameters are described separately in
[compile-time styles](compile-time-styles.md).

## When a stylesheet is invalid

Setup errors are available through `scene.checked()` or the runner's reporting
policy.
Source diagnostics contain byte offsets and one-based line/byte-column coordinates.
A rejected update preserves the previous stylesheet and computed styles.
Updates must be applied between frames on the UI thread. Stage construction can
throw; the factory runner's error boundary is described in
[declarative UI](declarative-ui.md).

For example, `Button { color: #ff5555; }` produces `UNKNOWN_PROPERTY`.
Replacing `color` with `foreground` and applying the corrected stylesheet
resolves the error. The rejected update leaves the previous styles active.

For one control, `ui::style(ref, source)` or `ui::styleFile(ref, path)` applies
the style. [Reference styles and file loading](declarative-ui.md) describes
their scope and lifetime.

## Planned extensions

<a id="future-style-parameters"></a>

Extended selectors, parameters and expressions are **TBD — M5**.
The [planned features page](planned-features.md#planned-style-parameters)
lists their status, proposed syntax and update rules.
