# Styling a scene {#styling-guide}

Implemented API examples below describe baseline `84b865e`; sections marked **TBD** describe future work.

Use PSS for presentation rules and the C++ recipe for controls, layout and actions.
PSS v0 is available now. Scene-wide loading from a file is also available now;
scoped styling through a control `Ref` is **TBD — M1, U127**.

## Start with one rule

This fragment assumes an existing idle `ui::Scene` named `scene` and includes
`<portals/ui_style.hpp>`. A selector names the controls to change; declarations
name the properties to change.

```cpp
scene | ui::style("Button { bold: true; }");
```

The stage owns its parsed input. You can release the original string after
stage construction. Install a named application layer when you want later
installation under the same name to replace that layer instead of adding one:

```cpp
scene | ui::style("Button { foreground: #4bb4dc; bold: true; }",
                  {.name = "buttons"});
```

## Why a layer has a name

`.name` identifies a style layer within its tier. It is not a selector and does
not assign that name to a control. Installing the stage already applies its
rules to matching controls; no later `apply("buttons")` call is required.

These two successive UI setup/update fragments use the default `APPLICATION`
tier. Assume the first installation succeeded and the scene remains open:

```cpp
scene | ui::style("Button { foreground: #4bb4dc; bold: true; }",
                  {.name = "buttons"});
scene | ui::style("Button { foreground: #ffc855; bold: true; }",
                  {.name = "buttons"});
```

The second stage replaces the complete `buttons` sheet, preserving its layer
position. It does not append a second layer or patch individual declarations.
Other layers remain installed. A rejected replacement preserves the earlier
sheet; check the scene's collected setup/update errors.

The key is **tier plus name**: a USER layer called `buttons` is independent of
an APPLICATION layer with that name. Names are optional for one-off rules.
Independent unnamed stages add independent layers; reusing the same stage
checks its existing layer identity. The selector `Button#save`, by contrast,
uses a control's `.id("save")` metadata to choose which control gets a rule.
This source-stage API does not expose an `apply` or `remove` command by name;
explicit lower-level handles are documented in [style layers](style-layers.md).

### Independent rule groups within one tier

Several layers within `APPLICATION` let the application update a rule group
without rebuilding every stylesheet in that tier:

- `layout`: size rules;
- `validation`: presentation of validation results;
- `branding`: application colors.

This fragment assumes an open scene and successful installations. Every stage
uses the default `APPLICATION` tier. These rules change presentation only;
they do not perform data validation.

```cpp
scene | ui::style("Button { min-width: 12; }", {.name = "layout"});
scene | ui::style("LineEdit { foreground: #ff5555; }", {.name = "validation"});
scene | ui::style("Button { background: #181c24; }", {.name = "branding"});

scene | ui::style("LineEdit { foreground: #55ff55; }", {.name = "validation"});
```

The last installation replaces only the `validation` sheet. The `layout` and
`branding` layers remain installed. The position of `validation` is preserved:
replacement does not move it to the end or change layer ordering within this
tier. Selector specificity still applies; layer position breaks equal-specificity
ties.

**A name is optional when individual rule groups do not need replacement.**
For a simple installation, `scene | ui::style("Button { bold: true; }")` is enough.
Unnamed stages can also create independent layers; the name enables targeted
replacement by a new stage rather than separation of rules itself.

## Target a particular control

Recipe `.id()` assigns an owning, case-sensitive selector identity. Here the
button receives the `save` identity before the recipe mounts:

```cpp
ui::Ref<Button> save;
auto scene = ui::scene(ui::vbox(
  ui::label("Ready"),
  ui::button(save, "Save").id("save")));

scene | ui::style(R"(
  Button#save { min-width: 12; bold: true; }
  Button#save:focused { background: #4bb4dc; foreground: #181c24; }
)", {.name = "form"});
```

This is a setup fragment, not a complete program. It installs presentation;
add an action and the runner as shown in [declarative composition](declarative-ui.md).
Standard controls receive their style type automatically. A custom widget needs
explicit metadata and must paint using its computed style; see [styles](styles.md).

## Read the syntax

The following notation is a schematic, not a parser grammar. Brackets mean
optional syntax; they are not literal PSS characters.

```text
stylesheet  := rule ...
rule        := selector { property: value; ... }
selector    := [Type or *] [.class ...] [#id] [:state]
```

ID, class and state predicates may occur in a different order. A selector has
at most one ID and one state; every predicate must match. Without a type, start
with `.`, `#` or `:`. Identifiers begin with an ASCII letter or underscore, then
use letters, digits, underscores or hyphens. Comments use `/* ... */`.

| Property | Accepted value | Effect |
| --- | --- | --- |
| `foreground`, `background` | `#RRGGBB` | Paint |
| `bold`, `italic`, `underline` | `true`, `false` | Paint |
| `min-width` | Integer from 0 to INT32_MAX, in terminal cells | Layout and paint |

A semicolon is required after every value. `min-width` requires a selector
without a state predicate; focus changes cannot silently change geometry.
Zero keeps intrinsic measurement. The layout's size policy and clipping still apply.

Presentation states are `normal`, `focused`, `disabled`, `selected`, `header`
and `caret`. They select computed presentation variants, not arbitrary event
names. A selector without a state matches every variant; `:normal` matches only
normal presentation. There is no `:hover` or left/right modifier selector.

## Decide which declaration wins

Each property resolves independently. In one sheet, specificity compares
`(has ID, class count + state count, has type)`; later rules win equal specificity.
An omitted property keeps its lower-priority value. Explicit `false` can override
`true`. These rules do not imply parent-to-child inheritance.

Scene source stages enable the [layer cascade](style-layers.md):

```text
widget defaults < DEFAULTS < THEME < APPLICATION < USER
```

A higher tier wins before selector specificity. Within a tier, ties use stable
layer installation order, then rule order. Replacing a named layer preserves its
position. The default source tier is `APPLICATION`; opt into `USER` explicitly.
Install application/user layers before attaching a live theme controller.

## Load a file

```cpp
scene | ui::style(ui::file("themes/modern.pss"), {.name = "palette-file"});
```

The path is owned and read when the scene consumes the stage. A plain string is
PSS content, not a guessed filename. There are no includes, watchers or automatic
reload. `parsePssDocument(bytes, sourceName)` itself performs no file I/O.
For a palette that remains independently replaceable, use a `THEME` sheet or
live theme rather than installing the palette above it as an application override.

## Compare with CSS and QSS

The braces and selector spelling are familiar, but PSS is a separate language.
[Qt's QSS syntax](https://doc.qt.io/qt-6/stylesheet-syntax.html) includes descendant
and child selectors, property selectors and subcontrols. PSS v0 has none of those,
no selector lists, no chained states, no inheritance and no length units.
Use `foreground`, not CSS/QSS `color`; use cells, not `px`.

[CSS custom properties](https://www.w3.org/TR/css-variables/) use `--name` and
`var(...)`. PSS v0 accepts neither. Existing typed C++ parameters and bounded
`mix(...)` expressions are covered by [themes](themes-guide.md) and the separate
[compile-time styles document](compile-time-styles.md).

## Handle failures and continue reading

Parsing, admission and installation errors retain typed diagnostics. A rejected
layer update preserves the previous sheet and computed styles; inspect
`scene.checked()` or use the runner's reporting policy. Updates belong between
frames on the UI thread. Construction before checked entry may still throw.
Source diagnostics retain byte offsets and one-based line/byte-column coordinates.

For a complete replay program with string sources, layers, live updates and a
failure path, read [the PSS consumer](../tests/package_consumer/style_pss_after.cpp)
and [its shared setup](../tests/package_consumer/style_pss_support.hpp).
The executable source, not this fragment, owns its cleanup and error policy.

## Future style parameters {#future-style-parameters}

**TBD — M5, U65/U128; invalidation U66, provenance U62:** the following direction
has been recorded, but runtime PSS v0 accepts no variables, functions or units.
The namespaces have different meanings:

| Form | Future meaning |
| --- | --- |
| `$name` | Typed value assigned or bound from code, possibly computed |
| `theme.text` | Semantic parameter of the active PSS theme |
| `this.text` | Resolved property of the current control |

There is no automatic same-name fallback from `$text` to `theme.text`; an
alternative must be explicit in the rule/fallback policy. `theme` and `this`
do not need a dollar prefix.

This is a **TBD — M5, U65/U128 syntax sketch**, not runnable PSS:

```css
/* Proposed syntax; runtime PSS v0 does not accept it. */
Button {
  foreground: $text;
  background: theme.surface;
  min-width: len(this.text) + 2;
}
```

`this.text` means the resolved, formatted localized text, not a resource ID or
UTF-8 byte sequence. `len` in a size expression measures terminal cells with
the same WidthPolicy as layout. Text arguments and language changes invalidate
dependent measurements. Ordinary buttons still get intrinsic sizing without a
mandatory formula; a formula customizes a constraint. Border and padding must
not be counted twice. Exact percentage bases/ranges/rounding remain undecided;
CSS units such as `px`, `em`, `vh` and `vw` are not promised.

### Assign from an event handler

**TBD — M5, U65/U128:** the requested short spelling is a design sketch; its
return signature and explicit-result overload are not finalized:

```cpp
// API sketch; this is not currently compilable Portals API.
ui::currentScene.setParam("width", value);
```

The intended default is checked validation of every affected consumer before
atomic publication. A rejected candidate preserves the engine's previous
parameters and computed styles and reports a typed diagnostic; no manual
`changed` or `.result()` ceremony is intended for ordinary handlers. Rollback
does not undo an external Property/model update or callback side effects.

This targets the scene's upper runtime override context, where theme values are
already available. Theme changes preserve explicit runtime overrides and
re-evaluate their dependencies. Only expressions that use the parameter are
affected; assignment does not overwrite every same-named concrete property.
Coordination with the `USER` tier is **TBD — M5, U65/U128**, without silently
changing existing rule precedence. A scoped currentScene must not retain an
owning global scene or reuse a previous scene outside its admitted context.

**TBD — M5, U65/U128:** optional fall-through is a separate explicit policy,
not the default. In that policy an inapplicable data value makes its declaration
fall through to the next applicable lower declaration/default, rather than
keeping an old successful override forever. Applicability is per consumer.
An unbound parameter remains diagnostic; checked assignment to an unknown
parameter rejects rather than silently creating it. Bad syntax, unknown
functions/fields, memory failures, wrong-thread, closed-scene and lifecycle
errors are not swallowed as successful optional fallback.

**TBD — M5, U65/U128:** `ui::Params`, `ui::params`, notification-driven
`ui::bind(source, transform)`, the fallback operator `??` and a function such as
`grayScale` are candidates, not published APIs or an accepted complete grammar.
Plain captured C++ variables are not automatically observed; callbacks must
use existing Property/model notifications, UI delivery and scene cleanup.
Owned typed values are not injected as executable PSS fragments. Recalculation
targets dependencies, not polling every callback each frame: color changes
request PAINT, sizes LAYOUT, and equal results do not request extra redraw.
The added API/optional-policy budget has not been estimated separately.

**TBD — M1, U127:** scoped control-Ref source admission remains separate.
Existing [compile-time styles](compile-time-styles.md), typed tokens and bounded
`mix` are experimental C++ APIs, not runtime parser features.
