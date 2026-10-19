# Выбор и обновление темы {#themes-guide-ru}

[Английская версия](../themes-guide.md)

[Тема](glossary.md) задаёт правила оформления и палитру. Вы можете установить
её с помощью `ui::theme`:

```cpp
#include <portals/ui.hpp>
#include <portals/ui_style.hpp>

int main() {
  using namespace portals;

  return ui::scene(ui::vbox(
    ui::label("F10: quit"),
    ui::button("Save")))
    | ui::theme(nordTheme())
    | ui::onKey("F10", ui::quit)
    | ui::runApp();
}
```

Сборка клиента описана в [кратком руководстве](index.md) и
[настройке пакета](package.md). `runApp` пытается восстановить терминал
и сообщает об ошибках. Проверяемое создание сцены описано в
[декларативном API](declarative-ui.md).
Далее фрагменты предполагают открытую [сцену](glossary.md#glossary-scene-ru) `ui::Scene scene` между кадрами,
`using namespace portals;` и заголовок `<portals/ui_style.hpp>`.

## Встроенные палитры

| Фабрика | Оформление |
|---|---|
| `modernTheme()` | Современная тёмная палитра |
| `turboTheme()` | Палитра в стиле Turbo |
| `nordTheme()` | Тёмная Nord |
| `gruvboxTheme()` | Тёмная Gruvbox |
| `tokyoNightTheme()` | Тёмная Tokyo Night |
| `catppuccinLatteTheme()` | Светлая Catppuccin Latte |

Фабрики возвращают таблицу `StyleSheet`, которая [хранит свои данные](glossary.md#glossary-ownership-ru),
и могут выделять память.
Ошибки установки доступны через `scene.checked()` и запуск приложения.
Менять тему разрешено между кадрами в UI-потоке.

## Собственная палитра

`ThemeTokens` содержит шесть RGB-значений:

```cpp
scene
  | ui::theme(themeFromTokens(ThemeTokens{
    .foreground = RgbColor{220, 225, 232},
    .background = RgbColor{24, 28, 36},
    .accent = RgbColor{75, 180, 220},
    .muted = RgbColor{125, 135, 150},
    .selection = RgbColor{45, 75, 110},
    .heading = RgbColor{255, 200, 85}
  }));
```

| Параметр | Назначение |
|---|---|
| `foreground` | Обычный и выделенный текст |
| `background` | Обычный фон, текст в фокусе и фон курсора |
| `accent` | Фон в фокусе и текст курсора |
| `muted` | Текст отключённого контрола |
| `selection` | Фон выделенной строки или диапазона редактора |
| `heading` | Текст заголовка |

Встроенные правила запрашивают полужирное начертание для фокуса, выделения
и заголовков. Курсор меняет местами цвета поля в фокусе. Дополнительные
варианты оформления контролов можно задать правилами PSS. PSS v0 не поддерживает
обращение к этим C++-параметрам по имени.

## Прикладные стили при смене темы

```cpp
scene
  | ui::style("Button { min-width: 12; }", {.name = "layout"})
  | ui::style("Label { bold: true; }",
    {.tier = StyleTier::USER, .name = "contrast"})
  | ui::theme(gruvboxTheme());
```

Смена темы обновляет `THEME`. Слои `APPLICATION` и `USER` сохраняются выше неё.
Если прикладной слой задаёт цвет текста, он продолжит иметь приоритет над
цветом темы. Подробнее — [приоритеты стилей](styling-guide.md) и
[контракты слоёв](style-layers.md).

## Правила палитры в файле

В репозитории есть `themes/modern.pss` и `themes/turbo.pss`, соответствующие
фабрикам C++. Их правила можно загрузить в прикладной слой:

```cpp
scene
  | ui::style(ui::file("themes/modern.pss"), {.name = "palette-file"});
```

Стадии источников стилей принимают `APPLICATION` и `USER`. Палитра в этих слоях
может переопределить цвета из `ui::theme`. Существующие фабрики и
`ui::theme(StyleSheet)` заменяют зарезервированный слот `THEME`;
прямая загрузка PSS-файла темы — **TBD — M5**. Автоматическая перезагрузка
и подстановка переменных в PSS v0 пока отсутствуют. Загрузка файлов описана
в [руководстве по стилям](styling-guide.md).

## Изменение типизированного цвета во время работы

Экспериментальное описание C++ поддерживает именованные параметры `RgbColor`,
`bool` и `std::int32_t`. Этот фрагмент объявляет динамический цвет акцента:

```cpp
namespace sc = portals::style::ct;
constexpr auto THEME = sc::sheet(
  sc::parameters(sc::parameter<"accent">(sc::rgb(75, 180, 220))),
  sc::rule(sc::selector<"Button">(),
           sc::foreground(sc::token<"accent">())),
  sc::rule(sc::selector<"Button">(StyleState::FOCUSED),
           sc::background(sc::mix(sc::token<"accent">(),
                                  sc::rgb(24, 28, 36), 50))));

auto stage = ui::liveTheme(THEME);
auto colors = stage.ref();
scene
  | stage;
```

[Стадия UI](glossary.md#glossary-ui-stage-ru) — объект настройки,
возвращаемый `ui::liveTheme(THEME)`. В примере он сохранён в переменной `stage`.
Выражение `scene | stage` подключает контроллер.

После успешного подключения обработчик события в UI-потоке может обновить цвет:

```cpp
auto changed = colors.set<"accent">(sc::rgb(255, 200, 85));
```

Результат показывает, изменилось ли значение: `true` означает изменение,
`false` — равенство значений. При ошибке прежние значения и стили сохраняются.
`mix` принимает процент от 0 до 100.
Атомарное пакетное обновление, проверка точных типов и время жизни контроллера
описаны в [стилях времени компиляции](compile-time-styles.md).

Сцена владеет контроллером; `colors` наблюдает за ним без продления времени жизни.
Чтение и обновление требуют UI-потока. Закрытая сцена или сменившийся контроллер
темы дают типизированную ошибку. Для нового контроллера требуется новая стадия.

## Цвета и начертания в терминале

Режим true color сохраняет RGB-значения. ANSI16 переводит их в меньшую палитру
и может свести близкие цвета к одному. Полужирное, курсивное и подчёркнутое
начертания зависят от поддержки терминала.

## Планируемая поддержка тем

Единый PSS-источник тем и расширенные параметры — **TBD — M5**.
Импорт внешних форматов — **TBD**, майлстоун ещё не назначен.
Подробности — в [планируемых возможностях](planned-features.md#planned-themes-ru).
