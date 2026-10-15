# Compile-time styles and live parameters {#compile_time_styles}

Include `<portals/style_static.hpp>` for the experimental C++23
`portals::style::ct` layer. It builds owning, constant-evaluated rules and
materializes the ordinary `StyleSheet`. The existing [style engine](styles.md)
still decides specificity, source order, property-wise cascade and invalidation.

For scalar descriptions with short selectors and rules stored as bounded data,
or for the same PSS string grammar at compile time and runtime, see
[Compile-time PSS](compile-time-pss.md). Existing typed sheets and `LiveTheme`
remain available; scalar snapshots do not retain live parameter expressions.

## Describe rules once

```cpp
namespace sc = portals::style::ct;

constexpr auto SHEET = sc::sheet(
  sc::parameters(sc::parameter<"accent">(sc::rgb(75, 180, 220)),
                 sc::parameter<"width">(std::int32_t{2})),
  sc::rule(sc::selector<"Button", "action">(),
           sc::foreground(sc::token<"accent">()),
           sc::minWidth(sc::token<"width">())));
```

`selector<Type, Id, Classes...>(state)` combines a type, optional ID, all listed
classes and one existing `StyleState`. Empty type or `"*"` matches any type;
empty ID matches any ID. Matching retains runtime byte semantics.
Supported declarations are `foreground`, `background`, `bold`, `italic`,
`underline` and `minWidth`.

Parameters have exactly `RgbColor`, `bool` or `std::int32_t` values and unique,
nonempty names. `sheet` checks token names, declaration types, duplicate
properties, rule count and default expressions during constant evaluation.
`rgb` checks components in [0, 255] before narrowing. `mix(left, right, percent)`
owns nested color expressions, accepts [0, 100], and rounds each integral
component down. This is a bounded expression tree, without derived-token
references or a dependency graph. `minWidth` must be nonnegative and can only
appear under `StyleState::ANY`.

`sc::themeFromTokens(defaults, extraParameters, extraRules...)` describes the
same six standard rules as `portals::themeFromTokens`. It supplies named colors
`foreground`, `background`, `accent`, `muted`, `selection` and `heading`.
Additional parameters must have different names; additional rules follow the
standard rules in source order.

## Install and update on the UI thread

`materialize(SHEET)` returns an owning ordinary stylesheet with default values;
allocation may throw. Pass it to the existing theme APIs when live parameters
are unnecessary. `live(scene, SHEET)` installs defaults and returns
`expected<LiveTheme<...>, ManagedError>`. It publishes no handle on failure.

The copyable handle owns its descriptor and current scalar values in shared
state, and observes the scene through its weak lifetime. Copies share values
but do not keep the scene alive. `get<"accent">()` reads a checked value;
`set<"accent">(color)` changes one parameter. A batch is explicit:

```cpp
auto changed = handle.apply(
  sc::change<"accent">(sc::rgb(255, 200, 85)),
  sc::change<"width">(std::int32_t{12}));
```

This fragment assumes a successfully obtained handle. Check `changed` before
using its value. Unknown names, wrong exact value types and repeated batch
names are compile-time errors. Changed batches stage a complete stylesheet and
commit all parameter values only after the existing engine accepts it. A
rejected batch preserves both previous values and installed widget styles.
Equal batches return `false`; with no pending style metadata they allocate
nothing and request no invalidation. They still check the scene boundary and
theme authority. Pending registrations/metadata may require materialization
and change effective styles even when parameter values stay equal. Successful
parameter changes return `true`.

Operations return the original managed/style error: `CLOSED` after close or
destruction, `WRONG_THREAD` on a foreign thread, and existing mutation/frame
boundary failures, including `WOULD_BLOCK` for reentry. Allocation failure
inside live attachment or updates becomes a STYLE-phase `NO_MEMORY` error.
Negative runtime width is rejected by the existing style engine. Copying and
destroying handles are thread-independent; operating on them is UI-only.
In the legacy single-sheet mode, an external `scene.theme(...)` replacement
supersedes the installed sheet until the next unequal parameter update; an equal
update deliberately leaves it alone. Opt into [style layers](style-layers.md) to
keep application/user overrides independent of the theme. In that mode a successful
external theme replacement or another live attachment revokes the old theme owner:
even equal updates and `get()` return a typed stale error. Enabling layers also
revokes controllers attached before the transition; attach a new controller afterwards.

## Complete installed-package consumers

The two programs below change a palette, override its background, change a
button's minimum width, resize and close the same replay terminal. `--failure`
checks a typed style error and scene cleanup. Partial scene configuration or
mount failure retains the existing managed rollback and error-reporting contract.

- [Before: runtime stylesheet](https://github.com/ttldtor/portals/blob/main/tests/package_consumer/static_styles_dynamic.cpp).
- [After: static rules and live parameters](https://github.com/ttldtor/portals/blob/main/tests/package_consumer/static_styles.cpp).
- [Shared setup, errors and cleanup](https://github.com/ttldtor/portals/blob/main/tests/package_consumer/static_styles_support.hpp).
- [Shared replay terminal](https://github.com/ttldtor/portals/blob/main/tests/support/examples/replay/terminal.hpp).

### Before

\include{lineno} static_styles_dynamic.cpp

### After

\include{lineno} static_styles.cpp

### Shared helper

\include{lineno} static_styles_support.hpp

### Replay terminal

\include{lineno} replay/terminal.hpp

Each full client footprint includes its consumer, the complete shared helper
and replay terminal. Moving setup into a helper does not shorten that footprint.
These listings alone make no measured compactness or performance claim.
This layer adds no CSS parser, hierarchical selectors, custom properties,
reactive subscriptions or compile-time virtual controls. See
[compile-time scenes](compile-time-scenes.md) for the separate structure layer.
