# Установленный пакет CMake {#package-ru}

[Подробная английская версия](../package.md)

Portals устанавливает открытые заголовки C++23, статические архивы core
и встроенной utf8proc, CMake configuration/version/targets и лицензии.
`portals::core` передаёт требование C++23 и полную цепочку линковки.
В UNIX конфигурация находит платформенную Threads. Загрузка зависимостей
по сети и системная utf8proc не требуются.
Версию задаёт project(Portals VERSION ...) в CMake; метаданные и проверки
потребителя сверяются с версией связанной библиотеки.

## Установка и подключение

В x64 Developer PowerShell MSVC или Linux с подходящим набором инструментов сборки C/C++:

```powershell
cmake -S . -B build/package -G Ninja -DCMAKE_BUILD_TYPE=Release -DPORTALS_BUILD_TESTS=OFF -DPORTALS_BUILD_EXAMPLES=OFF -DPORTALS_BUILD_TOOLS=OFF
cmake --build build/package
cmake --install build/package --prefix "C:/sdk/Portals"
```

```cmake
cmake_minimum_required(VERSION 3.24)
project(MyApplication LANGUAGES CXX)
find_package(Portals 0.2 CONFIG REQUIRED)
add_executable(app main.cpp)
target_link_libraries(app PRIVATE portals::core)
```

```powershell
cmake -S my-application -B build/my-application -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="C:/sdk/Portals"
cmake --build build/my-application
```

Для переноса переместите весь каталог установки: заголовки, оба архива, CMake и лицензии.
Укажите новый CMAKE_PREFIX_PATH. Пути импортируемых целей CMake вычисляются относительно него.
Собственные CMAKE_INSTALL_LIBDIR/INCLUDEDIR оставляйте относительными.

## Совместимость

Архитектура, компилятор, стандартная библиотека, конфигурация C++ и среда выполнения
библиотеки, utf8proc и приложения должны совпадать. Debug/Release собираются
отдельно или через совместимый генератор с несколькими конфигурациями и устанавливаются в общий каталог установки.
Архивы разделены по lib/Debug и lib/Release; установка одного сохраняет другой.
SameMajorVersion — правило поиска CMake, без обещания стабильного ABI.
Переносимых BMI модулей пакет не содержит.

Проект сохраняет выбор среды выполнения MSVC вызывающего кода. Стандартный
MultiThreaded$<$<CONFIG:Debug>:Debug>DLL означает /MDd в Debug и /MD в Release.
Для /MT выбирайте среду выполнения явно и пересобирайте архивы. Release не означает
отсутствие распространяемых компонентов среды выполнения Visual C++.

## Проверка переноса

При PORTALS_BUILD_TESTS=ON:

```powershell
cmake --build build/windows-msvc-debug --target portals_core
ctest --test-dir build/windows-msvc-debug -R "^portals.package-install$" --output-on-failure --no-tests=error
```

Тест устанавливает текущую сборку, переносит её в путь с пробелами/Unicode
и собирает независимое приложение-потребитель только через импортируемые цели CMake.
Он публикует ревизию модели, готовит собственный виджет и проверяет отрисовку
без терминала. Windows использует исходник неинтерактивного запуска из тестового окружения.
Проверка запрещает включать пути исходников и сборки, внутренние параметры
предупреждений и определения
в установленных CMake-файлах, отключает реестры пакетов и сохраняет каталоги.
Дополнительно выполняются сценарии ввода и ошибок для managed-контролов.

Генератор с несколькими конфигурациями Debug/Release регистрирует
`portals.package-install-multi-config`:

```powershell
cmake -S . -B build/package-multi -G "Ninja Multi-Config" -DPORTALS_BUILD_TESTS=ON -DPORTALS_BUILD_EXAMPLES=OFF -DPORTALS_BUILD_TOOLS=OFF
ctest --test-dir build/package-multi -C Debug -R "^portals.package-install-multi-config$" --output-on-failure --no-tests=error
```

Он собирает/устанавливает обе конфигурации в общий каталог установки, переносит его
и запускает приложение-потребитель в обеих конфигурациях. Поддерживается и генератор Visual Studio.

## Архивы Release

Упаковка использует одну зафиксированную ревизию из свежего архива исходников,
без незакоммиченных и внутренних рабочих файлов. Из одной Release-сборки формируются
примеры, HTML и SDK, затем проверяется распакованный архив. Промежуточный каталог не
переиспользуется и существующий архив не перезаписывается. Задайте новые абсолютные
пути; промежуточный каталог должен находиться вне исходников.
Для полной справки нужны Doxygen 1.18+, Mermaid CLI; проверенная связка
Node.js 24 LTS + Mermaid CLI 12. Читателю достаточно упакованных SVG.

```powershell
./tools/package/bundle.ps1 -WorkDir "C:/temp/portals bundle run-001" -OutputArchive "C:/artifacts/portals-mvp-windows-x64-release.zip"
```

Параметры: -Revision — коммит, -Parallel — параллелизм, -PythonExecutable —
Python 3 для проверки HTML-ссылок. Без Python эта проверка помечается not_run.
Архив для Windows по умолчанию явно использует MultiThreaded (/MT);
-Runtime MultiThreadedDLL выбирает /MD и записывает выбор в manifest.
dumpbin /DEPENDENTS проверяет реальные программы; динамическая среда выполнения CRT при /MT
отклоняется. Системные DLL Windows не перераспространяются.

```sh
cmake "-DSOURCE_DIR=/absolute/portals" "-DWORK_DIR=/tmp/portals-bundle-run-001" "-DOUTPUT_ARCHIVE=/absolute/artifacts/portals-mvp-linux-x64-release.tar.gz" "-DCXX_COMPILER=/usr/bin/g++" -P tools/package/bundle.cmake
```

Linux записывает ldd каждой программы и версию ldd/libc системы сборки. Совместимость
со всеми дистрибутивами не обещается. Поддержаны сборки для x64 Windows/MSVC на собственной платформе
и Linux, по умолчанию Ninja с одной конфигурацией; кросс-компиляция не выполняется.

Архив содержит bin, themes, data, docs/html, install, исходники примеров,
независимое приложение-потребитель, файлы запуска, GETTING-STARTED.md, лицензии, проверки
и manifest.json. Примеры имеют встроенные данные и не требуют файлов текущего
каталога. HTML копируется вместе с ресурсами и уведомлениями лицензий.
Манифест хранит коммит, контрольную сумму архива исходников,
ОС/компилятор/среду выполнения, импортируемые зависимости
и SHA256 файлов. Сам manifest.json исключён из собственного списка; SHA256 архива
охватывает весь архив. Хеши выявляют повреждение без подтверждения подлинности.

Проверка доверенного архива:

```sh
cmake "-DARCHIVE=<absolute archive path>" "-DTEST_WORK_DIR=<new absolute temporary path>" "-DCXX_COMPILER=<compatible compiler>" -P tests/bundle_smoke.cmake
```

Проверяются инвентарь/хеши, запуск конечных --check из чужого каталога
и установленный потребитель; каталоги свидетельств сохраняются.
BUNDLE_DIR и скопированный verify/bundle_smoke.cmake проверяют перенесённую
распаковку; VERIFY_ONLY=ON проверяет хеши без сборки.
portals.bundle-scripts проверяет повреждения, лишние файлы, выход за пределы каталога по относительному пути
и отказ менять существующие промежуточный каталог и архив без C++-сборки или терминала.

Источники: [CMake import/export](https://cmake.org/cmake/help/latest/guide/importing-exporting/index.html),
[GNUInstallDirs](https://cmake.org/cmake/help/latest/module/GNUInstallDirs.html),
[package helpers](https://cmake.org/cmake/help/latest/module/CMakePackageConfigHelpers.html),
[MSVC runtime](https://cmake.org/cmake/help/latest/prop_tgt/MSVC_RUNTIME_LIBRARY.html),
[dumpbin](https://learn.microsoft.com/en-us/cpp/build/reference/dependents?view=msvc-170).
