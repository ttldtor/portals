# Compile-time scene descriptions {#compile_time_scenes}

Include `<portals/ui_static.hpp>` to build an owning scene descriptor in C++23
constant evaluation. The experimental `ui::ct` layer describes existing `Label`,
`Button`, `LineEdit`, `Row` and `Column` controls. Mounting, input, state and
cleanup still use the ordinary managed scene and its retained tree.

## Describe structure, attach runtime behavior

```cpp
constexpr auto FORM = ui::ct::scene(
  ui::ct::column<"form">(
    ui::ct::label("Name"),
    ui::ct::lineEdit<"name">("Portals"),
    ui::ct::button<"apply">("Apply")));

ui::Ref<LineEdit> name;

auto description = ui::ct::describe(
  FORM, ui::ct::reference<"name">(name),
  ui::ct::onClick<"apply">([name] {
    name.text() += "!";
  }));

auto scene = ui::scene(std::move(description));
```

This fragment stops before choosing a terminal and running the scene. A complete
consumer must handle `scene.run(terminal)` and its typed error, including cleanup
failure. See the complete consumers below rather than treating the fragment as
the complete client footprint.

Factories own their literal bytes and children. Optional IDs are template
arguments; nonempty IDs must be unique. `scene` seals the descriptor. Invalid
layout or size policies, duplicate IDs, unknown adapter IDs, wrong reference
types and duplicate adapters of the same kind are compile-time errors.

`reference`, `onClick` and `bind` are runtime adapters. `bind<"name">(state)`
uses an existing `ManagedState<std::string>`; the state's current value supplies
the initial text instead of the descriptor's literal. Callbacks keep the existing
managed action contract. References observe mounted controls and become invalid
after their lifetime ends. They do not own controls.

## Complete example

The [static scene client](../tests/package_consumer/static_scene.cpp)
edits a name, activates Apply, updates status, changes styles and resizes.
Its `--failure` path checks typed callback errors and cleanup.
The [shared setup](../tests/package_consumer/static_scene_support.hpp)
contains runtime and error handling.

## What remains runtime

UTF-8 validity, grapheme segmentation, `WidthPolicy` and `TextLimits` use the
existing text engine at mount. A literal containing invalid UTF-8 can pass
constant evaluation and then produce a typed `TextError` during mount. No second
Unicode implementation is introduced. Callback captures, live state and UI
ownership cannot be made compile-time values by this layer.

`describe` builds the existing owning, move-only description. The descriptor can
then disappear; mount does not borrow it. Partial-mount rollback, staged
reference publication, theme switching and single-use scene behavior retain
their existing contracts. Disconnected event captures may remain in the router
until collection or application destruction, as with ordinary descriptions.

This first slice excludes virtual controls, arbitrary custom widgets, loaded
files, reconciliation and compile-time stylesheet parsing. Static descriptors
are not a promise of fewer runtime controls or faster rendering; measure the
same mounted scene when comparing performance.
