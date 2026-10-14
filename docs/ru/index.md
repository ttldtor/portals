# Portals: краткое руководство {#short-guide-ru}

[English](../index.md) · [Содержание и предметный индекс](guides.md) · [Глоссарий](glossary.md)

Portals — разрабатываемая библиотека терминального UI на C++23 с сохраняемым
деревом контролов. Для приложения начинайте с декларативного API `ui`.
Полная карта технических разделов пока доступна [на английском](../reference-map.md).

## Начните с декларативного API

Опишите дерево контролов, затем добавьте стиль и обработчики через pipe-стадии.
Кнопка дописывает текст в редактор, F10 запрашивает штатное завершение.

```cpp
#include <portals/ui_style.hpp>

int main() {
  using namespace portals;

  ui::Ref<LineEdit> message;
  ui::Ref<Button> add;

  auto scene = ui::scene(ui::vbox(
    ui::label("Greeting"),
    ui::lineEdit(message, ""),
    ui::button(add, "Add Hello")));

  return scene
    | ui::style("Button { bold: true; }")
    | ui::onClick(add, [message] { message.text() += "Hello!"; })
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

Runner пытается восстановить терминал и сообщает об ошибках. В этой короткой
форме рецепт создаётся до входа в `runApp`; фабричная перегрузка нужна, если
создание рецепта тоже должно попасть под проверяемую границу ошибок.
Команда завершения определяет текущую сцену внутри допущенного callback.
Явный `SceneRef` остаётся вариантом для действия, адресованного конкретной сцене.
[Декларативная композиция (EN)](../declarative-ui.md) описывает callbacks,
ошибки setup и низкоуровневый типизированный результат `run()`.

## Сборка и запуск

На Windows используйте x64 Developer PowerShell для Visual Studio:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
./build/windows-msvc-debug/examples/gallery/portals_gallery
```

На Linux используйте preset `linux-gcc-debug` и соответствующий каталог сборки.
[Начало работы (EN)](../getting-started.md) описывает компиляторы, тесты и локальную HTML-справку.

## Что доступно

- Формы: Label, Button, CheckBox, LineEdit, Row/Column, Grid/Overlay и наблюдающие ссылки.
- Данные: неизменяемые модели, виртуальные списки/таблицы, прокрутка и графики.
- Стили: темы, слои, compile-time и runtime PSS.
- Async: необязательные UI-корутины с ограниченными ресурсами поверх существующего Scheduler.
- Ввод: строковые сочетания, альтернативы, последовательности и явный выбор фаз.
  Доставка зависит от backend. Согласование Kitty и отдельные физические
  модификаторы пока **TBD — M7/U89**; направление подготовки не означает реализацию.
- Ошибки: live-реакции `onError` и отдельный итоговый отчёт после cleanup.
  Встроенный индикатор и панель диагностики — **TBD**, общий фундамент
  M2/U22/U24 и M3/U33; отдельная реализация Kitty UI не начата.

API развивается. Поддержанные платформы и точные границы runtime описаны
в [ограничениях (EN)](../limitations.md).

## Гайды по задачам

| Задача | Куда читать |
|---|---|
| Подготовить стили и загрузить PSS | [Стили](styling-guide.md) |
| Выбрать палитру и менять параметры | [Темы](themes-guide.md) |
| Связать состояние и обрабатывать события | [Биндинги, события и Ref](bindings-guide.md) |
| Пересмотреть compile-time синтаксис | [Типизированные стили](compile-time-styles.md), [PSS](compile-time-pss.md) |
| Загрузить сцену из строки/файла | [Загрузка сцен — TBD, M1/U84](scene-loading-guide.md) |
| Найти термин или будущий этап | [Глоссарий](glossary.md), [содержание и milestones](guides.md) |
| Подключить CMake-пакет | [Package (EN)](../package.md) |
| Разобраться с владением и кадром | [Architecture (EN)](../architecture.md) |

[Примеры (EN)](../examples.md) ведут к полным программам.
[Release notes (EN)](../../ReleaseNotes.md) сохраняют историю версий;
проектные эскизы и сравнения измерений описывают собственный датированный срез.
