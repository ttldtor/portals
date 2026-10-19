# References, bindings and events {#bindings-guide}

[Русская версия](ru/bindings-guide.md)

An explicit event handler and an observing control reference let you connect
a user action to a change in another control. Managed state can hold a shared
UI-owned value for several controls. A [`ui::Ref<T>`](glossary.md) identifies a control without keeping it alive.
Copying the reference does not subscribe to changes.
Explicit shared text state is supported by editors, labels and buttons.
Managed observation delivers the latest state values.
Read-only derived managed sources share the same observation and caption paths.

## Text changes from a button

This complete program uses the current [scene](glossary.md#glossary-scene)'s exit
command from a key callback invoked by that scene. The scoped `ui::quit()`
exits from the handler.

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
throw. The factory runner places construction inside that checked boundary.

## Text assignment

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

Retaining the operation result lets you handle errors explicitly:

```cpp
auto update = field.text().set("replacement");
auto accepted = update.result();
auto snapshot = field.text().get();
```

Both `accepted` and `snapshot` require a success check before their values can be read. `get()` returns owning bytes
and does not report errors automatically. Mutation preserves typed errors and
reports through a live scene collector where applicable. A rejected text candidate
preserves the previous text. `+=` appends to the complete draft/caption rather
than replacing the editor selection. The receiving control still validates text
against its width policy and limits.

`ui::quit()` inside a managed callback resolves that entered scene; it is a safe
no-op without a current live scene. Outside those bridges, you can name an
explicit owner with `ui::quit(scene.ref())` or the checked `SceneRef::quit()`. Neither reference
keeps its scene alive. See [the scoped context contract](declarative-ui.md).

## Shared managed values

The scene factory can create an optional shared string state. The field publishes
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

## Observing a managed value {#managed-observation}

`.observe(state, callback)` provides observation when a control needs to present a managed value
through application code. The scene owns the subscription and callback. For
ordinary shared text captions, `ui::label(model)` and `ui::button(model)` remain
the direct forms.

[Complete program](examples/docs_observe.cpp).

The editor publishes accepted strings; the label formats the latest accepted value.
This complete program lets you edit text and watch the label update.
F10 and the Quit button close the application:

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
snapshot. Notification retries must not repeat `set()`: the value has already
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

## Deriving a read-only value {#managed-derivation}

`owner.derive(input, transform)` computes an owning value from one managed
state or earlier derived source. The returned `ManagedSource<T>` exposes owning
immutable `snapshot()` values and has no `set()` or conversion to writable state.
Labels, buttons and managed `.observe` accept it directly. Editors and checkboxes
still require writable state for their two-way bindings.

[Complete program](examples/docs_derive.cpp): edit the name to update both the label
and button caption. Click the button or press F10 to close the scene. Build
`portals_docs_derive` with examples enabled. Setup failures flow through the scene
factory's `expected` result to the existing runner.

\include{lineno} docs_derive.cpp

The returned `expected<ManagedSource<Out>, ManagedError>` requires a success check before use.
Initial computation runs synchronously, so a successful source is immediately
readable during mounting. Initial failure publishes neither output nor subscription
and can be handled without stopping a later valid run. A transform takes `const In&`
and returns an owning, equality-comparable Out or `expected<Out, TreeError>`;
Out need not have a default constructor. References and known borrowed result
forms such as string_view, span, pointers and range views are rejected. Custom
types must still satisfy [Property's owning-value contract](property.md).

The transform must be a pure synchronous value computation. It must not mutate
the UI, enter lifecycle operations, or depend on unpublished sibling references.
One owned callable instance serves initial and later computations. Its input
snapshot remains alive throughout invocation. A changed declared input during
initial computation rejects admission with `WOULD_BLOCK`; arbitrary client side
effects cannot be rolled back.

Changed inputs use the existing Scheduler and bounded dirty recovery. Equal
transformed values advance the processed input revision without publishing an
output revision. A committed output whose notification was rejected is recovered
from its snapshot; repeating `set` is unnecessary. Pure chains settle before
ordinary caption synchronization in the same dirty frame when the input changed
before preparation. Later mutations from user `onPrepare` callbacks have no such
same-frame guarantee.

Runtime errors preserve the last good output and unchanged output revision;
the input remains committed. Returned TreeError values stay exact, bad_alloc maps
to `NO_MEMORY`, and other transform exceptions map to `CALLBACK_FAILED`.
The runner and bounded scene diagnostics carry the error and derivation call site;
snapshot still returns the last good value. Existing stop policy applies, with
retry only through an explicit later preparation. Close retires computation;
final snapshots remain readable until scene destruction. Retained immutable
snapshots can outlive the owner.

Each derive consumes one existing `maxStates` slot and one root observer slot.
Defaults are 256 states, 256 scene observers, 64 observers per node, 64 Property
endpoints and 16 recovery passes; the remaining intersection of those limits
determines capacity. `derive` accepts the existing optional output invalidation
(PAINT by default, or LAYOUT) and PropertyOptions; NONE is rejected.

Every output is fresh and read-only, so declared dependency cycles cannot be
constructed. Arbitrary captured side effects are not tracked; bounded
nonconvergence returns `WOULD_BLOCK` and does not diagnose a hidden graph cycle.
`Ref::draftText()`, `captionText()` and `committedText()` are snapshot-only text
sources and are not reactive derive inputs. Live text and derived-list/model
adapters remain separate planned work.

## Event interfaces

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
command receipt. Returning an editor undo/redo receipt directly preserves its
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

## Keyboard phases

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

## Observing layout changes

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

## Transforming live editor text {#live-text-transforms}

This complete application shows the current editor draft as two virtual lists.
Edit the field; words and graphemes update through the existing dirty-frame
preparation. Click Quit or press F10 to exit. The standard runner handles typed
setup/runtime failures and terminal restoration.

[Complete source](examples/guide_text_transforms.cpp). Include `<portals/ui.hpp>`
and link `portals::core`; see [consumer setup](index.md). To build this source
inside the repository, configure with `PORTALS_BUILD_TESTS=ON`, then build and run
`portals_docs_text_transforms`. It opens the native terminal and has no CLI options.

```cpp
// Copyright (c) 2026 ttldtor.
// SPDX-License-Identifier: BSL-1.0
#include <portals/ui.hpp>

/** @brief Show whitespace-delimited words and extended graphemes from the live editor draft. */
int main() {
  using namespace portals;

  ui::Ref<LineEdit> input;
  auto scene = ui::scene(ui::vbox(ui::lineEdit(input, "Hello, Portals!"), ui::label("Words"),
                                  ui::virtualList(ui::words(input.draftText())).height(4),
                                  ui::label("Graphemes (including spaces)"),
                                  ui::virtualList(ui::graphemes(input.draftText())).height(4), ui::button("Quit", [] {
                                    ui::quit();
                                  })));

  // clang-format off
  return scene
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
  // clang-format on
}
```

`ui::words(source)` and `ui::graphemes(source)` are cold descriptions: construction
neither reads text nor subscribes. Mount resolves direct references from the whole
recipe, including later siblings, and initializes the private graph before
publishing references. An initial failure rolls back the attempted mount. The
scene owns synchronization and cleanup; application code needs no timer,
subscription token or manual revision counter.

A clean editor bound to an externally updated string state synchronizes before
the live transforms and list models are prepared for that dirty frame. Both
lists therefore pin the new text in that frame. Equal ordered token values do
not publish another collection-model revision. An editor with an unconfirmed
draft still follows its configured edit-mode conflict policy.

`words` separates tokens only at whole graphemes whose scalars all have Unicode
`White_Space`. Punctuation, apostrophes, CJK and emoji remain in their tokens.
This is whitespace tokenization, not linguistic segmentation or Unicode UAX #29
word breaking. `graphemes` returns every existing extended grapheme, including
whitespace. Both retain owning input storage; `TextSegment::bytes()` is a view
valid while that segment is retained and unchanged.

`TextSplitOptions` defaults to 4096 tokens and `TextLimits{}`: 1 MiB input,
64 KiB per cluster, 1,048,576 graphemes and 2,097,152 cells. `maxTokens` must be
positive. Limits reject the complete candidate rather than truncate it. Customize
an individual recipe with `ui::words(input.draftText(), {.maxTokens = 1000})`.
The editor's own validation, text limits, width policy and confirmation mode
still apply; a draft source includes intermediate text without publishing it to
the editor model.

Token IDs conservatively use reset-on-change (`RESET_ON_CHANGE`) semantics:
any changed ordered token sequence receives a new generation. The same ordered
token bytes and width policy are a no-op even when delimiter offsets change.
Undo can therefore create fresh IDs. Only newly collection-bound lists use
clear-missing (`CLEAR_MISSING`) selection behavior, checked against the complete
ID index; ordinary provider lists keep their existing policy.

Transform and collection-model application are separate commits. Runtime
failure preserves the last good output/model and owning model pins; it does not
undo an already accepted editor or source value. Failures retain typed causes
and the existing runner stop policy. After origin removal, delivery quietly
retires and the list retains its last good owning collection for local scrolling
and resize; old prepared callbacks retire too. Removal of the receiving list
reclaims its private graph. Explicit stale source access still reports
`STALE_NODE`; scene closure reports `CLOSED`.

### Explicit sources and collections

For an existing `ManagedScene`, pass the complete subtree recipe to
`ui::mount(owner, recipe)` and check its `std::expected<Node<T>, ManagedError>`
result. This uses the same cold text-to-list bridge as the native example.
`ui::mount` mounts beneath the scene root; it has no public parent-node overload.
A factory callback passed to `ui::scene` must return an owning description,
optionally wrapped in `std::expected<Description<T>, ManagedError>`; `ui::scene`
itself returns `Scene`. The callback cannot return an already-mounted node or
a `void` setup result. Description constructor callbacks are internal.
The complete recipe avoids nesting an unsupported mount
factory or rebuilding the automatic token-ID adapter in application code.

Individual `ManagedScene::emplace` calls retain earlier successful children when
a later call fails; closing the scene cleans that partial form. A complete
`ui::mount` call instead rolls back its attempted subtree. These are different
transaction boundaries, even when successful layouts are the same.

For a mounted reference, `owner.liveText(input.draftText())` returns a checked
`ManagedSource<GraphemeText>` immediately initialized from the source. Its
`snapshot()` is an owning property-change snapshot. `owner.derive` and
`owner.observe` provide explicit transformations and presentation. The finite
`portals::words(text)` and `portals::graphemes(text)` return
`std::expected<TextSegments, TreeError>` and need no scene.

For domain rows with application-owned stable IDs, create an owning
`RowCollection<T>`, expose it through a read-only managed derived source, then
call `owner.model(source)`. This sink-only model has no writable publication
endpoint (`publisher()` returns `WOULD_BLOCK`). `CollectionModelOptions::maxRows`
and the existing model budgets bound admission. A failed source change leaves
the old source; an accepted source followed by failed model staging leaves the
new source and last good model. Equal source assignment does not resend a failed
notification; a later permitted preparation can retry the latest value.

All live registration, snapshots and mutation use the owning UI thread between
frames. Handles weakly observe their original scene/control identity; copies do
not prolong it or follow a reused node slot. Retained successful text snapshots,
segments, collections and model pins own their data independently. Callback
captures by reference still borrow their objects. See [managed
observation](#managed-observation), [text sources](text-property.md) and
[virtual lists](virtual-list.md) for the underlying contracts.
