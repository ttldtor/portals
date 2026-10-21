# Declarative UI {#declarative-ui}

[Русская версия](ru/declarative-ui.md)

Use the declarative API to arrange controls and attach event handlers.
`ui::scene(...)` creates a [scene](glossary.md#glossary-scene), and
`ui::runApp()` runs the application.

Use [data presentation](data-presentation.md) to show loading and result states
around ordinary content or a managed virtual list/table.

## Run a form

In this program, **Add Hello** appends text to the editor. **Exit** and F10
close the application. **Add Hello** uses a bold style.

[Source](examples/docs_basic.cpp). To connect Portals and build your application,
follow the [short library guide](index.md).

```cpp
#include <portals/ui_style.hpp>

int main() {
  using namespace portals;

  auto scene = ui::scene(ui::vbox(
    ui::lineEdit("").id("message"),
    ui::button("Add Hello").id("add"),
    ui::button("Exit").id("exit")));
  auto message = scene.get("message");
  ui::Ref<Button> add = scene.get("add");
  auto exit = scene.get<Button>("exit");

  return scene
    | ui::style(add, "* { bold: true; }")
    | ui::onClick(add,
                  [message] {
                    message.text() += "Hello!";
                  })
    | ui::onClick(exit, ui::quit)
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
}
```

`ui::vbox` arranges its children vertically; `ui::hbox` arranges them horizontally.
`ui::lineEdit`, `ui::button` and `ui::label` describe the controls to create.
This description is called a [recipe](glossary.md#glossary-recipe). The scene
creates the controls when it is prepared for running.

The `|` chain adds settings and handlers to the scene. Each operation is a
[UI stage](glossary.md#glossary-ui-stage), such as `ui::style(...)` or
`ui::onClick(...)`. The last operation, `ui::runApp()`, runs the event loop,
attempts terminal restoration and scene cleanup, and returns an exit code:
0 on success, 1 on a typed failure. The default error report goes to stderr
after cleanup.

Use `<portals/ui.hpp>` for scenes, controls and handlers. This example includes
`<portals/ui_style.hpp>` because it also uses runtime `ui::style(...)`.
See the [styling guide](styling-guide.md) for PSS syntax.

## Refer to controls

`ui::Ref<T>` lets a handler refer to a control created by the scene. Pass the
reference as the first factory argument, then capture it by value in handlers.
For example, `message.text() += "Hello!"` appends to the editor's text.
See [text properties](text-property.md) for assignment, reads and formatting.

A reference observes the control; the scene owns it. References become usable
after the complete recipe has mounted. Once the control is removed or the scene
closes, the reference cannot access it. Objects captured by C++ reference must
remain alive while their handlers can run.


### Look up a named control

Assign an owning ID with `.id("message")`, then call `scene.get("message")`.
The dynamic observation supports checked `text()` and Button `onClick()` capabilities.
`ui::Ref<Button> add = scene.get("add")` and `scene.get<Button>("add")` require
that the mounted widget derives from `Button`. No call to `get` prepares the scene.
IDs are case sensitive and copied; their input buffers need not survive the call.

Pending requests resolve after the whole recipe mounts and before configure/handler
stages. A missing or empty ID, an ambiguous ID, or a wrong type records
`BIND/INVALID_ARGUMENT` with the owning name, expected type and original call site.
Startup then skips configuration and terminal acquisition. Legacy style selectors may
still match repeated IDs; requesting such an ID is ambiguous. `maxConfigurations`
also bounds the pending lookup queue independently of pending configuration stages.

A lookup after preparation resolves immediately and reports recoverable failures to
the existing collector. In a checked setup/configuration scope, a short failure also
stops subsequent setup through the existing policy. A published Ref keeps its original
NodeId when an ID changes; removal and ID reuse never retarget it. Operations reject
foreign threads before reading mutable scene state. A successfully published observation
returns CLOSED after cleanup; a removed node returns STALE_NODE. Failed lookup slots
retain their original error for inspection. Capture refs by value in callbacks.

### Style one reference or read a file

`ui::style(add, "* { bold: true; }")` applies complete strict PSS v0 rules to
exactly the referenced node. It does not include its children. Existing PSS selectors
still filter that node, with their original specificity. This scope uses NodeId and
survives an ID rename without applying to a replacement node. A foreign scene's Ref
returns WRONG_SESSION; an expired or removed target rejects before style publication.

`ui::styleFile(add, "button.pss")` owns a native path and reads it only when the
scene consumes the stage. `ui::styleFile("application.pss")` is the scene-wide form.
For example, a file containing `* { bold: true; }` can replace the inline stage in
the complete program above. Relative paths use the process working directory at
consumption. Missing files and parse errors stop startup before terminal activity;
diagnostics retain the file operation, source label and PSS position when applicable.

These forms use the existing layer resolver and source metadata. Unnamed stages own
independent APPLICATION layers; reusing one stage updates its layer. `StyleOptions`
selects USER or an explicit named upsert using the same precedence and budgets as
[style layers](style-layers.md). Files have no watcher, includes or new expressions.
[Installed replay consumer](../tests/package_consumer/named_scoped.cpp) verifies
the complete handler/error/cleanup path without opening a native terminal.

## Handle button activation

<a id="pipe-click-handlers"></a>

`ui::onClick(ref, handler)` adds a handler after the controls have been created.
Handlers for the same button run in registration order. A handler can accept
`ui::ClickParams&` to inspect modifiers and stop the remaining handlers.

This complete program shows **Applied** after a normal activation. With an
admitted Shift-modified activation, it shows **Apply with Shift** and skips
the second handler. F2 changes the label; F10 exits.

[Source](examples/docs_events.cpp).

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::Ref<Label> status;
  ui::Ref<Button> apply;

  auto scene = ui::scene(ui::vbox(
    ui::label(status, "Ready").id("status"),
    ui::button(apply, "Apply")));

  return scene
    | ui::onClick(apply,
                  [status](ui::ClickParams &params) {
                    if (params.mods.has(key::SHIFT)) {
                      status.text() = "Apply with Shift";
                      params.stop();
                    }
                  })
    | ui::onClick(apply,
                  [status] {
                    status.text() = "Applied";
                  })
    | ui::onKey("F2",
                [status] {
                  status.text() = "F2";
                })
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

`params.mods.has(key::SHIFT)` reads semantic modifiers.
`params.source` distinguishes keyboard, mouse and programmatic activation;
`params.target` identifies the button. Parameters are borrowed for the callback.
A copied snapshot retains event data, but calling `stop()` on that copy cannot
stop the original activation. See [bindings and events](bindings-guide.md).

Some terminals reserve Shift+mouse for selection and withhold that event.
See [input](input.md) for terminal capabilities and modifier reporting.

## Register shortcuts

<a id="short-key-bindings"></a>

Use `ui::onKey("Ctrl+S", handler)` for a shortcut,
`ui::onKey("F10 | Ctrl+C", ui::quit)` for alternatives, and
`ui::onKey("Ctrl+K, K", handler)` for successive strokes.
The ordinary string shortcut form accepts press events. Use `ui::pressRepeat(...)`
when a command should also run on repeats.
See [shortcut syntax and event selection](input.md) for matching rules.

Pass `ui::quit` directly when the handler only needs to close the current scene.
For work that explicitly refers to another scene, use `SceneRef`;
see the [scene reference](declarative-reference.md).

## Share state between controls

Create a shared state in the scene's recipe factory. The factory receives
`ManagedScene&` and runs during preparation. Its checked result passes state
creation failures to the application runner.

The editor, label and button below share the same string state. Accepted editor
changes update both captions through managed preparation. Click the button or
press F10 to exit.

[Source](examples/docs_state.cpp).

```cpp
#include <portals/ui.hpp>
#include <string>

int main() {
  using namespace portals;

  auto scene = ui::scene([](ManagedScene &owner) {
    return owner.state(std::string("Portals"))
      .transform([](auto model) {
        return ui::vbox(ui::lineEdit(model),
          ui::label(model),
          ui::button(model, [] {
            ui::quit();
          }));
      });
  });

  return scene
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

For custom presentation, attach `.observe(state, callback)` to a control recipe.
The scene owns delivery, starting after all references are published; see
[managed observation](bindings-guide.md#managed-observation) for errors, limits
and the latest-value contract.

Use [`owner.derive(input, transform)`](bindings-guide.md#managed-derivation) when
several captions need one computed owning value. Its read-only ManagedSource is
accepted by the same label, button and observation recipes.

For large datasets, create a model in the same factory and pass its observing
handle to `ui::virtualList`, `ui::virtualTable` or `ui::chart`.
See [models](model.md) and [virtual tables](virtual-table.md).


## Scroll ordinary content

Use `ui::scrollArea` for a retained form and `ui::scrollBar` for a standalone or
shared axis. The [ordinary scroll guide](scroll-controls.md) contains a complete
native application, bar placement, checked commands and host capture requirements.
For datasets, keep using the existing [virtual views](virtual-preparation.md).

## Configure virtual views

<a id="typed-columns-and-named-view-settings"></a>

Use `tableColumn` and `tableColumns` to describe table fields and their
formatting. The [text and column guide](compact-examples.md) covers syntax.
`ui::VirtualTableConfig<T>`, `ui::VirtualListConfig<T>` and `ui::ChartConfig<T>`
provide named view settings. Their contracts are in the
[scene reference](declarative-reference.md).

## Continue with a complete application

- [Gallery](../examples/gallery/fluent_form.hpp): editor, checkbox, layout and theme switching.
- [Custom widget](../examples/custom_widget/fluent_meter.hpp): state, key handlers and preparation.
- [Dashboard](../examples/dashboard/dashboard.hpp): virtual table, chart and asynchronous data loading.
- [Command-line startup](cli.md): parse options before opening the terminal.
- [Scene reference](declarative-reference.md): preparation, ownership, typed results and reporting.

## Fine scene settings

<a id="scene-fine-settings"></a>

<details>
<summary>Set scene limits</summary>

Set `ui::SceneOptions` before creating the scene and pass it as the second
argument to `ui::scene`. The options are copied into the scene and checked
during preparation. There is no API for changing these limits on an existing scene.

| Field in `options.managed` | Meaning | Default |
|---|---|---|
| `maxNodes` | Retained managed node registrations, including removals awaiting preparation | 4096 |
| `maxStates` | Scene-owned properties | 256 |
| `maxModels` | Scene-owned models | 64 |
| `diagnostics.maxErrors` | Retained diagnostic records | 64 |
| `diagnostics.maxContexts` | Causal contexts retained per error | 8 |
| `diagnostics.maxContextBytes` | Total label bytes retained per error | 1024 |

All six limits must be greater than zero. Increase them when your form needs
more managed objects or more diagnostic detail. `maxErrors` limits stored
history; it does not specify how many errors the application tolerates.
On history overflow, the oldest records remain and `droppedErrors` increases.

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::SceneOptions options;
  options.managed.maxNodes = 8192;
  options.managed.maxStates = 512;
  options.managed.maxModels = 128;
  options.managed.diagnostics.maxErrors = 128;
  options.managed.diagnostics.maxContexts = 16;
  options.managed.diagnostics.maxContextBytes = 2048;

  auto scene = ui::scene(ui::label("F10: quit"), options);

  return scene
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

</details>

<details>
<summary>Set handler limits and sequence timeout</summary>

These options are also passed to `ui::scene` before creation. Change them
when a form has many handlers or users need more time between shortcut strokes.

| Field | Meaning | Default |
|---|---|---|
| `maxConfigurations` | Queued configure/key/click operations before preparation | 64 |
| `managed.maxKeyHandlers` | Owning key registrations | 64 |
| `managed.maxClickHandlers` | Pipe click handlers across the scene | 256 |
| `managed.maxClickHandlersPerNode` | Pipe click handlers on one button | 64 |
| `managed.keySequenceTimeout` | Inactivity between shortcut strokes, milliseconds | 1000 |

Each value must be greater than zero. The example allows two seconds between
the strokes of `Ctrl+K, K`. The comma denotes two successive strokes; see
[shortcut syntax](input.md).

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::SceneOptions options;
  options.maxConfigurations = 128;
  options.managed.maxKeyHandlers = 128;
  options.managed.maxClickHandlers = 512;
  options.managed.maxClickHandlersPerNode = 128;
  options.managed.keySequenceTimeout = 2000;

  auto scene = ui::scene(ui::label("Ctrl+K, K or F10: quit"), options);

  return scene
    | ui::onKey("Ctrl + K, K", ui::quit)
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

</details>

## Compose children from a range

`ui::vbox(range)` and `ui::hbox(range)` consume an input range immediately,
in iteration order. The range produces one concrete `Description<T>` type; use
the variadic overload for heterogeneous children. A temporary transform view
and its captures are destroyed after construction, while the parent owns all
produced recipes. The range is never used during mount.

```cpp
#include <portals/ui.hpp>

#include <array>

int main() {
  using namespace portals;

  const std::array<std::string_view, 3> captions{"Mercury",
    "Venus",
    "Earth"};
  auto scene = ui::scene(ui::vbox(
    captions | std::views::transform([](std::string_view text) {
      return ui::label(text);
    })));

  scene.onKey(key::F10, ui::quit);

  return scene.runApp();
}
```
The same scene with pipe stages:

```cpp
#include <portals/ui.hpp>

#include <array>

int main() {
  using namespace portals;

  const std::array<std::string_view, 3> captions{"Mercury",
    "Venus",
    "Earth"};
  auto scene = ui::scene(ui::vbox(
    captions | std::views::transform([](std::string_view text) {
      return ui::label(text);
    })));

  return scene
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
}
```

Both versions use the same scene, error reporting and cleanup. Existing
`row`/`column` overloads also accept ranges; `vbox(options, range)` and
`hbox(options, range)` preserve `LinearOptions`. Empty ranges create empty
layouts. A child mount failure preserves its typed error, rolls back the
created subtree and publishes no partial `NodeRef` or `Ref`.

Lvalue containers of move-only descriptions require explicit transfer:
`ui::vbox(children | std::views::as_rvalue)`. `ui::vbox(std::move(children))`
also transfers an owning non-view container that does not opt into
`enable_borrowed_range`. User-defined ranges must own their elements when using
this implicit-element-transfer overload; that ownership cannot be inferred. A temporary view yielding lvalue
descriptions is rejected, since its elements may belong to another owner.
Construction and iteration may throw; recipes already transferred from a
source are not restored. Borrowed references inside recipes retain their
existing lifetime requirements.

[Frames, gauges and compact sizing](compact-primitives.md) provide standard
widgets and consuming methods/pipe stages for a complete small scene.

## Text-driven lists

Use `ui::virtualList(ui::words(field.draftText()))` or
`ui::virtualList(ui::graphemes(field.draftText()))` for an automatically owned
text-to-list binding. [Complete native application and contracts](bindings-guide.md#live-text-transforms).
