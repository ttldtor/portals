# Radio groups {#radio-group}

[Русская версия](ru/radio-group.md) · [Control examples](control-examples.md)

`RadioGroup` displays a resident set of choices and keeps at most one selected
stable value. Each group is one Tab stop. Arrows select the previous or next
available option and wrap at the ends; Home/End select the first/last available
option. Space or Enter selects the active option. Clicking a visible enabled
row selects it. Repeating the current choice leaves the model unchanged.
If selection is absent, hidden or disabled, arrows and Space/Enter start with
the first available option; Home/End retain their endpoint behavior. With no
available option, user input leaves the model unchanged.

## Start with labels

```cpp
ui::Ref<RadioGroup> mode;
auto scene = ui::scene(ui::radioGroup(mode, {"Fast", "Balanced", "Quiet"}));
```

The scene creates and owns the model, generates row identities and initially
selects the first enabled visible option. An empty group has no selection.
The complete [device settings example](../examples/radio_group/main.cpp)
includes model-owned choices, disabled and hidden options, a read-only group,
theme styling, framing and resizing. Build `portals_radio_group_example` with
`PORTALS_BUILD_EXAMPLES=ON`; run the resulting executable in a native terminal.
F10 exits through the standard runner, which restores the terminal and reports
typed setup/runtime errors. The application links to `portals::core` (C++23).

## Use the same model and reference

The following complete client creates scene-owned state without exposing
binding tokens or requiring hand-written IDs or text segmentation:

\include{lineno} guide_radio_group.cpp

`ui::radioGroup(ref, owner.radioState(...))` retains the original checked state
creation error. To share or observe the state, retain its `ManagedState` handle
inside the scene factory. Passing an existing `ManagedState<RadioGroupValue>`
preserves its exact value, including an explicit absence of selection; mounting
never writes an initial default into that external model.

`mode.value()` returns the authoritative owning value with checked affinity and
liveness. `mode.select(value.selectedId())` performs a programmatic selection
and returns the original `PropertyUpdate<RadioGroupValue>` receipt. Pass
`std::nullopt` to clear the selection. The user cannot toggle the selected
option off. Programmatic selection may select a disabled or hidden existing ID;
an unknown full ID returns `INVALID_ARGUMENT` without committing. Failed `select`
commands also enter the existing scene error collector as `radio.select`, including
when a void callback ignores the returned result. Successful receipts remain unchanged;
low-level `ManagedScene::selectRadio` only returns its checked result.

`ui::onChange` receives successful changed **user** commits only. Programmatic
writes, failed input and equal/no-op writes do not notify it. A callback can
accept `const RadioGroupValue&` or the exact
`const PropertyUpdate<RadioGroupValue>&`. Values and receipts own their immutable
source. `selectedText()` borrows from that owner; retain the value/snapshot when
keeping the text after the callback. Use `selectedId()` to distinguish an empty
caption from no selection. Callback failure cannot roll back a committed value.
Appending a callback while delivering an event takes effect on the next event;
removing the group stops the remaining callbacks for that event.

## Options, identity and updates

For per-option availability, use ordinary cold data:

```cpp
ui::radioGroup(std::vector<RadioOption>{
  {"Fast"}, {"Quiet"}, {"Unavailable", false}, {"Legacy", true, false}});
```

The same vector is accepted by `owner.radioState(...)`. Captions also accept
`TextValue` and `i18n("key")`; pass an explicit `KeyTextResolver` when using
resource keys. Resolution and Unicode measurement use the receiving scene's
actual `WidthPolicy` during preparation. Missing resources and invalid text keep
their original typed diagnostic. There are no model reads or user formatters
in painting/layout. Control characters use the existing text placeholders.

For domain data that is reordered, renamed or replaced, construct
`RowCollection<RadioItem>` with existing full `RowId {value, generation}`
identities and validated `GraphemeText`, then create a checked
`RadioGroupValue::from(rows, selectedId)`. Reuse full IDs for the same domain
value; advance the generation when reusing an ID for another value. A source
with mixed width policies or an absent selected ID is rejected. A new value can
carry a replacement selection or `nullopt` in the same atomic state update.

`withSelection(...)` shares the source rows and prepared navigation index.
Changing selection does not copy the row vector. Generated shorthand IDs are
local to their immutable source; they do not promise identity across independently
created label collections. Pointer input uses the last arranged source ID and
checks it against the latest model. A removed/now-ineligible candidate or replaced
shorthand source consumes the stale click without committing or calling
`onChange`; the existing model synchronization refreshes the display. Reordered
explicit IDs continue to identify the same domain row.

Hidden items occupy no row. Disabled items remain visible. Both are skipped
by user navigation; an already selected disabled/hidden item retains its value.
`.settings({.readOnly=true})` retains focus and programmatic writes while
prohibiting user mutation. Group visibility/enabled state follows the ordinary
tree/router rules. The default resident cap is the existing configurable
`CollectionLimits::maxRows` of 4096; oversized sources fail without truncation.
This control does not provide a lazy/paged source or a search popup.

## Style and size

`RadioGroup` is the natural style identity for typed and loaded controls.
Ordinary IDs/classes, `Control` selectors, theme layers and manual `ControlStyle`
normal/focused/disabled appearances apply. The focused appearance marks the
active option; disabled appearance takes precedence. Chrome uses the same style
as its label. `( ) ` and `(*) ` have equal measured widths.

Natural width is the widest visible caption plus chrome; natural height is the
number of visible options. The standard fit/min/max/flex and frame modifiers
remain available. Narrow allocations clip complete graphemes. Bound painting
visits only rows intersecting the cumulative ancestor clip. A directly created,
unbound low-level group is safely empty and paints its own local allocation;
binding supplies the checked tree geometry context. Low-level callers retain
the `RadioGroupBinding`, call `sync()` for model updates, and use
`syncRadioGroup` for focus/eligibility presentation between frames.

## Load a scene

The builtin `RadioGroup` creates an empty internally owned source. Supply real
options or an authoritative code model through the existing explicit
`SceneResourceRegistry::add<RadioGroup>` factory. No resource array grammar or
delimiter-encoded caption list is implied. A factory returning
`ui::radioGroup({"Fast", "Balanced", "Quiet"})` can be registered as `Modes`;
the registry forwards the applicable scalar settings and preserves the natural
`RadioGroup` style identity. Resolve `scene.get<RadioGroup>("mode")` to use the
same `value`, `select` and `onChange` API.

Complete loaded client:

\include{lineno} guide_radio_group_resource.cpp

```text
scene {
  Modes#mode {
    radioInitial.index = 1
    readOnly = false
    width = 24
    classes = "settings"
  }
}
```

`radioInitial.index` is the zero-based original option ordinal, including hidden
options. `radioInitial.enabled=false` explicitly starts unselected; true requires
an index. False together with an index is invalid. An out-of-range index is
invalid. These literals apply only to an internally created shorthand model;
any initial override with an external authoritative state returns a typed
`INVALID_ARGUMENT` instead of overwriting the model. `readOnly` and common
sizing/frame properties are shared with the typed recipe. Captions, models,
handlers and localization resolvers remain code-owned.

Binding to another scene or adding a duplicate binding returns
`INVALID_ARGUMENT`; existing thread, node lifetime, frame and budget errors
propagate. A successful model receipt may report rejected/cancelled observer
notifications; this does not undo the commit. Do not retry equal writes to
deliver notifications: observe/read the latest snapshot to resynchronize.
