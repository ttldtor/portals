# Declarative composition {#declarative-ui}

`<portals/ui.hpp>` supplies owning, single-use descriptions in `portals::ui`.
They compose ordinary controls and custom widgets on the existing
[managed lifecycle](managed-controls.md). A description is a construction value;
it is not another widget tree or a reactive runtime. Mounted objects remain
owned by `WidgetTree`, while `ManagedScene` retains bindings and presentation work.

## Store descriptions, then mount

A description owns its child descriptions and callback objects. Standard text
factories copy their input immediately, so a description can outlive the source
`std::string` or `std::string_view`. Creating a description does not mount widgets
or subscribe to events. Descriptions are movable and noncopyable; mounting
consumes them. Reusing a consumed or moved-from description returns
`TreeError::INVALID_ARGUMENT` through `ManagedError`.

Use `ui::column(...)` and `ui::row(...)` for nested composition. Their overloads
accept `LinearOptions` before the children when gaps, padding or alignment need
customization. Standard factories are `label`, `button`, `checkbox` and
`lineEdit`. Binding state belongs to the same `ManagedScene` as before.
`custom<T>(args...)` constructs the concrete public `Widget` subtype, including
`final` classes. It does not introduce another base class.

Factories and fluent modifiers construct ordinary C++ values: allocation or
user-defined argument-copy exceptions can escape while assembling a description.
The `ui::mount(scene, std::move(description))` boundary returns typed managed
errors for mount, binding and initialization failures. A failed attempt removes
its new subtree and releases its managed registration budget. Existing siblings
and states remain intact. External side effects of user callbacks are not rolled
back. The original cause and an independent cleanup error remain distinguishable.

`ManagedScene::remove` retires identities immediately. Managed mutations reentering
that call return `WOULD_BLOCK`. If the caller holds an outer `tree.edit` or
`withScope` borrow, physical destruction and `unmount` wait for that borrow to
finish. Those deferred hooks follow the existing tree contract and can remove
other nodes; their external effects are not undone by description rollback.

## Observe without owning the widget

`ui::NodeRef<T>` is a copyable observing slot. Attach it with `.reference(ref)`
and obtain the existing checked `Node<T>` through `ref.node()`. References are
published after the entire description mounts successfully; before that they
are empty. Reusing a populated slot or binding it twice in one description is
rejected. Removing the mounted widget leaves a stale checked identity, not a
live owning pointer. `Application`, `ManagedScene` and mutable widget operations
keep their UI-thread contracts.

## Explicit customization

Standard controls inherit their normal managed bindings, styles and LineEdit
semantics. A business result callback runs after standard behavior; it cannot
undo a committed edit or checkbox toggle. The existing LineEdit validator remains
the commit veto. No manual token storage or standard presentation sync is needed.

`ui::LineEditConfig` uses named fields for the editor result callback, editor
options, sizing, clipboard and validation. For example, `ui::lineEdit(name,
{.validate = nonempty})` does not require positional placeholder arguments.
Invalid factory text retains its original `TextError` in `ManagedError::text`;
construction conveniences do not discard diagnostic kind or byte offset.

Fluent `.style(target)`, `.onEvent(handler)` and `.onPrepare(handler)` attach
custom identity and behavior through the same managed APIs. `.setup(callback)`
is the explicit escape hatch for low-level initialization. Its callback receives
the scene and typed node and returns `expected<void, ManagedError>`. Existing
router, layout and tree operations remain available through the caller's
`Application`; the composition layer does not hide them behind a new runtime.

Custom constructor arguments and lambda captures still obey C++ ownership rules.
Owning a callback does not make objects captured by reference live longer. Explicit
borrowed pointers or reference wrappers must remain valid for the uses made by
the custom widget. Consult the `custom` factory reference for argument storage
and forwarding requirements; arbitrary user types are not deep-copied magically.

## Complete installed consumer

The program below uses only the installed `portals::core` package. It retains the
same custom final `Meter`, domain behavior, palette and deterministic input as
the complete before/managed examples. Its full captured terminal output is
compared with the existing managed form. The shared fixture supplies domain
rendering and assertions; it contains no description mounting or binding setup.

\include{lineno} form_declarative.cpp

The executable is a finite, headless replay consumer, with no interactive window.
From its build output directory, run `./portals_form_declarative` to execute the
normal scenario and write its outcome summary and captured terminal bytes to
standard output. Run `./portals_form_declarative --failure` to exercise the
intentional `INVALID_GEOMETRY` callback error and verify its phase, node and
terminal cleanup. That verification returns success when the expected error is
preserved. These are the only supported invocation forms; Windows builds use
the same executable name with the `.exe` suffix.

The shared `form_support.hpp` and `form_replay.hpp` are included in full on the
[managed controls page](managed-controls.md).

### What became simpler

All three complete programs run the same replay, including validation, conflicting
edits, rejected notifications, hide/disable/remove, theme changes and cleanup.
The installed-package test compares both their outcome summaries and terminal
byte streams. The declarative program expresses the hierarchy in one `column`
and mounts it once; standard binding setup stays inside the control factories.

| Complete source | Lines | Nonempty lines |
| --- | ---: | ---: |
| Low-level `form_before.cpp` | 256 | 193 |
| Managed `form_after.cpp` | 218 | 160 |
| Declarative `form_declarative.cpp` | 236 | 165 |

These counts include the failure mode, business callbacks and replay assertions.
The declarative form needs six observing slots so the unchanged test can inspect
every control. It makes the hierarchy explicit and mount failure cleanup local,
but does not reduce the complete program's line count relative to the managed
version. Custom event and presentation logic remain explicit C++.

This stage does not implement reconciliation, dynamic tree replacement,
virtual-view lifecycle, a new CSS cascade or SQLite integration. Descriptions
assemble a tree once. The [performance experiment](managed-performance.md)
compares retained lifecycle costs at several sizes; it does not measure
compilation performance. Shorter construction code is not a speed claim.

The interactive [gallery and custom-widget examples](examples.md) now use these
descriptions. The gallery preserves its former terminal output under the original
themes; additional palettes use the same style engine.
