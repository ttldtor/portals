# Декларативный API {#declarative-ui-ru}

[Английская версия](../declarative-ui.md)

Декларативный API позволяет описать расположение контролов и назначить
обработчики событий. `ui::scene(...)` создаёт [сцену](glossary.md#glossary-scene-ru),
а `ui::runApp()` запускает приложение.

## Запуск формы

В этой программе кнопка **Add Hello** добавляет текст в поле ввода.
Кнопка **Exit** и клавиша F10 завершают приложение. Текст кнопок полужирный.

[Исходник](../examples/docs_basic.cpp). Подключение Portals и сборка своего
приложения описаны в [кратком руководстве](index.md).

```cpp
#include <portals/ui_style.hpp>

int main() {
  using namespace portals;

  ui::Ref<LineEdit> message;
  ui::Ref<Button> add;
  ui::Ref<Button> exit;

  auto scene = ui::scene(ui::vbox(
    ui::lineEdit(message, ""),
    ui::button(add, "Add Hello"),
    ui::button(exit, "Exit")));

  return scene
    | ui::style("Button { bold: true; }")
    | ui::onClick(add,
                  [message] {
                    message.text() += "Hello!";
                  })
    | ui::onClick(exit, ui::quit)
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
}
```

`ui::vbox` располагает дочерние контролы вертикально, `ui::hbox` — горизонтально.
`ui::lineEdit`, `ui::button` и `ui::label` описывают создаваемые контролы.
Такое описание называется [рецептом](glossary.md#glossary-recipe-ru).
Сцена создаёт контролы при подготовке к запуску.

Цепочка `|` добавляет настройки и обработчики к сцене. Каждая операция —
[стадия UI](glossary.md#glossary-ui-stage-ru), например `ui::style(...)` или
`ui::onClick(...)`. Последняя операция, `ui::runApp()`, запускает цикл событий,
пытается восстановить терминал и очистить сцену, затем возвращает код завершения:
0 при успехе, 1 при типизированной ошибке. Стандартный отчёт об ошибке выводится
в stderr после очистки.

Для сцен, контролов и обработчиков нужен `<portals/ui.hpp>`.
В примере подключён `<portals/ui_style.hpp>`, поскольку здесь также используется
`ui::style(...)` для стилей времени выполнения. Синтаксис PSS описан
в [руководстве по стилям](styling-guide.md).

## Ссылки на контролы

`ui::Ref<T>` позволяет обработчику обратиться к контролу, созданному сценой.
Передайте ссылку первым аргументом фабрики, затем захватите её по значению
в обработчиках. Например, `message.text() += "Hello!"` добавляет текст в поле ввода.
Присваивание, чтение и форматирование описаны в [свойствах текста](text-property.md).

Ссылка наблюдает за контролом, которым владеет сцена. Ссылки становятся доступны
после успешного монтажа всего рецепта. После удаления контрола или закрытия сцены
обратиться к нему через ссылку нельзя. Объекты, захваченные по ссылке C++,
должны оставаться живыми всё время, пока могут вызываться их обработчики.

## Обработка нажатий кнопки

<a id="pipe-click-handlers-ru"></a>

`ui::onClick(ref, handler)` добавляет обработчик после создания контролов.
Обработчики одной кнопки вызываются в порядке регистрации.
Обработчик может принимать `ui::ClickParams&`, чтобы проверить модификаторы
и остановить вызов оставшихся обработчиков.

Эта полная программа показывает **Applied** при обычной активации.
Если терминал передал активацию с Shift, она показывает **Apply with Shift**
и пропускает второй обработчик. F2 меняет текст метки, F10 завершает приложение.

[Исходник](../examples/docs_events.cpp).

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::Ref<Label> status;
  ui::Ref<Button> apply;

  auto scene = ui::scene(ui::vbox(
    ui::label(status, "Ready").id("status"),
    ui::button(apply, "Apply")));

  return scene
    | ui::onClick(apply,
                  [status](ui::ClickParams &params) {
                    if (params.mods.has(key::SHIFT)) {
                      status.text() = "Apply with Shift";
                      params.stop();
                    }
                  })
    | ui::onClick(apply,
                  [status] {
                    status.text() = "Applied";
                  })
    | ui::onKey("F2",
                [status] {
                  status.text() = "F2";
                })
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

`params.mods.has(key::SHIFT)` проверяет семантические модификаторы.
`params.source` различает активацию с клавиатуры, мышью и программную;
`params.target` указывает кнопку. Параметры заимствованы на время вызова.
Копия параметров сохраняет данные события, но вызов `stop()` у копии
не останавливает исходную активацию. Подробнее — в
[руководстве по привязкам и событиям](bindings-guide.md).

Некоторые терминалы используют Shift+мышь для выделения текста и не передают
такое событие приложению. Возможности терминалов и передача модификаторов
описаны в разделе [ввода](input.md).

## Горячие клавиши

<a id="short-key-bindings-ru"></a>

Используйте `ui::onKey("Ctrl+S", handler)` для сочетания клавиш,
`ui::onKey("F10 | Ctrl+C", ui::quit)` для альтернатив и
`ui::onKey("Ctrl+K, K", handler)` для последовательных нажатий.
Обычная строковая форма сочетания принимает события нажатия.
Для команды, которая должна выполняться и при повторе, используйте `ui::pressRepeat(...)`.
Правила сопоставления описаны в разделе
[синтаксиса сочетаний и выбора событий](input.md).

Передавайте `ui::quit` напрямую, если обработчик должен только завершить
текущую сцену. Для явного обращения к другой сцене используйте `SceneRef`;
подробнее — в [справке по сценам](declarative-reference.md).

## Общее состояние контролов

Создайте общее состояние в фабрике рецепта сцены. Фабрика получает
`ManagedScene&` и вызывается при подготовке. Её проверяемый результат
передаёт ошибки создания состояния функции запуска приложения.

Два редактора ниже используют общее строковое состояние. Фиксация изменения
в одном обновляет другой через управляемую привязку. F10 завершает приложение.

[Исходник](../examples/docs_state.cpp).

```cpp
#include <portals/ui.hpp>
#include <string>

int main() {
  using namespace portals;

  auto scene = ui::scene([](ManagedScene &owner) {
    return owner.state(std::string("Portals")).transform([](auto model) {
      return ui::vbox(ui::lineEdit(model), ui::lineEdit(model));
    });
  });

  return scene
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

Для больших наборов данных создайте модель в той же фабрике и передайте
наблюдаемую ссылку на неё в `ui::virtualList`, `ui::virtualTable` или `ui::chart`.
Подробнее — в разделах [моделей](model.md) и [виртуальных таблиц](virtual-table.md).

## Настройка виртуальных представлений

<a id="typed-columns-and-named-view-settings-ru"></a>

`tableColumn` и `tableColumns` описывают поля таблицы и их форматирование.
Синтаксис приведён в [руководстве по тексту и колонкам](compact-examples.md).
`ui::VirtualTableConfig<T>`, `ui::VirtualListConfig<T>` и `ui::ChartConfig<T>`
задают настройки представлений по именам. Контракты описаны в
[справке по сценам](declarative-reference.md).

## Примеры приложений

- [Галерея](../../examples/gallery/fluent_form.hpp): редактор, CheckBox, компоновка и смена темы.
- [Собственный виджет](../../examples/custom_widget/fluent_meter.hpp): состояние, клавиши и подготовка.
- [Панель данных](../../examples/dashboard/dashboard.hpp): виртуальная таблица, график и асинхронная загрузка.
- [Запуск с командной строкой](cli.md): разбор параметров до открытия терминала.
- [Справка по сценам](declarative-reference.md): подготовка, владение, типизированные результаты и отчёты.

## Тонкие настройки сцены

<a id="scene-fine-settings-ru"></a>

<details>
<summary>Как установить лимиты сцены</summary>

Задайте `ui::SceneOptions` до создания сцены и передайте вторым аргументом
в `ui::scene`. Настройки копируются в сцену и проверяются при подготовке.
Изменение этих лимитов у уже созданной сцены API не предусматривает.

| Поле в `options.managed` | Назначение | По умолчанию |
|---|---|---|
| `maxNodes` | Сохраняемые регистрации управляемых узлов, включая удалённые до следующей подготовки | 4096 |
| `maxStates` | Свойства во владении сцены | 256 |
| `maxModels` | Модели во владении сцены | 64 |
| `diagnostics.maxErrors` | Сохраняемые записи диагностики | 64 |
| `diagnostics.maxContexts` | Сохраняемые причинные контексты одной ошибки | 8 |
| `diagnostics.maxContextBytes` | Суммарное число байтов подписей контекстов одной ошибки | 1024 |

Все шесть лимитов должны быть больше нуля. Увеличьте их, если форме нужно
больше управляемых объектов или подробностей диагностики. `maxErrors`
ограничивает размер истории; число ошибок, которое допускает приложение,
он не задаёт. При переполнении сохраняются старые записи и растёт `droppedErrors`.

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::SceneOptions options;
  options.managed.maxNodes = 8192;
  options.managed.maxStates = 512;
  options.managed.maxModels = 128;
  options.managed.diagnostics.maxErrors = 128;
  options.managed.diagnostics.maxContexts = 16;
  options.managed.diagnostics.maxContextBytes = 2048;

  auto scene = ui::scene(ui::label("F10: quit"), options);

  return scene
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

</details>

<details>
<summary>Как настроить обработчики и паузу последовательности</summary>

Эти настройки также передаются в `ui::scene` при создании. Меняйте их,
когда у формы много обработчиков или пользователям нужно больше времени
между нажатиями в клавиатурной последовательности.

| Поле | Назначение | По умолчанию |
|---|---|---|
| `maxConfigurations` | Отложенные операции configure/key/click до подготовки | 64 |
| `managed.maxKeyHandlers` | Сохраняемые регистрации клавиатурных обработчиков | 64 |
| `managed.maxClickHandlers` | Обработчики нажатия в цепочках всей сцены | 256 |
| `managed.maxClickHandlersPerNode` | Такие обработчики одной кнопки | 64 |
| `managed.keySequenceTimeout` | Пауза без ввода между нажатиями последовательности, миллисекунды | 1000 |

Каждое значение должно быть больше нуля. В примере между нажатиями
`Ctrl+K, K` допускаются две секунды. Запятая обозначает два последовательных
нажатия; подробнее — в [синтаксисе клавиатурных сочетаний](input.md).

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::SceneOptions options;
  options.maxConfigurations = 128;
  options.managed.maxKeyHandlers = 128;
  options.managed.maxClickHandlers = 512;
  options.managed.maxClickHandlersPerNode = 128;
  options.managed.keySequenceTimeout = 2000;

  auto scene = ui::scene(ui::label("Ctrl+K, K or F10: quit"), options);

  return scene
    | ui::onKey("Ctrl + K, K", ui::quit)
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

</details>
