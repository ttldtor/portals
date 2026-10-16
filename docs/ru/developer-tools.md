# Инструменты разработчика {#developer-tools-ru}

[Английская версия](../developer-tools.md)

Выберите команду по нужному результату. Если раздел не указывает иное, выполняйте
команды из корня репозитория. Здесь описаны инструменты текущей версии исходников;
в установленном SDK и справке последнего релиза набор может быть старее.

| Задача | Точка запуска | Справка |
| --- | --- | --- |
| Собрать библиотеку и запустить тесты | CMake и CTest | [Сборка и проверки](developer-tools-build.md) |
| Проверить имена | `python tools/check_naming.py` | [Именование](developer-tools-build.md#tool-naming-ru) |
| Выбрать и проверить набор CI | `python tools/ci/checks.py` | [Выбор CI](developer-tools-build.md#tool-ci-ru) |
| Собрать и проверить HTML-справку | `portals_docs`, затем `check_links.py` | [Документация](developer-tools-build.md#tool-documentation-ru) |
| Обработать HTML и раскрасить CMake | `tools/docs/finalize_html.py` | [Обработчик HTML](developer-tools-build.md#tool-html-processor-ru) |
| Измерить изменения исходников | `portals_compactness` | [Измерения](developer-tools-measure.md#tool-compactness-ru) |
| Измерить затраты выполнения | Пять программ `portals_*_benchmark` | [Бенчмарки](developer-tools-measure.md#tool-benchmarks-ru) |
| Сохранить и сравнить измерения | `tools/quality/performance.cmake` | [Результаты производительности](developer-tools-measure.md#tool-performance-ru) |
| Собрать наблюдения Clang-tidy | `tools/quality/readability.cmake` | [Читаемость](developer-tools-measure.md#tool-readability-ru) |
| Исследовать терминал | `portals_terminal_probe` | [Диагностика терминала](developer-tools-measure.md#tool-probe-ru) |
| Проверить восстановление native-терминала | `native_windows.ps1` или `native_xterm.sh` | [Native-проверки](developer-tools-measure.md#tool-native-ru) |
| Собрать переносимый комплект | `tools/package/bundle.ps1` или `bundle.cmake` | [Упаковка](developer-tools-release.md) |
| Запустить пример из комплекта | Входящие в комплект `run.ps1` / `run.sh` | [Скрипты запуска](developer-tools-release.md) |
| Подготовить PR релиза | `tools/release/start.ps1` или `start.py` | [Подготовка релиза](developer-tools-release.md) |
| Собрать и опубликовать файлы релиза | Workflow релиза | [Помощники CI](developer-tools-release.md) |

## Как читать команды

Примеры PowerShell используют пути относительно рабочей копии. Заключайте весь
аргумент `-DNAME=value` в кавычки, если значение содержит пробелы или точки с запятой.
Аргументы CMake-скрипта указываются перед `-P script.cmake`. Для генератора с
несколькими конфигурациями добавьте к сборке `--config Release` и используйте
исполняемый файл из каталога `Release`. В примерах измерений выбраны Ninja Release
и отдельный каталог `build/tools`.

Если программа поддерживает `--help`, эта команда показывает принимаемые аргументы.
У бенчмарков и shell-скриптов используйте отдельный `--help`, у PowerShell —
`Get-Help`, у прямых CMake-скриптов — `-DHELP=ON`. Неудачная проверка возвращает ненулевой код. Конкретные числа указаны
там, где реализация их определяет; для ошибок CMake и PowerShell не обещается
один переносимый числовой код.

## Встроенная справка

Справка хранится рядом с реализацией и доступна до передачи обязательных входных
данных. Команды ниже показывают помощь без запуска основной работы:

```powershell
python tools/docs/finalize_html.py --help
python tools/ci/checks.py select --help
Get-Help ./tools/native_windows.ps1 -Full
cmake -DHELP=ON -P tools/quality/performance.cmake
./build/tools/portals_mvp_benchmark.exe --help
```

## Все сопровождаемые скрипты

Для поиска по имени файла приведён полный перечень скриптов `tools/` и
`cmake/`. Исходник содержит встроенную справку либо описание внутреннего
помощника. Подробные параметры находятся в связанном справочнике.

| Файл | Раздел |
| --- | --- |
| [cmake/FinalizeDiagrams.cmake](../../cmake/FinalizeDiagrams.cmake) | [Внутренний помощник](developer-tools-build.md) |
| [cmake/FinalizeDocumentation.cmake](../../cmake/FinalizeDocumentation.cmake) | [Внутренний помощник](developer-tools-build.md) |
| [cmake/TestProcesses.cmake](../../cmake/TestProcesses.cmake) | [Внутренний помощник](developer-tools-build.md) |
| [tools/benchmarks/check.cmake](../../tools/benchmarks/check.cmake) | [Измерения](developer-tools-measure.md) |
| [tools/benchmarks/check_managed.cmake](../../tools/benchmarks/check_managed.cmake) | [Измерения](developer-tools-measure.md) |
| [tools/check_naming.py](../../tools/check_naming.py) | [Сборка и проверки](developer-tools-build.md) |
| [tools/ci/checks.py](../../tools/ci/checks.py) | [Сборка и проверки](developer-tools-build.md) |
| [tools/ci/test_checks.py](../../tools/ci/test_checks.py) | [Регрессионный тест](developer-tools-build.md) |
| [tools/docs/check_links.py](../../tools/docs/check_links.py) | [Сборка и проверки](developer-tools-build.md) |
| [tools/docs/architecture.py](../../tools/docs/architecture.py) | [Exports архитектуры](developer-tools-build.md#tool-architecture-ru) |
| [tools/docs/architecture_test.py](../../tools/docs/architecture_test.py) | [Регрессионная проверка](developer-tools-build.md#tool-architecture-ru) |
| [tools/docs/finalize_html.py](../../tools/docs/finalize_html.py) | [Сборка и проверки](developer-tools-build.md) |
| [tools/docs/test_cli_help.py](../../tools/docs/test_cli_help.py) | [Регрессионный тест](developer-tools-build.md) |
| [tools/docs/test_finalize_html.py](../../tools/docs/test_finalize_html.py) | [Регрессионный тест](developer-tools-build.md) |
| [tools/native_windows.ps1](../../tools/native_windows.ps1) | [Сборка и проверки](developer-tools-build.md) |
| [tools/native_xterm.sh](../../tools/native_xterm.sh) | [Сборка и проверки](developer-tools-build.md) |
| [tools/package/bundle.cmake](../../tools/package/bundle.cmake) | [Упаковка и выпуск](developer-tools-release.md) |
| [tools/package/bundle.ps1](../../tools/package/bundle.ps1) | [Упаковка и выпуск](developer-tools-release.md) |
| [tools/package/common.cmake](../../tools/package/common.cmake) | [Внутренний помощник](developer-tools-release.md) |
| [tools/package/run.ps1](../../tools/package/run.ps1) | [Упаковка и выпуск](developer-tools-release.md) |
| [tools/package/run.sh](../../tools/package/run.sh) | [Упаковка и выпуск](developer-tools-release.md) |
| [tools/quality/performance.cmake](../../tools/quality/performance.cmake) | [Измерения](developer-tools-measure.md) |
| [tools/quality/readability.cmake](../../tools/quality/readability.cmake) | [Измерения](developer-tools-measure.md) |
| [tools/release/metadata.py](../../tools/release/metadata.py) | [Упаковка и выпуск](developer-tools-release.md) |
| [tools/release/package.cmake](../../tools/release/package.cmake) | [Упаковка и выпуск](developer-tools-release.md) |
| [tools/release/publish.py](../../tools/release/publish.py) | [Упаковка и выпуск](developer-tools-release.md) |
| [tools/release/start.ps1](../../tools/release/start.ps1) | [Упаковка и выпуск](developer-tools-release.md) |
| [tools/release/start.py](../../tools/release/start.py) | [Упаковка и выпуск](developer-tools-release.md) |

## Внутренние помощники и тесты

В `tools/` находятся и прямые команды, и вспомогательный код. Общие CMake-модули,
обработчики HTML и помощники публикации выделены в соответствующих справочниках.
Используйте указанную цель сборки или workflow: они передают нужные входные данные.

`tests/*.cmake`, `tests/release_*.py` и скрипты проверки потребителя относятся
к реализации тестов. Запускайте их через CTest или указанную команду unittest.
`tools/ci/test_checks.py` и `tools/docs/test_finalize_html.py` проверяют свои
помощники; команды приведены в справке по сборке и CI.

[Тестирование (на английском)](../testing.md) описывает replay-сценарии, внедрение
ошибок и контракт native-supervisor. [Фаззинг (на английском)](../../fuzz/README.md)
описывает тестовые программы для запуска с санитайзерами.

## Скрипты локального рабочего каталога

Игнорируемый Git каталог `.work/` содержит личные рабочие материалы. Он не входит
в архивы исходников и SDK. В `logs/`, `tmp/`, `artifacts/`, `experiments/` и
`worktrees/` могут находиться сгенерированные драйверы, копии инструментов и зависимости.
Количество таких файлов не определяет число поддерживаемых утилит.

Исторический скрипт из логов может содержать абсолютный путь и предположения одного
эксперимента. Храните его вместе с результатом исходной задачи. Переиспользуемый
локальный помощник должен иметь запись в частном каталоге инструментов: владелец,
требования, входные данные, результат и ограничения измерений. Перед включением
в CI перенесите его в поддерживаемые исходники, добавьте проверки и справку.
При уборке рабочего каталога сохраняйте неопубликованные изменения и доказательства
активных задач.

## Сопровождение

При изменении утилиты обновляйте таблицу параметров, команды и страницы EN/RU вместе.
Указывайте значения по умолчанию, единицы измерения, необходимые файлы, расположение
результата, коды завершения и изменения файлов, терминала или удалённого сервиса.
Добавляйте новые точки запуска в этот каталог. Трассы и результаты отдельных
прогонов храните вне руководств.
