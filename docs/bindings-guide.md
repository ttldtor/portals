# References, bindings and events {#bindings-guide}

[Русская версия](ru/bindings-guide.md)

Start with an explicit event handler and an observing control reference. Use a
managed state when several controls need the same UI-owned value. A [`ui::Ref<T>`](glossary.md) identifies a control without keeping it alive.
Copying the reference does not subscribe to changes.
Explicit shared text state is supported by editors, labels and buttons.
Managed observation delivers the latest state values.
Derived bindings remain **TBD — M2**.

## Change text from a button

This complete program uses the current [scene](glossary.md#glossary-scene)'s exit
command from a key callback invoked by that scene. Use scoped `ui::quit()` to exit from the handler.

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;
  ui::Ref<LineEdit> field;
  ui::Ref<Button> append;
  auto scene = ui::scene(ui::vbox(
    ui::lineEdit(field, "initial"),
    ui::button(append, "Append")));

  return scene
    | ui::onClick(append,
                  [field] {
                    field.text() += "!";
                  })
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

The scene mounts the complete [recipe](glossary.md#glossary-recipe) before publishing its references and
registering pipe handlers. Capturing a `Ref` by value does not keep its widget or
scene alive. Recipe construction occurs before final `runApp` entry and can
throw; use the factory runner when construction needs that checked boundary.

## Understand text assignment

These are UI callback fragments for successfully mounted references:

```cpp
field.text() = "replacement";
field.text() += "suffix";
destination.text() = field.text();
```

The last line copies the current text once. Subsequent source changes do not
update the destination. `TextProperty` accesses text owned by a control.
[`Property<T>`](glossary.md) stores a value and sends change notifications.
These types have separate ownership and notification contracts.

Use a retained operation result for explicit error handling:

```cpp
auto update = field.text().set("replacement");
auto accepted = update.result();
auto snapshot = field.text().get();
```

Check `accepted` and `snapshot` before reading them. `get()` returns owning bytes
and does not report errors automatically. Mutation preserves typed errors and
reports through a live scene collector where applicable. A rejected text candidate
preserves the previous text. `+=` appends to the complete draft/caption rather
than replacing the editor selection. The receiving control still validates text
against its width policy and limits.

`ui::quit()` inside a managed callback resolves that entered scene; it is a safe
no-op without a current live scene. Use `ui::quit(scene.ref())` or the checked
`SceneRef::quit()` for an explicit owner outside those bridges. Neither reference
keeps its scene alive. See [the scoped context contract](declarative-ui.md).

## Share a managed value

Create an optional shared string state in the scene factory. The field publishes
according to its edit mode; the label and button show the model value. The button
caption changes with that value and its action still exits the application.
State creation failures reach the standard runner through the checked factory.

[Complete source](examples/docs_state.cpp).

```cpp
#include <portals/ui.hpp>
#include <string>

int main() {
  using namespace portals;

  auto scene = ui::scene([](ManagedScene &owner) {
    return owner.state(std::string("Portals")).transform([](auto model) {
      return ui::vbox(
        ui::lineEdit(model),
        ui::label(model),
        ui::button(model, [] { ui::quit(); }));
    });
  });

  return scene
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

`ManagedState<T>` weakly observes the scene-owned `Property<T>`. It cannot be
attached to another scene. Its operations run on the owning UI thread; use model
publishers or scheduler endpoints for worker delivery. `snapshot()` owns its value;
`set()` returns the original commit/delivery receipt. A committed value can survive
rejected observer delivery. Setting an equal value neither creates a revision nor
resends a rejected notification.

Captions consume the latest changed revision during the existing dirty-frame
preparation. They do not create a timer or require presentation synchronization
from application code. A direct `caption.text() = ...` changes only that caption:
it leaves the binding and model intact until a new model revision replaces the
caption. A failed text conversion preserves the previous caption and typed error;
an explicit later preparation can retry the latest model value. Failed preparation
keeps the existing runner error/stop policy; it does not promise automatic recovery
of an active run.

An invalid intermediate editor draft remains visible without changing the shared
caption. `ON_COMPLETION` and `EXPLICIT` defer model publication by their existing
rules. [Editor modes](line-edit.md) and [history](editor-history.md) still apply.

## Observe a managed value {#managed-observation}

Use `.observe(state, callback)` when a control needs to present a managed value
through application code. The scene owns the subscription and callback. For
ordinary shared text captions, `ui::label(model)` and `ui::button(model)` remain
the direct forms.

[Complete program](examples/docs_observe.cpp).

The editor publishes accepted strings; the label formats the latest accepted value.
Run this complete program, edit the text, and press F10 or click Quit to close it:

```cpp
#include <portals/ui.hpp>

#include <string>

int main() {
  using namespace portals;

  auto scene = ui::scene([](ManagedScene &owner) {
    return owner.state(std::string("Portals")).transform([](auto model) {
      ui::Ref<Label> summary;

      return ui::vbox(
        ui::lineEdit(model),
        ui::label(summary, "").observe(model, [summary](const std::string &value) {
          summary.text() = text("Accepted: {}", value);
        }),
        ui::button("Quit", [] { ui::quit(); }));
    });
  });

  return scene
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

The initial callback runs during the first permitted preparation after the whole
recipe has mounted and all `Ref` objects have been published. Registration and
description setup do not invoke it. Captured sibling references may appear before
or after the observing control in the recipe. A failure in this initial callback
is a `PREPARE` failure handled by the runner; it does not undo a successful mount.
For a scene with managed observers, preparation or run/session entry during an
unfinished mount returns `WOULD_BLOCK`, including nested mounts.

For explicit registration, `owner.observe(receiver, state, callback)` scopes
delivery to a managed node; `owner.observe(state, callback)` uses the scene root.
Both return `std::expected<void, ManagedError>`. Failed registration leaves no
managed attachment or subscription. The receiver and state must belong
to that scene, and operations run on its UI thread. A copyable callback takes `const T&`
and returns `void` or `std::expected<void, TreeError>`. Its value is backed by an
immutable owning snapshot for the invocation; copy data that must outlive it.

Each registration tracks its own last successful value revision. Equal assignment
does not deliver again. Updates use the existing scheduler and may coalesce to the
latest state; this API is intended for presentation, not a lossless event log.
When delivery is rejected, existing dirty preparation retries from the latest
snapshot. Do not repeat `set()` to retry notification: the value has already
committed. Callback failure preserves the original receiver, phase and typed
error; a failed callback does not advance its successful revision or roll back
the source. Short `Ref` mutation failures inside a void callback still reach the
scene diagnostic collector and the existing runner stop policy.

`ManagedOptions::maxObservers` defaults to 256 per scene and
`maxObserversPerNode` to 64. Existing property and scheduler limits also apply.
`maxObserverRecoveryPasses` defaults to 16 and must be positive. If callbacks keep
changing earlier observed values and recovery cannot converge within that limit,
preparation returns `WOULD_BLOCK`; no timer or idle retry is installed. A later
explicit preparation may retry after the cause is resolved.

Removing the receiver or closing the scene retires delivery. A retained callback
never follows a replacement node that reuses its slot. Captures by C++ reference
still borrow their objects. Removing the receiver from its callback is supported;
destroying the active scene from a callback is prohibited. Registration and nested
preparation, run or close during recovery are rejected by the existing checked
boundaries.

## Choose the event interface

The following interfaces serve different registration paths:

| Registration | Callback input | Role |
| --- | --- | --- |
| `ui::onClick(buttonRef, callback)` | No arguments or `ui::ClickParams&` | Ordered scene-owned pipe handlers |
| Recipe `.onClick(callback)` / `Ref<Button>::onClick(callback)` | No arguments or `const ClickEvent&` | Existing managed activation adapter |
| `ui::onKey(selection, callback)` | No arguments or `const KeyEvent&` | Raw keyboard interception before semantic routing |
| `ui::onLayout(ref, callback)` / `ref.onLayout(callback)` | No arguments or `const NodeGeometry&` | Scene-owned notification after final layout |
| Recipe `.observe(state, callback)` / `ManagedScene::observe(receiver, state, callback)` | `const T&` | Scene-owned presentation of the latest managed state |
| `Property<T>::observe(receiver, callback)` | `W&`, `shared_ptr<const PropertyChange<T>>` | Scheduled value notification to a checked receiver |
| `ManagedScene::onEvent(node, callback)` | Existing custom event contract | Explicit custom-widget routing |

Click/key adapters accept `void`, `expected<T, TreeError>` or the exact editor
command receipt. Return an editor undo/redo receipt directly to preserve its
diagnostic provenance. `Property::observe` returns a caller-owned subscription;
managed scene registrations retain their own tokens. Do not discard a low-level
subscription and expect it to remain connected.
A successful expected payload is ignored and destroyed before leaving the handler.
Even `false` means handled; inspect success-side receipts before returning.

For a more involved click handler:

```cpp
scene
  | ui::onClick(append, [field](ui::ClickParams &params) {
    field.text() += params.mods.has(key::SHIFT) ? "HELLO!" : "Hello!";
    params.stop();
  });
```

`ClickParams` exposes read-only `target`, `mods`, `source` and owning original
`event` metadata. `stop()` stops later handlers of the same synchronous
activation. It does not introduce DOM bubbling. The callback borrows the context;
a copied context owns its event snapshot, but stopping that copy cannot stop the
original chain. Semantic modifiers do not identify physical left/right keys.

## Select keyboard phases

These fragments assume existing callbacks `move` and `stop`:

```cpp
scene
  | ui::onKey("W", move);
scene
  | ui::onKey(ui::repeat("W"), move);
scene
  | ui::onKey(ui::release("W"), stop);
scene
  | ui::onKey(ui::pressRepeat("W"), move);
```

Plain shortcut strings select PRESS; explicit helpers select actual phases.
The raw `KeyEvent` registration overload retains PRESS+REPEAT behavior. The VT
byte backend supplies PRESS only and does not synthesize RELEASE. Newest matching
key registration wins. Click pipe handlers instead run in registration order.
`"Ctrl+K+K"` means Ctrl+K then Ctrl+K; `"Ctrl+K,K"` has an unmodified second K.
See [input](input.md) for alternatives, sequence timeout, priorities and limits.

## Lifetime, errors and complete examples

Refs publish after mount and become invalid when their targets retire or the
scene closes. Copying them extends no owner lifetime. Operate on UI and outside
forbidden frame/reentry boundaries. Arbitrary C++ reference captures still
borrow their targets; a scene move cannot repair a captured `&scene`.

For complete programs and failure paths, read
[text-property consumer](../tests/package_consumer/text_property.cpp) and
[shortcut-sequence consumer](../tests/package_consumer/shortcut_sequences.cpp).
The [property reference](property.md) specifies ownership, backpressure and
cancellation; [declarative composition](declarative-ui.md) specifies runner reporting.

**TBD — M2:** derived binding facilities. The future expression
syntax, dependency graph and cycle policy are not presented as working features.
Typed live theme parameters are already available through explicit UI updates;
see [themes](themes-guide.md). Their existence does not imply automatic domain
or string-PSS variable binding.

## Observe layout changes

`ui::onLayout(ref, callback)` reports the initial final geometry and later
changes after the tree frame has ended. Callback mutations request the next
frame. `ref.geometry()` and `ref.size()` return checked copies of the final
bounds or local size. Before final layout, while hidden, or after layout
invalidation, they return `TreeError::INVALID_GEOMETRY`. See
[layout observation](layout.md) for an example and delivery rules.

## Planned scene parameters and async

**TBD — M5:** `$name` will refer to a code-assigned value or a registered
notification source. `theme.*` refers to theme parameters and `this.*` to control
properties. Capturing an ordinary C++ `int` does not create an observed source.
Assignment is checked by default; the exact return signature of compact
`ui::currentScene.setParam` still needs API design. Rejecting a candidate preserves
previous parameters and styles. Changes already made to an external model remain.
See [themes](themes-guide.md).

**TBD — M6:** `ui::async` for functions and coroutine lambdas with `co_await`.
Existing UiAsync/UiTask can await supported timers/replies, but ordinary
`onClick`/`onKey` does not automatically accept arbitrary coroutine handlers.
The adapter must own the lambda captures and coroutine frame, establish scene
context at each UI resume and remove it before suspension. It does not move
CPU work to a worker automatically. UI close cancels the continuation; the
application still owns producer stop/join. See [current coroutines](ui-coroutines.md).
The planned milestone is M6.
