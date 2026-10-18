# Браузерные Gallery и Level {#wasm-gallery-ru}

[Английская версия](../wasm-gallery.md) · [Примеры](examples.md) · [Инструменты разработчика](developer-tools.md)

Этот подключаемый экспериментальный пример запускает Portals через WebAssembly и
xterm.js. В Gallery есть поле ввода, checkbox, темы и конечная анимация; Level
получает наклон устройства из Telegram и рисует уровень с калибровкой.
Обе сцены используют существующий fluent API. Нативные примеры сохраняют
синхронный runner.

WASM необязателен: `PORTALS_BUILD_WASM_GALLERY` по умолчанию равен `OFF`.
Существующим нативным presets и потребителям пакета не нужны Emscripten, Node.js,
npm или xterm.js. Команды ниже относятся только к сборке браузерного примера.

Источником текущей сборки служит код этого checkout. Отдельно опубликованный POC
остаётся исторической сборкой на базе v0.3.0 и не обновляется автоматически вслед
за этой веткой. Браузерная симуляция не квалифицирует физические устройства Telegram.

## Сборка runtime

Нужны CMake, Ninja и Emscripten **6.0.12**, активированное окружение SDK и переменная
`EMSDK`, указывающая на его корень. Для host-инструментов используется Node.js **24**.
Из корня репозитория:

```powershell
cmake --preset wasm-debug
cmake --build --preset wasm-debug
cmake --build build/wasm-debug --target portals_wasm_sdk_smoke
node build/wasm-debug/examples/wasm_gallery/portals_wasm_sdk_smoke.js
```

Для Release замените `wasm-debug` на `wasm-release`. Пример включается через
`PORTALS_BUILD_WASM_GALLERY`; обычная нативная сборка его не включает.
Эти presets используют C++23 и native Wasm exceptions, без Asyncify, JSPI и pthreads.
Нативные терминальные примеры и нативные тестовые программы в них не собираются.

Gallery резервирует фиксированный Wasm stack размером 1 МиБ: стандартных
64 КиБ в Emscripten 6.0.12 недостаточно для неоптимизированного mount сцены.
Бюджет в байтах задаётся через `cmake --preset wasm-debug -DPORTALS_WASM_STACK_SIZE=1048576`.
Рост heap не увеличивает этот stack. В Debug включена проверка указателя
`STACK_OVERFLOW_CHECK=2`; см. описание [размера stack](https://emscripten.org/docs/tools_reference/settings_reference.html#stack-size)
и [проверок переполнения](https://emscripten.org/docs/tools_reference/settings_reference.html#stack-overflow-check) Emscripten.

Соберите статический host рядом с runtime:

```powershell
Push-Location examples/wasm_gallery/web
npm ci
npm run build -- ../../../build/wasm-debug/wasm-gallery
npm run build -- --help
Pop-Location
```

В каталоге результата будут Emscripten loader и WASM, собранные JS/CSS host, HTML,
source maps, лицензии и хеши артефактов в `provenance.json`. Сборщик host требует
готовые runtime-файлы и сохраняет их. Cache keys зависят от содержимого; loader и
WASM используют одну версию runtime. Для локального просмотра нужен HTTP-сервер,
для Mini App — HTTPS. Открытие HTML напрямую через `file:` не поддерживается.

Для локального просмотра можно использовать стандартный HTTP-сервер Python;
Python нужен только для этой команды просмотра:

```powershell
python -m http.server 8000 --bind 127.0.0.1 --directory build/wasm-debug/wasm-gallery
```

Откройте `http://127.0.0.1:8000/`; после просмотра остановите сервер через Ctrl+C.
Тот же каталог результата можно разместить на статическом HTTPS-хостинге;
серверная часть приложению не нужна. WASM workflow собирает и проверяет артефакты,
но не публикует их.

## Использование сцен

Выберите **Gallery** или **Level** над терминалом. Переключение завершает прежнюю
сцену и создаёт новую, сохраняя тему. **Quit**, F10 или Ctrl+C останавливает текущую
сцену; переключатель остаётся доступным. Theme и F2 меняют темы.
Tab и Shift+Tab обходят контролы терминала, когда фокус находится внутри xterm.
Браузер может перехватывать функциональные клавиши.

**Run something** в Gallery отключает свою кнопку на один проход вперёд и назад,
меняя выделенную букву каждые 300 мс. Ввод и другие действия остаются доступны.
Минимальные цели размещения — 22 колонки/11 строк для Gallery и 25 колонок/19 строк
для Level. Шрифт уменьшается не ниже 10px; при меньшем viewport возможно обрезание.

Мышь передаёт координаты ячейки по SGR. Одиночное касание пальцем или пером до
500 мс, со смещением не более 10 CSS pixels и завершением в той же ячейке, использует
тот же путь ввода. Свайпы, долгие нажатия и несколько пальцев не активируют контрол.
В Level терминальный ввод read-only, при возврате в Gallery ввод восстанавливается.
Поведение экранной клавиатуры требует проверки на физическом устройстве.

Level использует Telegram DeviceOrientation в клиентах с поддержкой Mini Apps 8.0+.
Он запрашивает обновления каждые 50 мс, учитывает поворот экрана на четверть оборота,
применяет low-pass фильтр и мёртвую зону 0,7°, ограничивает положение пузырька ±30°.
**Zero** принимает текущую позу за начало отсчёта. До корректного показания Zero
отключена; отсутствие API явно отображается. Browser sensor fallback отсутствует.
Пример не является калиброванным измерительным прибором.

## Runtime и владение

`Application::start()` и `Application::step()` предоставляют пошаговое выполнение
через `ApplicationReadyTerminal`, чьи операции ввода/вывода не ожидают готовности.
Managed и fluent сцены предоставляют соответствующий lifecycle. Синхронный `run()`
использует то же ядро через прежний deadline transport.

Host вызывает один ограниченный шаг через `requestAnimationFrame`, не запуская
блокирующий цикл в потоке браузера. Сцена, захваченное ею состояние и ready transport
должны переживать все активные шаги. Пример уничтожает сцену до транспорта, отменяет
таймер Gallery при остановке и копирует принятый вывод во владеющее JS-хранилище
до завершения C++. Нельзя уничтожать runtime из его host output callback.

Поколения сцен отклоняют sensor callbacks от прежней сцены. Hidden/inactive
состояние приостанавливает шаги и освобождает датчик. Adapter допускает один
ожидающий Telegram start и последовательно обрабатывает подтверждения остановки.
Новая сессия ждёт завершения предыдущего stop callback и рассылки его события.
Если start или stop не завершится, следующая сцена останется waiting.
У native global sensor events нет request ID, поэтому произвольные запоздалые
native events между перезапусками невозможно полностью различить. Page hide и
фатальные ошибки освобождают host listeners и runtime.

Transport и экспорты расположены в
[browser_terminal.hpp](../../examples/wasm_gallery/browser_terminal.hpp) и
[main.cpp](../../examples/wasm_gallery/main.cpp). Сцены описаны в
[demo_scene.hpp](../../examples/wasm_gallery/demo_scene.hpp) и
[level_scene.hpp](../../examples/wasm_gallery/level_scene.hpp).

## Проверка сборки

После сборки runtime и host:

```powershell
Push-Location examples/wasm_gallery/web
npm test
npx playwright install chromium
$env:PORTALS_WASM_DIR = (Resolve-Path ../../../build/wasm-debug/wasm-gallery).Path
npm run test:browser
Pop-Location
```

`PORTALS_WASM_DIR` выбирает каталог результата; `PORTALS_WASM_PORT` по умолчанию 41813.
Playwright запускает и останавливает свой сервер, проверяя относительные пути
артефактов под URL-префиксом проекта. Unit-тесты проверяют output backpressure,
touch и sensor leases; браузерные тесты используют настоящий WASM с симуляцией SDK.
Физические Desktop/Android/iOS, IME и датчик квалифицируются отдельно. Интеграция
runtime также требует нативной матрицы и package-consumer проверок, перечисленных
в [руководстве разработчика](development.md).
