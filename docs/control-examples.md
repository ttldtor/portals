# Control examples {#control-examples}

[Русская версия](ru/control-examples.md) · [All examples](examples.md) · [Scene resources](scene-loading-guide.md)

Use this index to find a complete application or the closest existing
resource example. Links marked **reference** describe resource settings or show
a fragment; they are not complete applications.

| Topic | Complete C++ program | Resource program or reference |
|---|---|---|
| Protected password | [guide_password.cpp](examples/guide_password.cpp) | [guide_password_resource.cpp](examples/guide_password_resource.cpp) |
| Frames and scrolling | [guide_scrollbar_skin.cpp](examples/guide_scrollbar_skin.cpp) | [guide_scrollbar_resource.cpp](examples/guide_scrollbar_resource.cpp) |
| Dialogs | [guide_dialog.cpp](examples/guide_dialog.cpp), [guide_dialog_actions.cpp](examples/guide_dialog_actions.cpp) | [guide_dialog_resource.cpp](examples/guide_dialog_resource.cpp) |
| CheckBox changes | [guide_check_change.cpp](examples/guide_check_change.cpp) | [guide_check_resource.cpp](examples/guide_check_resource.cpp) |
| Notification severity | [guide_toast.cpp](examples/guide_toast.cpp) | [guide_toast_resource.cpp](examples/guide_toast_resource.cpp); the resource supplies only the body |
| Loading indicator | [guide_loading_indicators.cpp](examples/guide_loading_indicators.cpp) | [guide_loading_resource.cpp](examples/guide_loading_resource.cpp) |
| Shared actions | [guide_actions.cpp](examples/guide_actions.cpp), [shared actions guide](actions.md) | Actions and callbacks are code-bound; scene resources do not define executable actions |

The password programs use the protected `Password` control and inspect a
synchronous `SecretView` only to update an `Empty`/`Supplied` status. They never
copy secret bytes into ordinary text. An ordinary `LineEdit` configured with
`LineEditDisplayMode::PASSWORD` only masks its presentation; it retains the
ordinary plaintext model and history described in the
[editor display guide](controls.md#editor-presentation-limits).

Resource programs declare controls and public settings in the scene text, then
bind behavior through typed C++ refs and handlers. Executable callbacks remain
C++ declarations. In particular, the notification resource only supplies the
body inside a C++ `NotificationCenter`; commands, the notification host and its
action callbacks stay in C++. The password resource contains only an empty
initial value; nonempty password literals are rejected by the resource contract.
