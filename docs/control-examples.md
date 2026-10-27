# Control examples {#control-examples}

[Русская версия](ru/control-examples.md) · [All examples](examples.md) · [Scene resources](scene-loading-guide.md)

Use this index to find a complete application or the closest existing
resource example. Links marked **reference** describe resource settings or show
a fragment; they are not complete applications.

| Topic | Complete C++ program | Resource program or reference |
|---|---|---|
| Search journal composition | [main.cpp](../examples/search_journal/main.cpp): operator field, two troubleshooting notes, selected-match summary and bounded journal | [FormField](form-fields.md), [FindBar](find-bar.md), [LogView](log-view.md) contracts |
| Indexed suggestions | [Command browser](live-search.md), [main.cpp](../examples/suggestions/main.cpp) | Query and actions remain C++; the result uses an ordinary virtual list |
| Background field validation | [Account directory](form-fields.md#form-field-async), [main.cpp](../examples/async_validation/main.cpp) | Worker rules are code-bound |
| Literal search | [FindBar guide](find-bar.md), [main.cpp](../examples/find_bar/main.cpp) | [guide_find_bar_resource.cpp](examples/guide_find_bar_resource.cpp) |
| Labeled fields and validation | [Account settings](form-fields.md), [main.cpp](../examples/form_field/main.cpp) | [guide_form_field_resource.cpp](examples/guide_form_field_resource.cpp) |
| Protected password | [guide_password.cpp](examples/guide_password.cpp) | [guide_password_resource.cpp](examples/guide_password_resource.cpp) |
| Frames and titles | [guide_frame_styles.cpp](examples/guide_frame_styles.cpp), [recording and guide](compact-primitives.md) | [Frame properties](compact-primitives.md) reference |
| On-frame scrollbar styles | [guide_scrollbar_gallery.cpp](examples/guide_scrollbar_gallery.cpp), [recording and guide](scroll-controls.md) | [guide_scrollbar_resource.cpp](examples/guide_scrollbar_resource.cpp) |
| Bounded plain-text log | [LogView](log-view.md), [main.cpp](../examples/log_view/main.cpp) | [guide_log_view_resource.cpp](examples/guide_log_view_resource.cpp) |
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
