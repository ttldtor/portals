# Первое приложение: заметка {#first-application-ru}

<!-- translation-source: docs/first-application.md
source-revision: 28b4656e9d64ea4c035cc5472145dccce31bdb02
status: current
-->

[English](../first-application.md) · [Руководства](guides.md) · [Глоссарий](glossary.md)

В этом руководстве вы создадите небольшую форму заметки с полем ввода,
кнопками **Save** и **Clear draft**, сочетаниями клавиш и темой оформления.
**Save** копирует текущий текст в строку под полем, а **Clear draft** очищает
поле, сохраняя эту строку. Заметка существует до выхода из приложения: этот
пример не записывает её на диск.

Анимация показывает готовое приложение: F2 сохраняет исходный текст, F3
очищает поле, затем в поле появляется новая заметка, и F2 сохраняет её.
Начнём с формы с полем ввода, затем добавим действия кнопок, сочетания клавиш
и оформление.

![Форма заметки: поле ввода, сохранённый текст и кнопки Save и Clear draft.](../images/quick-note.gif)

## 1. Форма, которую можно запустить

Потребуются Git, CMake 3.24+, Ninja и инструментарий с поддержкой C99 и C++23,
включая `<expected>` и `<format>`. В Windows для этого потребуется x64 Developer
PowerShell для Visual Studio. Подробности подключения приведены в
[кратком руководстве](index.md).

Для первой версии можно создать каталог `quick-note` и сохранить этот код
в `main.cpp`:

<details open>
<summary>Полный main.cpp после шага 1</summary>

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::Ref<LineEdit> draft;

  auto scene = ui::scene(ui::vbox(
    ui::label("Quick note"),
    ui::lineEdit(draft, "Remember the milk"),
    ui::label("Tab: focus | F10: quit")));

  return scene
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

</details>

Здесь `ui::vbox` описывает вертикальное расположение контролов. Это
[рецепт](glossary.md#glossary-recipe-ru), который `ui::scene` сохраняет в
[сцене](glossary.md#glossary-scene-ru). `ui::Ref<LineEdit> draft` даёт доступ
к созданному по рецепту полю. После запуска можно изменить текст
**Remember the milk**; F10 завершает приложение.

Второй файл, `CMakeLists.txt`, подключает приложение к Portals:

```cmake
cmake_minimum_required(VERSION 3.24)
project(QuickNote LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(portals
  GIT_REPOSITORY https://github.com/ttldtor/portals.git
  GIT_TAG cb0fdec5634cfc9d877791313bdee5da2c6aa18f
)
FetchContent_MakeAvailable(portals)

add_executable(quick_note main.cpp)
target_link_libraries(quick_note PRIVATE portals::core)
```

Из каталога `quick-note` эти команды настраивают сборку, собирают и запускают
приложение:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --target quick_note
./build/quick_note
```

В Windows исполняемый файл называется `build/quick_note.exe`; его можно запустить
в терминале. При первой конфигурации CMake загрузит закреплённую ревизию Portals.
Если исходники уже есть локально, для их использования при конфигурации можно добавить
`-DFETCHCONTENT_SOURCE_DIR_PORTALS="C:/path/to/portals"`, где указан путь к вашим исходникам.


## 2. Отображение сохранённого текста

В первой версии есть поле ввода, но ещё нет места, где увидеть сохранённую копию.
После выхода из приложения можно продолжить работу в том же `main.cpp` из шага 1.
Для метки и кнопки понадобятся собственные ссылки после объявления
`ui::Ref<LineEdit> draft;`:

```cpp
  ui::Ref<Label> saved;
  ui::Ref<Button> save;
```

Внутри `ui::vbox(...)` метка и кнопка **Save** размещаются после поля ввода,
а метка с подсказкой остаётся последней:

```cpp
    ui::label(saved, "Nothing saved yet"),
    ui::button(save, "Save"),
```

Действие `saveNote` копирует текст из поля в метку. Его можно разместить между
объявлением `scene` и `return scene`; стадия `ui::onClick` связывает действие
с кнопкой перед уже имеющимся обработчиком F10:

```cpp
  auto saveNote = [draft, saved] {
    saved.text() = draft.text();
  };

  return scene
    | ui::onClick(save, saveNote)
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
```

При нажатии **Save** действие `saveNote` читает текущий текст поля и присваивает
его метке. Оператор `|` добавляет к сцене [стадию UI](glossary.md#glossary-ui-stage-ru),
а `ui::onClick` связывает кнопку с действием. Обработчик захватывает ссылки
по значению: они наблюдают за контролами сцены и не продлевают их жизнь.
Подробнее — в [ссылках на контролы](text-property.md).

<details>
<summary>Полный main.cpp после шага 2</summary>

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::Ref<LineEdit> draft;
  ui::Ref<Label> saved;
  ui::Ref<Button> save;

  auto scene = ui::scene(ui::vbox(
    ui::label("Quick note"),
    ui::lineEdit(draft, "Remember the milk"),
    ui::label(saved, "Nothing saved yet"),
    ui::button(save, "Save"),
    ui::label("Tab: focus | F10: quit")));

  auto saveNote = [draft, saved] {
    saved.text() = draft.text();
  };

  return scene
    | ui::onClick(save, saveNote)
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

</details>

Команда сборки из шага 1 подготовит обновлённую версию. Затем её можно
запустить той же командой `./build/quick_note`. В работающем
приложении текст можно изменить; Tab переводит фокус на **Save**, а Enter
активирует кнопку. Под полем появится сохранённая копия. Дальнейшее редактирование
не изменит её, пока вы снова не активируете **Save**.

## 3. Очистка поля и сочетания клавиш

Форма уже сохраняет копию. Следующая версия добавит отдельный способ очистить
поле и сочетания клавиш для обоих действий. После выхода из приложения
изменения вносятся в `main.cpp` из шага 2. Ссылку для новой кнопки можно
разместить после
`ui::Ref<Button> save;`:

```cpp
  ui::Ref<Button> clear;
```

Внутри `ui::vbox(...)` кнопка **Clear draft** располагается после **Save**.
Последняя метка также перечисляет новые сочетания; объявление `scene` будет
выглядеть так:

```cpp
  auto scene = ui::scene(ui::vbox(
    ui::label("Quick note"),
    ui::lineEdit(draft, "Remember the milk"),
    ui::label(saved, "Nothing saved yet"),
    ui::button(save, "Save"),
    ui::button(clear, "Clear draft"),
    ui::label("Tab: focus | F2: save | F3: clear | F10: quit")));
```

Действие `clearDraft` очищает поле. Его можно объявить после существующего
`saveNote` и связать с новой кнопкой и F3; F2 вызывает сохранение через уже
имеющееся действие. Ниже показан код от `clearDraft` до конца цепочки обработчиков:

```cpp
  auto clearDraft = [draft] {
    draft.text() = "";
  };

  return scene
    | ui::onClick(save, saveNote)
    | ui::onClick(clear, clearDraft)
    | ui::onKey("F2", saveNote)
    | ui::onKey("F3", clearDraft)
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
```

`clearDraft` очищает только поле ввода; сохранённая строка остаётся на экране.
Кнопка **Clear draft** и F3 вызывают одно действие. Кнопка **Save** и F2
вызывают другое — уже созданное на предыдущем шаге `saveNote`.

<details>
<summary>Полный main.cpp после шага 3</summary>

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  ui::Ref<LineEdit> draft;
  ui::Ref<Label> saved;
  ui::Ref<Button> save;
  ui::Ref<Button> clear;

  auto scene = ui::scene(ui::vbox(
    ui::label("Quick note"),
    ui::lineEdit(draft, "Remember the milk"),
    ui::label(saved, "Nothing saved yet"),
    ui::button(save, "Save"),
    ui::button(clear, "Clear draft"),
    ui::label("Tab: focus | F2: save | F3: clear | F10: quit")));

  auto saveNote = [draft, saved] {
    saved.text() = draft.text();
  };

  auto clearDraft = [draft] {
    draft.text() = "";
  };

  return scene
    | ui::onClick(save, saveNote)
    | ui::onClick(clear, clearDraft)
    | ui::onKey("F2", saveNote)
    | ui::onKey("F3", clearDraft)
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

</details>



После повторной сборки и запуска F2 сохраняет текущий текст, а F3 очищает поле.
Сохранённая строка остаётся видна и после очистки. Новый текст можно записать
в неё повторным нажатием F2. Tab перемещает фокус между полем и кнопками; Enter
активирует кнопку в фокусе. Другие формы сочетаний описаны в
[руководстве по вводу](input.md).

## 4. Тема и стиль

В последней версии появятся палитра и правило оформления кнопок. После выхода
из приложения в `main.cpp` из шага 3 можно разместить заголовок стилей после
`#include <portals/ui.hpp>`:

```cpp
#include <portals/ui_style.hpp>
```

Стадии темы и стиля можно разместить в цепочке перед существующими
обработчиками событий:

```cpp
    | ui::theme(nordTheme())
    | ui::style("Button { min-width: 16; bold: true; }")
```

`ui::theme(nordTheme())` выбирает палитру. Правило PSS делает текст кнопок
полужирным и задаёт минимальную ширину в 16 ячеек терминала. Ссылки, форма и
обработчики из шага 3 остаются частью итоговой программы. Доступные варианты
описаны в руководствах по [стилям](styling-guide.md) и [темам](themes-guide.md).

<details>
<summary>Полный main.cpp после шага 4</summary>

```cpp
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

int main() {
  using namespace portals;

  ui::Ref<LineEdit> draft;
  ui::Ref<Label> saved;
  ui::Ref<Button> save;
  ui::Ref<Button> clear;

  auto scene = ui::scene(ui::vbox(
    ui::label("Quick note"),
    ui::lineEdit(draft, "Remember the milk"),
    ui::label(saved, "Nothing saved yet"),
    ui::button(save, "Save"),
    ui::button(clear, "Clear draft"),
    ui::label("Tab: focus | F2: save | F3: clear | F10: quit")));

  auto saveNote = [draft, saved] {
    saved.text() = draft.text();
  };

  auto clearDraft = [draft] {
    draft.text() = "";
  };

  return scene
    | ui::theme(nordTheme())
    | ui::style("Button { min-width: 16; bold: true; }")
    | ui::onClick(save, saveNote)
    | ui::onClick(clear, clearDraft)
    | ui::onKey("F2", saveNote)
    | ui::onKey("F3", clearDraft)
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

</details>

После сборки той же командой `./build/quick_note` запускает эту версию с темой и оформлением кнопок.

## Результат

![Форма заметки после Save: текст Remember the milk в поле и сохранённой строке, кнопки Save и Clear draft.](../images/first-application.png)

На изображении — вывод итоговой программы в терминале в памяти размером
64 × 10 ячеек после **Save**, с темой Nord. Он отображён через xterm.js;
шрифт и цвета в вашем терминале могут отличаться.

Если приложение не запустилось, `ui::runApp()` сообщает об ошибке в stderr
после попытки восстановления терминала и возвращает ненулевой код.
Создание описания перед `runApp()` подчиняется обычным правилам исключений C++;
для включения этого шага в границу обработки ошибок есть
[фабричная перегрузка](declarative-ui.md).
[Диагностика](diagnostics.md) объясняет стандартный и пользовательский отчёт.

Дальше можно изучить [общее состояние](property.md) и
[размещение контролов](layout.md). Другие практические задачи собраны в
[руководствах](guides.md).
