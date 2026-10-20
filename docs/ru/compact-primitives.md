# Короткие рамки, индикаторы и размеры

[Английская версия](../compact-primitives.md)

Небольшая retained-сцена собирается стандартными виджетами. Кнопка использует
существующий Button/action/router path без обязательных brackets; её активируют
Enter, Space и левый клик. F10 также завершает приложение. Пользовательский
подкласс Widget и цикл опроса не нужны.

## Обычная композиция

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  auto content = ui::vbox(ui::label("Transfer"), ui::gauge(.65).width(24), ui::plainButton("Exit", [] { ui::quit(); }));
  auto scene = ui::scene(ui::hbox(ui::border(std::move(content)).width(30).height(7)));

  scene.onKey(key::F10, ui::quit);

  return scene.runApp();
}
```

## Pipe-композиция

```cpp
#include <portals/ui.hpp>

int main() {
  using namespace portals;

  auto scene = ui::scene(ui::hbox(ui::vbox(ui::label("Transfer"), ui::gauge(.65).width(24),
                                        ui::plainButton("Exit", [] { ui::quit(); }))
                                  | ui::border()
                                  | ui::width(30)
                                  | ui::height(7)));

  return scene
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
}
```

## Размеры и flex

`width(cells)` и `height(cells)` заменяют только выбранную ось фиксированными
неотрицательными границами. `flexX(weight)`, `flexY(weight)` и `flex(weight)`
включают существующий flex-режим с положительным весом и нулевой preferred-базой,
сохраняя minimum/maximum. Так область делится по весам даже для дочерних виджетов,
которые измеряют весь предложенный viewport.
Модификаторы выполняются по порядку при mount: `width(10).flexX()` сохраняет
предел десять ячеек; `flex().height(3)` оставляет горизонтальный flex и фиксирует
высоту. Отрицательный размер и нулевой вес возвращают `ManagedError` с
`TreeError::INVALID_ARGUMENT`; созданное поддерево удаляется, refs не публикуются.
Операции доступны как consuming-методы и pipe-стадии. Политики применяет родительский
layout; корень приложения получает размер viewport.

## Контракты рамки и индикатора

`border(child, options, style)` и alias `frame` владеют одним consuming-рецептом.
Они монтируются в одной транзакции с дочерними refs и ошибками. Отступ в одну
ячейку измеряет и размещает существующий LinearLayout. Внутренний clip ограничивает
отрисовку и hit testing даже при переполнении fixed/minimum-size дочернего элемента.
Пустая область ничего не рисует. Одноячеечные строки/столбцы сворачиваются в углы
и оставшийся край; рамка два на два содержит четыре угла без содержимого.
Рамка не заполняет и не закрашивает внутреннюю область.

`gauge(value, options, style, policy)` — строго determinate-индикатор. По умолчанию
диапазон `[0, 1]`, intrinsic-ширина десять ячеек и высота одна. Границы конечны,
`minimum < maximum`, их разность конечна и представима. Значение конечно и
входит во включительный диапазон. Недопустимое значение, NaN/бесконечность,
отрицательная preferred-ширина и нулевой paint cap дают `INVALID_ARGUMENT`.
Заполнение занимает `floor(width * fraction)` столбцов по всей выделенной высоте.

Между кадрами используйте `setGaugeValue(tree, node, value)`: операция проверяет
аргумент до изменения состояния, запрашивает только paint, а для равного значения
не запрашивает redraw. В уже dirty `onPrepare` callback метод `Gauge::display(value)`
обновляет snapshot без запроса следующего кадра. Таймер и анимация не создаются.

`BorderOptions` и `GaugeOptions` выбирают Unicode/ASCII glyphs и положительный
`maxPaintCells` (по умолчанию 16384). Декорация владеет GraphemeText с полной
WidthPolicy принимающей сцены, включая revision/emoji/ambiguous. Unicode-декорация
шире одной ячейки заменяется ASCII. До submit рамка проверяет выделенный периметр,
индикатор — площадь. Превышение cap даёт `WOULD_BLOCK`, даже если область частично
обрезана; повышайте cap лишь вместе с достаточными render budgets. Полностью
обрезанные узлы пропускаются существующим tree path. Сама реализация paint виджета
не выделяет heap-память. Submitted text принадлежит виджету до конца enclosing
frame и потребляется renderer до завершения этого frame.

Переданный `ControlStyle` владеет foreground/background и остальными значениями
normal/focused/disabled. Это manual defaults ниже theme rules, а не явное
переопределение темы. У Border/Gauge эти defaults задаются только при construction.
Динамические цвета меняет существующий style resolver, включая exact-Ref application
styles: computed cache и приоритет темы остаются согласованными. Border/Gauge используют существующий style resolver и
синхронизацию disabled ancestors. `plainButton` возвращает тот же `Button`, что и
`button`: сохраняются refs, bindings, keyboard и pointer routing; убраны только
обязательные brackets. Обычный `button` сохраняет прежние brackets по умолчанию. Обе фабрики отображают
лимиты caption в `NODE_LIMIT`, некорректный UTF-8 — в `INVALID_ARGUMENT` и сохраняют
исходный `TextError` вместе с transactional cleanup.

## Low-level расширение и совместимость

Семантика трёхаргументного `WidgetTree::arrange` сохранена. Новая перегрузка с
четырьмя аргументами пересекает дополнительный surface `contentClip` с обычным
inherited/root clip узла; descendants наследуют итоговый geometry clip.
`Layout::childClip` по умолчанию возвращает полную выделенную область, поэтому
Row/Column/Grid, Overlay и пользовательские layouts сохраняют размещение и clipping.
Border использует hook для внутренней области. Недопустимый custom clip и
переполнение transform дают типизированные geometry errors; неуспешный pass отбрасывается.

Новый virtual hook и хранение Button требуют пересборки SDK и consumers.
Совместимость исходников сохранена; совместимость со старыми object files не обещается.
См. [декларативную композицию](declarative-ui.md) и [layout policies](layout.md).

## Стиль приложения для точного Ref

Полный пример задаёт владеющие значения по умолчанию в конструкторе и стиль
приложения для конкретного Ref. `ui::style(ref, pss)` копирует исходный текст до
отложенного mount; его слой приложения перекрывает тему и сохраняется при её
замене. Селектор `*` действует только на указанный узел благодаря точной области
слоя. Область не меняет правила сопоставления: управляемая кнопка автоматически
получает тип `Button` и также совпадает с селекторами `Control`. Явные метаданные
описаны в [контракте стилевой цели](styles.md).
При ошибке фабрики не публикуются ни Ref, ни частичный scoped-стиль; тот же Ref
и владеющую стадию стиля можно использовать в новой сцене.

```cpp
#include <portals/ui_style.hpp>

int main() {
  using namespace portals;

  ui::Ref<Button> exit;
  ControlStyle defaults;
  defaults.normal.foreground = RgbColor{180, 180, 180};

  auto scene = ui::scene(ui::border(ui::vbox(
    ui::gauge(.65).width(24), ui::plainButton("Exit", [] { ui::quit(); }, defaults).reference(exit))));

  scene
    | ui::style(exit, "* { foreground: #ffffff; background: #203040; }");

  return scene
    | ui::onKey(key::F10, ui::quit)
    | ui::runApp();
}
```
