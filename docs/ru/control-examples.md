# Примеры элементов управления {#control-examples-ru}

[English version](../control-examples.md) · [Все примеры](../examples.md) · [Ресурсы сцен](../scene-loading-guide.md)

Здесь собраны полные приложения и подходящие примеры ресурсов. Пометка
**справка** означает, что ссылка описывает настройки или показывает фрагмент,
но не полное приложение.

| Тема | Полная программа на C++ | Программа ресурса или справка |
|---|---|---|
| Композиция журнала поиска | [main.cpp](../../examples/search_journal/main.cpp): поле оператора, две памятки, выбранное совпадение и ограниченный журнал | Контракты [FormField](form-fields.md), [FindBar](find-bar.md), [LogView](log-view.md) |
| Индексированные подсказки | [Выбор команды](live-search.md), [main.cpp](../../examples/suggestions/main.cpp) | Запрос и действия остаются в C++; результат использует обычный виртуальный список |
| Фоновая проверка поля | [Каталог имён](form-fields.md#form-field-async-ru), [main.cpp](../../examples/async_validation/main.cpp) | Рабочие правила задаются кодом |
| Буквальный поиск | [Руководство FindBar](find-bar.md), [main.cpp](../../examples/find_bar/main.cpp) | [guide_find_bar_resource.cpp](../examples/guide_find_bar_resource.cpp) |
| Поля с подписями и валидация | [Настройки учётной записи](form-fields.md), [main.cpp](../../examples/form_field/main.cpp) | [guide_form_field_resource.cpp](../examples/guide_form_field_resource.cpp) |
| Защищённое поле пароля | [guide_password.cpp](../examples/guide_password.cpp) | [guide_password_resource.cpp](../examples/guide_password_resource.cpp) |
| Рамки и прокрутка | [guide_scrollbar_skin.cpp](../examples/guide_scrollbar_skin.cpp) | [guide_scrollbar_resource.cpp](../examples/guide_scrollbar_resource.cpp) |
| Ограниченный текстовый журнал | [LogView](log-view.md), [main.cpp](../../examples/log_view/main.cpp) | [guide_log_view_resource.cpp](../examples/guide_log_view_resource.cpp) |
| Диалоги | [guide_dialog.cpp](../examples/guide_dialog.cpp), [guide_dialog_actions.cpp](../examples/guide_dialog_actions.cpp) | [guide_dialog_resource.cpp](../examples/guide_dialog_resource.cpp) |
| Изменения CheckBox | [guide_check_change.cpp](../examples/guide_check_change.cpp) | [guide_check_resource.cpp](../examples/guide_check_resource.cpp) |
| Важность уведомлений | [guide_toast.cpp](../examples/guide_toast.cpp) | [guide_toast_resource.cpp](../examples/guide_toast_resource.cpp); ресурс задаёт только содержимое |
| Индикатор загрузки | [guide_loading_indicators.cpp](../examples/guide_loading_indicators.cpp) | [guide_loading_resource.cpp](../examples/guide_loading_resource.cpp) |
| Общие действия | [guide_actions.cpp](../examples/guide_actions.cpp), [руководство по действиям](../actions.md) | Действия и callback задаются в коде; ресурсы сцен не объявляют исполняемые действия |

Программы с паролем используют защищённый `Password` и синхронно передают
`SecretView` только для изменения статуса «Пусто»/«Введено». Байты секрета не
копируются в обычный текст. У обычного `LineEdit` с
`LineEditDisplayMode::PASSWORD` маскируется только отображение: модель и история
сохраняют открытый текст. Подробнее — в [руководстве по отображению редактора](../controls.md#editor-presentation-limits).

В ресурсном тексте задаются элементы управления и открытые настройки; поведение
подключается в C++ через типизированные ссылки и обработчики. Исполняемые
callback остаются объявлениями C++. В частности, ресурс уведомлений задаёт
только содержимое внутри созданного в C++ `NotificationCenter`; команды, хост
уведомлений и callback действий остаются в C++. Ресурсный пример пароля
содержит только пустое начальное значение; контракт ресурса отклоняет
непустые литералы пароля.
