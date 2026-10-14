# Choosing and updating a theme {#themes-guide}

[Русская версия](ru/themes-guide.md)

A [theme](glossary.md) supplies presentation rules and a palette. Install one with `ui::theme`:

```cpp
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

int main() {
  using namespace portals;

  return ui::scene(ui::vbox(
    ui::label("F10: quit"),
    ui::button("Save")))
    | ui::theme(nordTheme())
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

See [the short guide](index.md) and [package setup](package.md) to build
a client. The compact runner attempts cleanup and reports failures;
[declarative UI](declarative-ui.md) describes checked construction.
The fragments below assume an open, idle [scene](glossary.md#glossary-scene), `ui::Scene`, named `scene`,
`using namespace portals;`, and `<portals/ui_style.hpp>`.

## Choose a built-in palette

| Factory | Appearance |
|---|---|
| `modernTheme()` | Modern dark palette |
| `turboTheme()` | Turbo-like palette |
| `nordTheme()` | Nord dark palette |
| `gruvboxTheme()` | Gruvbox dark palette |
| `tokyoNightTheme()` | Tokyo Night dark palette |
| `catppuccinLatteTheme()` | Catppuccin Latte light palette |

Factories return a `StyleSheet` that owns its rules and may allocate.
The `ui::Scene` facade collects installation errors for `scene.checked()` or the runner.
Theme changes belong between frames on the UI thread.

## Supply your own colors

`ThemeTokens` supplies six named palette values, or
[theme tokens](glossary.md), each stored as an `RgbColor`:

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

| Token | Role |
|---|---|
| `foreground` | Normal and selected text |
| `background` | Normal fill, focused text and caret fill |
| `accent` | Focused fill and caret text |
| `muted` | Disabled text |
| `selection` | Selected row or editor range fill |
| `heading` | Header text |

The built-in rules request bold for focus, selection and headers.
The caret reverses the focused field colors. Add PSS rules for other
control-specific choices. Runtime PSS v0 cannot refer to these C++ tokens by name.

## Preserve application styles when changing a theme

```cpp
scene | ui::style("Button { min-width: 12; }", {.name = "layout"})
      | ui::style("Label { bold: true; }",
                  {.tier = StyleTier::USER, .name = "contrast"})
      | ui::theme(gruvboxTheme());
```

Theme replacement updates `THEME`. `APPLICATION` and `USER` layers stay
installed above it. If an application layer sets a foreground color, that
declaration continues to take priority over the theme's foreground.
See [style precedence](styling-guide.md) and [layer contracts](style-layers.md).

## Load palette rules from a file

The repository includes `themes/modern.pss` and `themes/turbo.pss`, matching
their C++ factories. Load their rules as an application layer:

```cpp
scene | ui::style(ui::file("themes/modern.pss"), {.name = "palette-file"});
```

Style-source stages accept `APPLICATION` and `USER`. A palette installed there
can override colors from `ui::theme`. Current theme factories and
`ui::theme(StyleSheet)` replace the reserved `THEME` slot; direct PSS-file theme
loading is **TBD — M5**. PSS v0 has no automatic reload or theme-variable
substitution. See [file loading](styling-guide.md).

## Change a typed color while the application runs

The experimental C++ descriptor supports named `RgbColor`, `bool` and
`std::int32_t` parameters. This setup declares a live accent:

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

The [UI stage](glossary.md#glossary-ui-stage) is the configuration object
returned by `ui::liveTheme(THEME)`; in this example it is stored as `stage`.
Applying `scene | stage` attaches the controller.

After successful attachment, an event handler on the UI thread can update it:

```cpp
auto changed = colors.set<"accent">(sc::rgb(255, 200, 85));
```

Check the result: `true` means changed, `false` means equal, and an error
preserves previous values and styles. `mix` accepts a percentage from 0 to 100.
For atomic batches, exact type checks and controller lifetime, see
[compile-time styles](compile-time-styles.md).
The scene owns the controller. `colors` observes it without extending its lifetime.
Reads and updates require the UI thread. A closed scene or superseded theme
controller returns a typed error; attach a fresh stage for a new controller.

## Terminal appearance

True color preserves RGB values. ANSI16 maps them to a smaller palette and
can merge nearby colors. Bold, italic and underline depend on terminal support.

## Planned theme support

Canonical PSS theme sources and expanded parameters are **TBD — M5**.
Importing external theme formats is **TBD**, with a milestone still to be assigned.
[Planned features](planned-features.md#planned-themes) describes these changes.
