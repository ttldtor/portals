# Сборка Portals и примеров {#getting-started-ru}

[Подробная английская версия](../getting-started.md)

Эта страница описывает сборку исходников Portals, проверку библиотеки
и запуск примеров из её репозитория. Для создания своего приложения
используйте [краткое руководство](index.md), для подключения установленной
библиотеки — [пакет CMake](package.md).

## Сборка из исходников

Потребуются CMake 3.24+, Ninja, компилятор C99 для встроенной зависимости
utf8proc и компилятор C++23 со стандартной библиотекой, содержащей `<expected>`
и `<format>`. Зависимости уже входят в исходники.

В Windows откройте x64 Developer PowerShell для Visual Studio с установленным
компонентом разработки настольных приложений C++:

```powershell
cmake --preset windows-msvc-debug
cmake --build --preset windows-msvc-debug
ctest --preset windows-msvc-debug
```

В Linux используйте GCC с подходящей стандартной библиотекой:

```sh
cmake --preset linux-gcc-debug
cmake --build --preset linux-gcc-debug
ctest --preset linux-gcc-debug
```

Предустановка использует `g++` из PATH. При необходимости укажите версию явно:
`cmake --preset linux-gcc-debug -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14`.
Сборки Debug размещаются в `build/<preset>`. Отдельная сборка Release:

```powershell
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure --no-tests=error
```

Эти команды Release подходят и для Linux. Архитектура, компилятор, стандартная
библиотека и среда выполнения должны быть совместимы с приложением.
`PORTALS_BUILD_EXAMPLES`, `PORTALS_BUILD_TESTS` и `PORTALS_BUILD_TOOLS` по умолчанию
включены при самостоятельной сборке и выключены при подключении через
`add_subdirectory`.

## Запуск примеров

Запускайте программы из терминала с подключёнными собственными потоками ввода
и вывода:

```powershell
./build/windows-msvc-debug/examples/gallery/portals_gallery
./build/windows-msvc-debug/examples/dashboard/portals_dashboard --rate 10000 --fps 30 --rows 1000000
./build/windows-msvc-debug/examples/custom_widget/portals_custom_widget
./build/windows-msvc-debug/examples/editor_history/portals_editor_history
./build/windows-msvc-debug/examples/click_handlers/portals_click_handlers
```

В Linux замените каталог на `build/linux-gcc-debug`. F10 или Ctrl+C завершает
работу. В галерее Tab меняет фокус, Enter подтверждает ввод или действие,
F2 меняет тему. В dashboard стрелки, Home и End перемещают выделение таблицы;
Корректные изменения заметки применяются сразу. В custom widget стрелки Left/Right меняют уровень.
Полный список параметров находится на странице [примеров](examples.md).

Галерея принимает `--theme NAME` (или `-t NAME`), `--ansi16` и `--help`.
Объявление Lyra разбирается до открытия терминала: справка возвращает 0,
неверные параметры — 2. Публичные программы не имеют режима `--check`.

Конечные проверки находятся в отдельных тестовых программах и потребителе
установленного пакета. Терминал в памяти и логические часы позволяют завершать
их автоматически. Успех не подтверждает внешний вид или восстановление
настоящего терминала.

## Собственное приложение

Используйте декларативный пример из [короткого руководства](index.md).
`runApp` запускает сцену, выполняет очистку и сообщает об ошибке. Создание
описания перед заключительным этапом `runApp` подчиняется обычным правилам
исключений C++. Перегрузка `ui::runApp(factory, setup)` включает создание
описания в проверяемую границу. Подробности — в [декларативном API](declarative-ui.md).

Для встраивания и собственной обработки результата используйте `run()`:
он возвращает `expected` со статистикой или ошибкой.
Пример [функции галереи](../../examples/gallery/fluent_form.hpp) показывает форму
`gallery::form(ui::opts(options))`. Настройки остаются во владении вызывающего кода
и должны пережить синхронный `ui::runApp(argc, argv, ui::lyra::cli(parser))`.
Разбор выполняется до монтажа; [командная строка](cli.md) описывает ошибки, справку,
версию и снимки аргументов/окружения. Пример подключает `portals::lyra`.
[custom_widget/meter.hpp](../../examples/custom_widget/meter.hpp) показывает собственный
виджет с неизменяемой моделью и подготовкой отображения.

## Диагностика действия

```text
portals_dashboard_check --rate 0 --trace trace.json
```

Команда запускает тестовую диагностическую программу, собранную с `PORTALS_BUILD_TESTS=ON`.
Переведите фокус на Inspect и нажмите Enter. Обработчик присвоит заметке её
текущее значение. После выхода трасса различает `handler_returned`,
`value_unchanged` и `frame_not_requested`. Ошибки записи файла сообщаются
отдельно после восстановления терминала. Правила интерпретации и сведения
о потерях находятся в [справке по диагностике](diagnostics.md).

## Установка и HTML-справка

Исходники подключаются через `add_subdirectory` и `portals::core`.
Установленный пакет подключается через `find_package(Portals CONFIG REQUIRED)`.
Команды установки и проверки переноса находятся в [справке по пакету](package.md).

Для сборки HTML нужны Doxygen 1.18+ и Mermaid CLI. Проверенная связка для схем:
Node.js 24 LTS и Mermaid CLI 12:

```powershell
npm.cmd install --global @mermaid-js/mermaid-cli
mmdc.cmd --version
```

При первой установке Mermaid CLI обычно загружает браузер Puppeteer.
Затем схемы строятся локально. Для обычной сборки библиотеки эти инструменты
не нужны. Когда `doxygen` и `mmdc` доступны в PATH:

```powershell
cmake -S . -B build/docs -G Ninja -DPORTALS_BUILD_DOCS=ON
cmake --build build/docs --target portals_docs
```

Если Mermaid находится вне PATH, задайте
`-DPORTALS_MERMAID_EXECUTABLE=C:/path/to/mmdc.cmd`; в POSIX укажите исполняемый
файл `mmdc`. Откройте `build/docs/docs/html/index.html`. HTML содержит исходники
примеров, SVG-схемы и лицензии; его можно читать без сети и генераторов.
Для проверки ссылок после изменения документации используйте
`python tools/docs/check_links.py build/docs/docs/html`.

Первичные источники: [предустановки CMake](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html),
[режим сборки CMake](https://cmake.org/cmake/help/latest/manual/cmake.1.html),
[настройки Doxygen](https://www.doxygen.nl/manual/config.html).
