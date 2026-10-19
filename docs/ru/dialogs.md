# Диалоги и overlay host {#dialogs-ru}

[English version](../dialogs.md)

Overlay host удерживает одно основное содержимое и зарегистрированные слои
диалогов в том же retained-дереве. Открытие меняет видимость, допуск ввода и фокус
без второго event loop и пересоздания сцены. Закрытый диалог остаётся смонтированным
и сохраняет состояние дочерних контролов.

## Полная modal-форма

\include guide_dialog.cpp

[Полный исходник](../examples/guide_dialog.cpp). Подключите `portals::core`;
см. [пакет](package.md). [Отдельный проект руководств](examples.md) собирает
`guide_dialog`, корневая сборка с `PORTALS_BUILD_TESTS=ON` — `portals_docs_dialog`.
Installed consumers компилируют этот же сопровождаемый исходник. Программа
открывает нативный терминал; CLI и режима автоматического replay у неё нет.

Поле и CheckBox хранят состояние сцены; пример не подключается к feed и не
сохраняет настройки. Tab остаётся внутри активного modal-слоя. Escape сначала
получают дочерние контролы; непринятая отмена закрывает верхний modal. Дочерний
контрол может принять Escape. Фоновый F10 и основная кнопка Exit недоступны, пока
modal открыт. Сначала используйте Close или непринятый Escape; F10 завершает
основную форму через штатный runner с восстановлением терминала.

`ui::overlayHost(body, layers...)` принимает одно обычное содержимое и recipes
диалогов. `ui::dialog(reference, title, body, options)` создаёт изначально закрытый
слой. Заголовки используют существующие владеющие преобразования текста/ресурсов.
Эта C++-композиция не регистрирует `Dialog` и `OverlayHost` как встроенные теги
scene-resource loader. Копии Ref, захваченные до mount, наблюдают общий слот: публикация происходит только
после успешного полного монтажа. Повторно откройте закрытый диалог, не монтируя его
контролы заново при каждом показе.

## Checked-команды и публикация

`reference.open()` и `reference.dismiss()` возвращают владеющий
`ui::DialogCommandResult`. В допущенных button/key/dismissal callbacks короткая
запись участвует в существующих диагностике и остановке runner. Вне этого контекста
проверяйте результат: boolean test сообщает успех, `error()` копирует владеющую
ошибку, `result()` возвращает `std::expected<void, ManagedError>`. Нового типа
результата callback, exception shortcut, скрытого retry или очереди открытия нет.
`reference.opened()` — checked-запрос `std::expected<bool, ManagedError>`, а не
долгоживущая ссылка на состояние.

Выполняйте UI-affine операции на потоке владельца. Cold/неопубликованный Ref или
закрытый owner возвращает `CLOSED`, удалённая identity — `STALE_NODE`, чужая сессия —
`WRONG_SESSION`. Первое открытие с привязкой к anchor до пригодного завершённого
layout возвращает `WOULD_BLOCK`, не планируя будущий показ. После штатной подготовки
приложение может явно повторить checked-команду, если панель ещё нужна. Открытие по
центру не требует ранее опубликованной геометрии: штатная подготовка вычисляет
положение панели.

Ошибки валидации и выделения памяти до публикации оставляют переход неопубликованным.
После публикации видимости/ввода/фокуса ошибка blur/gain или dismissal callback
сохраняет уже выполненный переход. Проверяйте `opened()`, а не считайте любую ошибку
доказательством неизменившегося состояния. `ManagedError` сохраняет phase, node,
исходную cause и независимую информацию cleanup; runner сообщает итог после попытки
восстановления терминала.

## Ввод, фокус и retirement

Modal ограничивает новый ввод и фокус своим поддеревом. Открытие из допущенного
callback не прерывает его и не воспроизводит событие у новой цели. Фоновый pointer
capture отменяется до дальнейшей доставки наружу. Tab и Shift-Tab используют
существующий порядок внутри верхнего modal. Даже modal без допустимого дочернего
фокуса закрывает доступ к body: фокус пуст, непринятый Escape работает. Вложенные
modal используют верхний слой; закройте его до закрытия нижнего. Удалённый opener
не используется повторно как identity возврата фокуса.

`.onDismiss(callback)` получает `DialogDismissReason::PROGRAMMATIC` для явной
команды или `CANCEL` для managed-отмены. Допустимы `void` и ровно
`std::expected<void, TreeError>`. Уведомление следует после commit; ошибка не
открывает панель заново и не повторяет callback при retry. Dismiss уже закрытого
живого слоя — no-op. Reentrant open/dismiss того же host во время staging,
focus/capture delivery, dismissal notification или teardown возвращает
`WOULD_BLOCK`. Не уничтожайте владеющие scene/application из callback.

Скрытие завершает activation. Disable завершает допуск ввода/modality и возвращает
фокус, сохраняя картинку disabled-слоя. Raw show/reenable не активирует managed-сессию:
нужен новый checked `open()`. Удаление завершает registrations и capture. Эти пути
retirement не вызывают пользовательский onDismiss. Допущенные callbacks удерживают
captures до возврата, в том числе при self-removal.

Отказ доставки model notification не откатывает опубликованное значение модели
или синхронную команду диалога. Equal assignment не повторяет отвергнутую доставку.
Сохраняются [property receipts](property.md) и [managed-подготовка](managed-controls.md).
Прикладная задача, намеренно вызывающая живой Ref позже, — новая команда, а не
автоматически отменяемое библиотечное уведомление.

## Немодальные панели и anchors

`DialogOptions::mode = DialogMode::NON_MODAL` сохраняет интерактивность body.
Открытие не забирает клавиатурный фокус. Pointer activation автоматически не вводит
keyboard focus scope; явный focus или Tab позволяет войти в панель. Закрытие
возвращает фокус только при сохранённом владении соответствующим переходом;
новый выбор body остаётся. Escape закрывает немодальную панель только при
маршрутизации отмены в этот слой; фоновый Escape не является глобальным close.

Эти options компилируются в installed consumer:

\snippet dialog.cpp dialog-options

Передайте их в `ui::dialog(reference, title, body, helpOptions)`. Consuming modifier
`.anchor(opener, DialogPlacement::BELOW_START)` разрешает cold Ref body при полном
монтаже; `ABOVE_START` предпочитает другую сторону. Anchor должен принадлежать
обычному body этого точного host. Другой host или любая dialog-owned цель даёт
`INVALID_ARGUMENT`, включая скрытое содержимое диалога. Неудачный полный mount не
публикует частичные Ref и завершает созданное поддерево.

Первое anchored open требует живой пригодной опубликованной геометрии; отсутствие
или полная обрезка даёт `WOULD_BLOCK`. У уже открытой панели stale/hidden/полностью
обрезанный anchor даёт центрирование на следующем prepared pass. Disabled anchor
с живой геометрией остаётся пригодным. Используется владеющая геометрия существующей
ограниченной подготовки, включая окружающие scroll areas. Граф anchors между
диалогами и дополнительные passes не вводятся.

## Геометрия и оформление

Intrinsic size host зависит только от body. Каждый диалог имеет structural root
размером с host и непрозрачную панель с рамкой. Это не `SizedWidget`: задавайте
размер панели через owning options вместо `ui::width/height`:

| Option | Значение по умолчанию и ограничение |
| --- | --- |
| `mode` | `MODAL`; `NON_MODAL` сохраняет ввод снаружи. |
| `placement` | `CENTER`; anchored preferences — `BELOW_START`/`ABOVE_START`. |
| `maximumPanelSize` | 60 на 20 ячеек; оба cap строго положительны. |
| `viewportMargin` | 1 ячейка, неотрицательный; уменьшается на tiny viewport. |
| `anchorGap` | 1 ячейка, неотрицательный. |
| `closeOnEscape` | `true`; дочерние контролы всё равно получают отмену первыми. |
| `frame` | Существующая glyph/paint-budget политика `BorderOptions`. |

Панель уменьшается до доступного места; tiny/zero viewport даёт обрезанную или
пустую геометрию без отрицательных размеров. Центрирование округляется вниз.
Anchored placement может выбрать сторону с большей высотой, затем ограничивает
координаты host. Paint и hit testing используют те же clipping и порядок sibling.
Shadow/backdrop engine, draggable window manager и отдельного style cascade нет;
поддерживаемое оформление задают существующие [слои стилей](style-layers.md).

`OverlayHostOptions::maxLayers` по умолчанию равен 32 и должен быть положительным.
Он считает все зарегистрированные слои, включая скрытые. Options-first overload:
`ui::overlayHost(hostOptions, body, layers...)`. Удаление завершает регистрацию;
повторный open/close не регистрирует ещё одну копию.

Закрытым и неизменившимся диалогам не нужны polling timer и постоянная перерисовка.
Shutdown сцены завершает их вместе с остальными managed-ресурсами. MessageBox result,
save/discard workflow и asynchronous dialog result остаются прикладными
композициями, а не дополнительными обещаниями этого foundation.
