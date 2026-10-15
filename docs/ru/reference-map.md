# Содержание документации {#reference-map-ru}

[Английская версия](../reference-map.md)

## Начало работы

[Краткое руководство](index.md) · [Руководства по задачам](guides.md) · [Глоссарий](glossary.md)
· [Сборка Portals и примеров](getting-started.md) · [Подключение пакета](package.md)

## API приложения

- [Командная строка и снимки процесса](cli.md).

- [Декларативная сцена](declarative-ui.md), [запуск](application.md), [управляемые контролы](managed-controls.md).
- [Стили](styling-guide.md), [темы](themes-guide.md), [синтаксис PSS](styles.md), [слои](style-layers.md).
- [Привязки и события](bindings-guide.md), [свойства](property.md), [текстовые ссылки](text-property.md).
- [Ввод](input.md), [фокус](focus.md), [диагностика](diagnostics.md).
- [Примеры приложений](examples.md), [текст и колонки](compact-examples.md), [пример редактора](editor-history-demo.md), [ограничения](limitations.md).

## Контролы и данные

- [Основные контролы](controls.md), [LineEdit](line-edit.md), [история текста](text-history.md), [Undo/Redo](editor-history.md).
- [Компоновка](layout.md), [сетка и наложение](grid.md), [прокрутка](scroll.md), [графики](chart.md).
- [Модели](model.md), [запросы](model-queries.md), [подготовка представлений](virtual-preparation.md).
- [Виртуальные списки](virtual-list.md), [виртуальные таблицы](virtual-table.md).
- [Корутины](ui-coroutines.md), [планировщик](scheduler.md).

## Устройство библиотеки

- [Архитектура](architecture.md), [дерево виджетов](widget-tree.md), [геометрия](geometry.md).
- [Текст и Unicode](text.md), [буфер ячеек](cell-buffer.md), [отрисовка](render.md), [разница кадров](diff.md).
- [Терминал](terminal.md), [проверка терминала](terminal-probe.md).
- Экспериментальные [сцены](compile-time-scenes.md), [стили](compile-time-styles.md) и [PSS](compile-time-pss.md) времени компиляции.
- [Совместимость со старым API (EN)](../legacy.md).

## Справка по API

В HTML-справке есть отдельное меню **API reference**: типы, концепты,
пространства имён, члены типов и заголовки. В исходниках публичные контракты
находятся в `include/portals`. Для обычного приложения начните с `ui.hpp`,
`managed.hpp` и `application.hpp`. Генерация и открытие локальной справки
описаны в [сборке Portals и примеров](getting-started.md).

## Планы и разработка

[Возможности и майлстоуны](planned-features.md) · [Загрузка сцен — TBD](scene-loading-guide.md)

Материалы для разработчиков библиотеки пока доступны на английском:
[разработка](../development.md), [тестирование](../testing.md),
[выпуск версий](../releasing.md), [заметки о версиях](../../ReleaseNotes.md).

[Зависимости и лицензии](dependencies.md).

[Справка по сценам и рецептам](declarative-reference.md).
