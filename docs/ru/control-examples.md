# Примеры элементов управления {#control-examples-ru}

[English version](../control-examples.md) · [Все примеры](../examples.md) · [Ресурсы сцен](../scene-loading-guide.md)

Здесь собраны полные приложения и подходящие примеры ресурсов. Пометка
**справка** означает, что ссылка описывает настройки или показывает фрагмент,
но не полное приложение.

| Тема | Полная программа на C++ | Программа ресурса или справка |
|---|---|---|
| Защищённое поле пароля | [guide_password.cpp](../examples/guide_password.cpp) | [guide_password_resource.cpp](../examples/guide_password_resource.cpp) |
| Рамки и прокрутка | [guide_scrollbar_skin.cpp](../examples/guide_scrollbar_skin.cpp) | [guide_scrollbar_resource.cpp](../examples/guide_scrollbar_resource.cpp) |
| Диалоги | [guide_dialog.cpp](../examples/guide_dialog.cpp), [guide_dialog_actions.cpp](../examples/guide_dialog_actions.cpp) | [guide_dialog_resource.cpp](../examples/guide_dialog_resource.cpp) |
| Изменения CheckBox | [guide_check_change.cpp](../examples/guide_check_change.cpp) | [guide_check_resource.cpp](../examples/guide_check_resource.cpp) |
| Важность уведомлений | [guide_toast.cpp](../examples/guide_toast.cpp) | [guide_toast_resource.cpp](../examples/guide_toast_resource.cpp); ресурс задаёт только содержимое |
| Индикатор загрузки | [guide_loading_indicators.cpp](../examples/guide_loading_indicators.cpp) | [guide_loading_resource.cpp](../examples/guide_loading_resource.cpp) |
| Общие действия | [guide_actions.cpp](../examples/guide_actions.cpp), [руководство по действиям](../actions.md) | Действия и callback задаются в коде; ресурсы сцен не объявляют исполняемые действия |

Программы с паролем используют защищённый `Password` и синхронно передают
`SecretView` только для изменения статуса «Пусто»/«Введено». Байты секрета не
копируются в обычный текст. У обычного `LineEdit` с
`LineEditDisplayMode::PASSWORD` маскируется только отображение: модель и история
сохраняют открытый текст. Подробнее — в [руководстве по отображению редактора](../controls.md#editing-presentation-and-limits).

В ресурсном тексте задаются элементы управления и открытые настройки; поведение
подключается в C++ через типизированные ссылки и обработчики. Исполняемые
callback остаются объявлениями C++. В частности, ресурс уведомлений задаёт
только содержимое внутри созданного в C++ `NotificationCenter`; команды, хост
уведомлений и callback действий остаются в C++. Ресурсный пример пароля
содержит только пустое начальное значение; контракт ресурса отклоняет
непустые литералы пароля.
