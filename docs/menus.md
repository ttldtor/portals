# Menus of shared actions {#menus}

[Русская версия](ru/menus.md) · [Shared actions](actions.md) · [Runnable examples](examples.md)

A menu lets the user choose an application command with the arrow keys or the
mouse. Pass the same action to a menu, a button and an explicit shortcut to share
their callback, caption and availability.

![Command desk with the Save command selected and the saved note visible](images/commands.png)

[Watch keyboard selection and command execution](images/commands.gif).

## A complete application

\include guide_menu.cpp

The [complete source](examples/guide_menu.cpp) links `portals::core`; see
[package setup](package.md). Tab moves focus to the menu. Up and Down move the
selection; Enter runs its action. The Save button and Ctrl+S run the same action.
F10 exits through the normal application cleanup.

The menu is an ordinary retained control in the scene. Its frame and width use
the same [layout modifiers](layout.md) as other controls. It starts visible and
does not open a popup. A menu bar, nested menus and a searchable command palette
are outside this API.

## Share state and presentation

Use `ui::ActionMetadata` when a command needs a shortcut caption, help text or
initial enabled, visible or checked state. The [Command desk example](../examples/commands/main.cpp),
built as `portals_commands_example`, shows an editor, a framed menu and a Save
button. The saved label changes only when the shared Save command runs.

The `shortcut` field is display text. Bind keys explicitly with `ui::onKey`, as
the example does. Updating an action updates its menu and button presenters
through the existing scene preparation. A checked action remains checked until
the application changes its metadata; selection does not toggle it.

Hidden commands occupy no row. Disabled commands remain visible and cannot run.
Keyboard navigation skips unavailable commands. A menu with no eligible action
has no active selection. Changing focus preserves its local selection.
Home and End select the first and last eligible command. Enter and Space activate
on a reported press; explicit repeat and release events do not activate. Up and
Down may repeat. Keyboard and mouse activation preserve the original modifiers.
Legacy terminal input may not distinguish repeated keys from separate presses;
each decoded activation is accepted in that case. The menu does not infer a held
key or add a debounce delay.

The default `MenuOptions::maxEntries` is 256. Every entry, including a duplicate,
also counts against its action set's `maxPresenters` limit. An empty menu is
valid. Allocating less height clips rows through the ordinary layout; this control
does not add scrolling automatically. Use a different composition when every row
must remain reachable in a small viewport.

## Ownership and errors

Menus reuse the [ActionSet lifetime and error contracts](actions.md). A recipe
retains its cold declarations until mounting; the attached scene owns the
callbacks. Reusing a set already attached to another scene is rejected. Closing
the scene retires the actions, including when a callback closes its own scene.
An ordinary failed menu mount restores newly attached action sets to their cold
state. Actions already used by the scene remain attached. Independent bindings
created by application setup code are preserved; arbitrary setup side effects
are not rolled back.

Menu configuration and action admission failures follow the ordinary typed
setup-error path to `ui::runApp`. A command callback can return its existing
checked result; the scene collects that result through the same path used by
action buttons and key handlers. Menu data never contains executable commands
loaded from a scene resource.
The current menu factory receives code-bound `ActionRef` values; scene-resource
syntax for binding a menu's commands is not provided.

The first implementation provides a bounded command list. It does not perform
background search or keep command history; use the separate model-backed search
APIs when building those application features.
