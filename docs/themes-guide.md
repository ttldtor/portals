# Choosing and updating a theme {#themes-guide}

Implemented API examples below describe baseline `84b865e`; sections marked **TBD** describe future work.

A theme is an ordinary stylesheet that supplies a palette. It changes
presentation; it does not change input behavior or introduce another runtime.
Use [styles](styling-guide.md) for application-specific overrides.

## Choose a built-in palette

This fragment includes `<portals/ui.hpp>` and assumes an idle `ui::Scene`:

```cpp
scene | ui::theme(nordTheme());
```

| Factory | Appearance |
| --- | --- |
| `modernTheme()` | Modern dark palette |
| `turboTheme()` | Turbo-like palette |
| `nordTheme()` | Nord dark palette |
| `gruvboxTheme()` | Gruvbox dark palette |
| `tokyoNightTheme()` | Tokyo Night dark palette |
| `catppuccinLatteTheme()` | Catppuccin Latte light palette |

Factories return owning `StyleSheet` values and may allocate before installation.
For a prepared managed scene, `ManagedScene::theme(sheet)` returns the checked
installation result. The UI facade collects setup failures for `checked()` or
the runner. Replace a theme between frames on the scene's UI thread.

## Supply your own six colors

`ThemeTokens` is a C++ palette, not a PSS variable declaration. This setup
fragment replaces the theme using six explicit RGB values:

```cpp
scene | ui::theme(themeFromTokens(ThemeTokens{
  .foreground = RgbColor{220, 225, 232},
  .background = RgbColor{24, 28, 36},
  .accent = RgbColor{75, 180, 220},
  .muted = RgbColor{125, 135, 150},
  .selection = RgbColor{45, 75, 110},
  .heading = RgbColor{255, 200, 85}
}));
```

| Token | Presentation role |
| --- | --- |
| `foreground` | Normal and selected text |
| `background` | Normal fill, focused text and caret fill |
| `accent` | Focused fill and caret text |
| `muted` | Disabled text |
| `selection` | Selected row/editor range fill |
| `heading` | Header text |

The standard rules request bold for focus, selection and headers. The caret
reverses the focused field colors. These tokens do not cover every possible
widget surface; append ordinary rules for additional presentation choices.

## Keep application overrides across theme changes

Enable layers before installing independently replaceable styles:

```cpp
scene.styleLayers()
     .styleLayer(StyleTier::APPLICATION, "application", applicationStyles)
     .styleLayer(StyleTier::USER, "contrast", userStyles);
scene | ui::theme(gruvboxTheme());
```

This fragment assumes owning `StyleSheet` values named `applicationStyles` and
`userStyles`. Theme replacement changes the `THEME` slot. Application and user
layers stay above it, including when live theme parameters change. An application
foreground override can therefore hide an accent update: the update succeeded,
but that higher declaration still wins. See [layer precedence](style-layers.md).

## Update a typed parameter at runtime

Include `<portals/ui_style.hpp>`. The existing experimental C++23 descriptor
supports named `RgbColor`, `bool` and `std::int32_t` parameters. This fragment
declares a live accent and a derived focused color:

```cpp
namespace sc = portals::style::ct;
constexpr auto THEME = sc::sheet(
  sc::parameters(sc::parameter<"accent">(sc::rgb(75, 180, 220))),
  sc::rule(sc::selector<"Button">(),
           sc::foreground(sc::token<"accent">())),
  sc::rule(sc::selector<"Button">(StyleState::FOCUSED),
           sc::background(sc::mix(sc::token<"accent">(),
                                  sc::rgb(24, 28, 36), 50))));

auto stage = ui::liveTheme(THEME);
auto colors = stage.ref();
scene | stage;
```

After successful attachment, an admitted UI callback can change the accent:

```cpp
auto changed = colors.set<"accent">(sc::rgb(255, 200, 85));
```

Check the result: `true` means changed; `false` means equal; failure preserves
the previous values and stylesheet. `apply(sc::change<"name">(value), ...)`
changes a batch atomically. Names and exact scalar types are checked at compile
time. `mix` is an existing bounded expression with a percentage from 0 to 100;
this is not an arbitrary expression evaluator or a derived-token graph.

The scene owns the attached controller. `colors` is a weak observer, does not
keep the scene alive, and is UI-only for reads/updates. Closure or stale theme
authority produces a typed error. Attach a fresh stage after another theme
owner supersedes it; the old stage does not silently reinstall itself.

## Use files and understand the limits

`themes/modern.pss` and `themes/turbo.pss` match their typed factories. File
source syntax is `ui::style(ui::file(path), options)`; see the [styling guide](styling-guide.md).
Its default tier is `APPLICATION`, so choose layering deliberately rather than
assuming a file loaded there becomes the replaceable `THEME` slot.
There is no implicit file watcher or theme-variable substitution in PSS v0.

## Direction: one PSS source for themes

**TBD — M5, U65:** PSS becomes the single editable source of built-in themes.
Generated C++ materialization and embedded resources will come from that source;
applications will not need theme files in their working directory. This avoids
manually maintaining a second palette table. The existing factories remain the
implemented path; this change has not landed in the documented baseline.

The accepted direction is to validate and load a theme through the style path,
then atomically replace the `THEME` layer. Application and user layers remain
installed. A theme change is replacement of a stylesheet, not an accumulating
patch to computed properties. Default `ui::style(ui::file(path))` currently
installs in `APPLICATION`; it must not be presented as this future theme loader.

**TBD — M5, U65:** common control/state rules will use typed semantic parameters,
with complete light/dark defaults. Themes can override parameters and rules.
Automatic system light/dark detection is a separate, unfinished contract, not
an existing feature or a separately scheduled promise.

**TBD — M5, U65; target data flow (one resolver):**

```text
editable PSS -> generated embedded PSS / file PSS -> validate -> replace THEME
external theme -> importer -> normalized PSS ------^            |
                                                 existing style resolver
                                                 APPLICATION / USER preserved
```

The importer branch is not independently scheduled. This diagram does not
introduce a separate theme runtime.

### Import external themes into PSS

**TBD — U65 direction, M5; importer milestone not independently scheduled:**
an importer will normalize supported external fields into parameters/rules and
emit PSS. External format parsers will not become required retained-core
dependencies, and importing a code-based theme will not execute its code.

Windows Terminal color-scheme JSON and Base16/Tinted YAML are first candidates,
not committed compatibility targets. Each importer needs a documented format
version, field mapping, synthesized defaults, unsupported properties and source
notices. An ANSI palette alone does not define Button, selection, error or
domain-specific roles such as bid/ask; their mapping must be explicit and
configurable. The importer package needs separate decomposition and its added
budget is not yet estimated.

### Runtime parameters are still future work

**TBD — M5, U65/U128:** runtime PSS parameters and expressions follow the
[separate namespaces and checked-update policy](styling-guide.md#future-style-parameters).
The experimental typed C++ `token`/`mix` and `LiveTheme` path above already exists;
it does not make those expressions valid inside a PSS string. `LiveThemeRef`
does not automatically bind an arbitrary domain property; update it explicitly
on UI. Compile-time styles remain a [separate review document](compile-time-styles.md).

## Work through a complete program

Read [the source-based PSS consumer](../tests/package_consumer/style_pss_after.cpp)
and [its shared setup](../tests/package_consumer/style_pss_support.hpp) for layers,
live updates, failed updates and cleanup. For the separate typed descriptor,
read [compile-time styles](compile-time-styles.md) and its complete consumers.
Keep that document as a separate review target rather than mixing its C++ syntax
with future string grammar.

Terminal color mode still affects the result. True color preserves the RGB
palette; ANSI16 quantizes colors and may collapse nearby choices. Bold, italic
and underline are terminal requests, not guarantees of identical appearance.
