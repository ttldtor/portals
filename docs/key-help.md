# Key hints and help {#key-help}

[Русская версия](ru/key-help.md)

A key command's name accompanies its registration. `KeyHints` and `HelpView` read those
same registrations, so rebinding or disabling a command updates its presentation
without maintaining a separate list of shortcuts.

## A complete application

\include guide_key_help.cpp

[Complete source](examples/guide_key_help.cpp). The example links `portals::core`; see
[package setup](package.md). The [standalone guide project](examples.md) builds
`guide_key_help`. This is a native application without a replay mode. Its Save
command changes a status label; it does not write a file or implement persistence.

F1 opens the help pane, Escape closes it while its scope is active, and F10 exits
through the normal runner. These keys are explicitly chosen by the application;
neither control installs hidden open, close or quit bindings. The buttons disable
Save or change its shortcut. The hints and help reflect both changes.

The help body is registered for style resolution as `HelpView.help-body`.
Its supplied `ControlStyle` provides manual defaults beneath matching theme and
application rules; without a computed style, painting uses its normal manual
default. Styling the outer HelpView wrapper does not automatically style the body.
KeyHints uses the disabled variant when it or an ancestor is disabled, before
choosing focused or normal appearance.

The compact handlers return the checked result from `help.open()`, `help.close()`
and binding changes. Their failures then reach the
existing managed error path. These commands return `std::expected`, not the
automatically collected text-property assignment proxy. Outside a handler,
the caller checks the result explicitly.

## Metadata, ownership and changes

`ui::keyInfo(label)` adds an owning name to an ordinary `ui::onKey` registration.
Optional `.description(text)` supplies the longer help explanation.
`.owner(reference)` ties admission to that control and its ancestors;
`.scope(reference)` restricts admission to an existing focus scope. Neither
modifier adds a new routing priority: the newest matching admitted registration
still wins. A deeper scope does not automatically outrank a newer global binding.

`.reference(binding)` is useful when the application needs to change that binding
later. `ui::KeyBindingRef` observes the scene-owned command; destroying the
observation does not unregister it. Its checked operations are `setEnabled`,
`setVisible`, `setText`, `rebind`, `unbind` and `remove`. Unbinding keeps the identity
and explanation; removal retires it. Generated identities are local to a scene
and are unsuitable as saved configuration keys.

An unpublished or closed observation returns a typed error. Operations are
UI-affine; worker threads cannot mutate a binding. Removed controls are never
recovered by reusing their slot. Names and descriptions accept owning `TextValue`
expressions. Resource text uses an explicit `ui::keyInfo(expression, resolver)`;
resolution occurs during checked metadata normalization, not
on every paint. A read snapshot retains neither the resolver nor the callback.

## What the two controls display

`ui::keyHints(help)` reserves one row. It shows named, currently eligible commands
and fits complete entries to the available width. Its overflow action opens the
supplied help pane through the existing focus and input path. An empty or fully
fitting row does not add an invisible focus stop.

`ui::helpView(help)` starts hidden and contains an ordinary `ScrollArea`. It is
nonmodal: it neither shields the rest of the scene nor traps Tab. Opening enters
its existing focus scope; closing returns focus through the existing return
stack. Hiding, disabling or removing the pane retires that activation. Reopening
creates a new activation; an old return receipt cannot close the new one.

Full help explains unavailable bindings and conditional overlaps. A conflict is
not a promise about which command will run in every focus context. Legacy
unannotated registrations remain routable and inspectable; compact hints require
an explicit command name.

Disabling a named command leaves it visible as unavailable in full help and
removes it from compact hints. Setting its visibility to false also disables its
routing and removes it from projections. Hidden or disabled owners and ancestors
cannot admit the command. Changing only the explanation preserves a pending key
sequence; rebinding or changing admission cancels the old pending snapshot.

## Terminal support, errors and limits

Terminal phase support is explicit `KeyCapabilities` information. `UNKNOWN`
remains routable and is marked with ` [?]`; it does not claim native release,
repeat or left/right modifier support. `UNSUPPORTED` excludes affected annotated
commands. This metadata does not negotiate a terminal protocol.

The default formatter uses ASCII names, ` then ` between sequence strokes,
` / ` between alternatives, and explicit repeat/release markers.
`KeyFormatOptions`, `KeyHintsOptions` and `HelpViewOptions` customize those strings.
Clipping respects grapheme boundaries. A formatted binding is limited to 8192
bytes by default. Scene defaults cap labels at 512 bytes, descriptions at 2048
bytes and aggregate logical projection payload at 1 MiB; existing registration
and shortcut limits still apply.

Registration and changes stage owning data before publication. An invalid or
over-budget candidate leaves the prior binding and routing state intact. If a
fresh presentation cannot be prepared, the visible control shows its unavailable
marker rather than claiming that an old projection is current. The error uses
the existing diagnostics path; unchanged failed input is not retried every frame.
An explicit later change or invalidation permits recovery. A presentation failure
does not undo an independently committed binding change.

Hidden projections do not format rows. Static hints and help do not poll, add a
second timer or continuously redraw. See [input](input.md), [focus](focus.md) and
[managed controls](managed-controls.md) for the underlying retained lifecycle.
