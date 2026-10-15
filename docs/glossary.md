# Glossary {#glossary}

| Term | Meaning in Portals | Read next |
|---|---|---|
| Description / recipe | Owning single-use value describing controls to construct; awaiting construction and mount | [Definition](#glossary-recipe) |
| UI stage | Object describing an operation applied to a scene with the pipe operator | [Definition](#glossary-ui-stage) |
| Ownership | Responsibility for storing data and keeping it alive | [Definition](#glossary-ownership) |
| Borrowing | Temporary access to data stored by another owner | [Ownership](#glossary-ownership) |
| Grapheme | Text cluster treated as one cursor/editing unit | [Text units](#glossary-text-units) |
| Terminal cell | One column and row position of the terminal grid | [Text units](#glossary-text-units) |
| Frame | One layout/paint result encoded in a cell buffer | [Definition](#glossary-frame) |
| Model pin | Retained model data and availability captured by `ModelFrame<T>` | [Definition](#glossary-model-pin) |
| Backpressure | Refusal or delay when bounded delivery capacity is exhausted | [Definition](#glossary-backpressure) |
| Cancellation scope | `UiScope` groups subscriptions and timers for collective cancellation | [Definition](#glossary-cancellation) |
| Scene | The `ui::Scene` facade for recipe, setup, running and cleanup | [Definition](#glossary-scene) |
| ManagedScene | Scene-owned registrations, properties and models over the existing Application/tree | [Managed controls](managed-controls.md) |
| Ref | Observing typed handle; copying it does not keep a scene or widget alive | [Text properties](text-property.md) |
| Node | Checked tree identity with slot/generation; observes a mounted widget | [Widget tree](widget-tree.md) |
| Property | UI-owned typed value publishing immutable snapshots | [Properties](property.md) |
| ManagedState | Weak handle to a scene-owned Property | [Managed controls](managed-controls.md) |
| Snapshot | Owning immutable value/revision retained independently of later updates | [Properties](property.md) |
| Binding | Explicit connection between model/state and a view; ordinary text assignment copies a value | [Bindings guide](bindings-guide.md) |
| PSS | Portals stylesheet syntax; resembles CSS but has its own small grammar | [Styling guide](styling-guide.md) |
| Theme | A palette expressed as ordinary style rules, installed in the THEME tier | [Themes guide](themes-guide.md) |
| Theme token | Named palette value; typed theme parameters differ from unsupported PSS string variables | [Themes guide](themes-guide.md) |
| LiveTheme | Typed parameter controller which recomputes ordinary sheets using the existing resolver | [Compile-time styles](compile-time-styles.md) |
| Layer / tier | Independently installed sheet / its precedence class | [Style layers](style-layers.md) |
| Selector | Predicate over explicit type/id/classes/state metadata | [Styles](styles.md) |
| Invalidation | Request to recompute layout or paint; layout includes paint | [Properties](property.md) |
| UI owner thread | Thread on which scene/tree mutation and lifecycle operations are admitted | [Managed controls](managed-controls.md) |
| Diagnostic / final report | Operation record / report delivered after attempted cleanup | [Declarative UI](declarative-ui.md) |
| `$name` | **TBD — M5:** code-provided or observed parameter, separate from theme parameters | [Themes](themes-guide.md) |
| `theme.*` | **TBD — M5:** namespace of semantic PSS theme parameters | [Themes](themes-guide.md) |
| `this.text` / `len` | **TBD — M5:** localized control text / its terminal-cell width under WidthPolicy | [Styling](styling-guide.md) |
| Checked parameter assignment | **TBD — M5:** validate a candidate before atomic publication; optional policy must be explicit | [Themes](themes-guide.md) |
| TBD | Planned behavior or unfinished guide section; see the planned-feature milestones | [Planned features](planned-features.md) |
| Milestone | Planned group and order of work; delivery dates are specified separately | [Planned features](planned-features.md) |

## Recipe

<a id="glossary-recipe"></a>

A recipe is a `ui::Description<T>` value describing how to create a control
and its children. For example, `ui::label("Hello")` describes a label;
`ui::vbox(ui::label("Hello"))` describes a container with that label.
The recipe stores constructor arguments, child recipes and callbacks.
It can be moved and is consumed once by mounting, including a failed mount.
Creating a recipe does not insert controls into a tree.

## Scene

<a id="glossary-scene"></a>

`ui::scene(std::move(recipe))` returns a `ui::Scene` object that owns the application's
runtime and manages preparation, configuration, running and cleanup.
Construction stores the recipe; it does not mount controls or enter terminal
mode. Preparation mounts the recipe: it creates controls in `WidgetTree`
and registers their bindings with `ManagedScene`. `ui::runApp()` prepares
the scene before running it. See [declarative composition](declarative-ui.md)
for preparation errors and lifecycle constraints.

## UI stage

<a id="glossary-ui-stage"></a>

A UI stage describes an operation applied to a scene with `|`.
`ui::style(...)` returns a configuration object that specifies stylesheet rules.
For example, `auto styling = ui::style(pss);` creates a `ui::StyleStage`.
`scene | styling` applies its rules to the scene. The stage can also appear
inline: `scene | ui::style(pss)`.
`ui::runApp()` returns a stage that runs the scene and produces an exit code.

## Ownership

<a id="glossary-ownership"></a>

An owning object stores the data it needs for its lifetime. The style object
returned by `ui::style(pss)` owns the parsed stylesheet, so it does not need
the caller's original string after that call returns. A borrowed reference is valid only while its owner and the granting operation
permit access. For example, references supplied by `WidgetTree::read` are limited
to its callback. An observing reference
such as `ui::Ref<T>` accesses another owner's object and does not extend its lifetime.

## Text units

<a id="glossary-text-units"></a>

`GraphemeText` divides Unicode text into extended grapheme clusters. A letter
and its combining accent can form one cluster used for cursor movement.
A terminal cell is one grid position. `WidthPolicy` determines how many cells
a supported text cluster occupies; bytes, clusters and cells are counted separately.
See [text and width](text.md).

## Frame

<a id="glossary-frame"></a>

The application prepares data, calculates layout and consumes the widgets' paint
commands into `CellBuffer`. `FrameWriter` then encodes the changed cells for the
terminal. Entering `WidgetTree::frame` restricts which tree operations are allowed;
frame preparation and output delivery are described in [the runner](application.md).

## Model pin

<a id="glossary-model-pin"></a>

`model.pin()` captures a `ModelFrame<T>` that keeps the provider snapshot and
one immutable table of consumer results alive. Pass the same pin to every view
of that model during a frame so they see consistent data and availability.
This model frame is distinct from the painted terminal frame. See [models](model.md).

## Backpressure

<a id="glossary-backpressure"></a>

A bounded queue can refuse a publication with `WOULD_BLOCK` when it is full.
The caller decides whether to retry after delivery, report the refusal, or use
an explicitly permitted replacement policy. See [scheduler queues](scheduler.md).

## Cancellation scope

<a id="glossary-cancellation"></a>

`UiScope` groups scheduler registrations. Closing it prevents new invocations,
including queued deliveries; a running callback keeps its captures until it returns.
Model requests use cooperative cancellation through `request.cancel()`.
Stopping and joining producer threads remains the application's responsibility.
See [scheduler cancellation](scheduler.md) and [model requests](model.md).
