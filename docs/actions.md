# Shared actions {#actions}

[Русская версия](ru/actions.md) · [Basic controls](controls.md)

Use `ui::ActionSet` when one command needs the same callback and presentation in
more than one place, such as a button and an explicitly bound key chord. For a
single button with no shared state, pass its callback directly to `ui::button`.

## A complete application

\include guide_actions.cpp

The [complete source](examples/guide_actions.cpp) links `portals::core`; see
[package setup](package.md). The factory creates both actions before returning a
scene description. Button recipes and key stages retain their declarations until
managed attachment, so the local `ActionSet` may leave scope before the lazy
mount. Once attached, the scene owns the actions for its session and retires
their callbacks when it closes.

The `add` key is an organizational key used to identify one entry in its set.
It is separate from `exclusiveGroup`, which groups mutually exclusive checked
presentation state. The compact `action(key, caption, callback)` overload is
useful for a basic action; use `ui::Action` and `ui::ActionMetadata` when the
action also needs shortcut/help text or initial enabled, visible, checked, or
exclusive-group state. `add(...)` returns a checked `std::expected`; the compact
`action(...)` form carries a typed admission failure to its later binding.
Constructing the callback and its captures happens before admission and may
throw. In this example those constructions happen inside the factory body, so
`ui::runApp` reports `std::bad_alloc` as `NO_MEMORY` and other factory
exceptions as `CALLBACK_FAILED`, then returns 1 after cleanup. Constructing the
factory object or its captured arguments before calling `runApp` is outside
that boundary.

An `ui::ActionRef` is a weak reference to the set entry. The attached scene keeps
the callback alive; closing that scene invalidates refs and releases callbacks,
even if the original `ActionSet` object or cold recipes still exist. The same
ref can be used by several buttons and by `ui::onKey`. Binding a set already
attached to another scene reports `WRONG_SESSION`.

Caption, enabled, visible, and checked state are shared across all button
presenters and update from one metadata revision. Disabled or hidden actions do
not enter their callback. `checked` is presentation state and does not toggle
automatically when invoked. A nonempty `exclusiveGroup` allows at most one
checked action with that group name in the set; the organizational keys do not
define groups.

The `shortcut` field is presentation text only. It does not register input or
install a global shortcut, and this API does not automatically add actions to
`KeyHints`. Bind each chord through the existing `ui::onKey` API so normal
scope, modal, and key-matching rules continue to apply. The example does this
explicitly for Ctrl+H, F10, and Ctrl+C.

Each set has positive admission limits: 256 actions, 1024 button presenters,
65536 bytes for live keys and current metadata, and 256 bytes per key by
default. Configure them with `ui::ActionSetOptions`. The metadata-byte limit
covers live set data; caller-held snapshots and a callback already in progress
can keep older data alive independently. Admission failures use typed errors
and do not publish a partial entry. Action mutations and invocation failures
use `EditorCommandResult`; its `result()` exposes the checked result when the
caller needs to handle it directly. Managed callbacks return their existing
checked result, which the scene's collector records once.

Actions are code-bound declarations. Loaded scene resources do not define
executable actions.
