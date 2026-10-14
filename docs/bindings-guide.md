# References, bindings and events {#bindings-guide}

Start with an explicit event handler and an observing control reference. Use a
managed state when several controls need the same UI-owned value. A [`ui::Ref<T>`](glossary.md) identifies a control without keeping it alive.
Copying the reference does not subscribe to changes.
Automatic and derived bindings are **TBD — M2**.

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
    | ui::onClick(append, [field] { field.text() += "!"; })
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

A scene factory receives `ManagedScene&` and can create a scene-owned state.
This factory fragment shares one text model between two editors and returns
either its recipe or the original typed setup failure:

```cpp
auto makeEditors = [](ManagedScene &owner)
  -> std::expected<ui::Description<Column>, ManagedError> {
  auto text = owner.state<std::string>("shared");
  if (!text) {
    return std::unexpected(text.error());
  }
  return ui::vbox(ui::lineEdit(*text), ui::lineEdit(*text));
};
auto scene = ui::scene(makeEditors);
```

`ManagedState<T>` weakly observes a scene-owned `Property<T>`. `set()` changes
that model; `snapshot()` returns its owning immutable value and revision. Check
the returned update receipt: a value can commit successfully while some bounded
notification deliveries are rejected. Equal assignments are no-ops and do not
retry rejected delivery. Editor confirmation and validation determine when a
draft publishes; see [managed controls](managed-controls.md) and [editor](line-edit.md).

Managed handles belong to their scene and UI thread. They are not worker-side
mutation endpoints. Use worker-safe model publishers or scheduler endpoints
for cross-thread delivery; see [models](model.md) and [scheduler](scheduler.md).

## Choose the event interface

The following interfaces serve different registration paths:

| Registration | Callback input | Role |
| --- | --- | --- |
| `ui::onClick(buttonRef, callback)` | No arguments or `ui::ClickParams&` | Ordered scene-owned pipe handlers |
| Recipe `.onClick(callback)` / `Ref<Button>::onClick(callback)` | No arguments or `const ClickEvent&` | Existing managed activation adapter |
| `ui::onKey(selection, callback)` | No arguments or `const KeyEvent&` | Raw keyboard interception before semantic routing |
| `Property<T>::observe(receiver, callback)` | `W&`, `shared_ptr<const PropertyChange<T>>` | Scheduled value notification to a checked receiver |
| `ManagedScene::onEvent(node, callback)` | Existing custom event contract | Explicit custom-widget routing |

Click/key adapters accept `void`, `expected<void, TreeError>` or the exact editor
command receipt. Return an editor undo/redo receipt directly to preserve its
diagnostic provenance. `Property::observe` returns a caller-owned subscription;
managed scene registrations retain their own tokens. Do not discard a low-level
subscription and expect it to remain connected.

For a more involved click handler:

```cpp
scene | ui::onClick(append, [field](ui::ClickParams &params) {
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
scene | ui::onKey("W", move);
scene | ui::onKey(ui::repeat("W"), move);
scene | ui::onKey(ui::release("W"), stop);
scene | ui::onKey(ui::pressRepeat("W"), move);
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

**TBD — M2:** automatic/derived binding facilities. The future expression
syntax, dependency graph and cycle policy are not presented as working features.
Typed live theme parameters are already available through explicit UI updates;
see [themes](themes-guide.md). Their existence does not imply automatic domain
or string-PSS variable binding.

## Planned scene parameters, async and geometry

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

**TBD — M1:** `onLayout` and checked arranged-geometry reads through Ref.
Notification refers to completed layout and is delivered outside tree traversal;
callback mutations request the next frame. Before valid layout, a getter must
not fabricate a zero size. Exact getter syntax remains undecided.
