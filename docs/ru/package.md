# Установка и подключение пакета CMake {#package-ru}

[Английская версия](../package.md)

Portals устанавливает статический SDK C++23: открытые заголовки, архивы core и
частной utf8proc, метаданные CMake и лицензии. Цель `portals::core` передаёт требование
к языку и полную цепочку линковки. Загрузка зависимостей по сети и системная utf8proc
не нужны. В UNIX конфигурация находит платформенную Threads.

## Совместное подключение заголовков Windows

Portals не требует заголовков Windows в коде приложения. Если приложение также
подключает `windows.h`, определите `NOMINMAX` до этого include. Объявления диалогов
сообщений и графических символов сохраняют макросы `MessageBox` и `UNICODE`
вызывающего кода. Переносимая ссылка на составной диалог — `ui::Ref<MessageDialog>`;
прежняя квалифицированная форма `ui::MessageBox` доступна в обоих символьных режимах Windows.

Прежнее явное написание `PrimitiveGlyphs::UNICODE` всё ещё конфликтует с активным
макросом Win32 `UNICODE` в вызывающем коде. Параметры символов по умолчанию уже
выбирают Unicode с документированным переходом к ASCII.

## Release с Ninja

На Windows используйте x64 Developer PowerShell MSVC. На Linux команды те же,
с подходящим набором инструментов C/C++ и путём установки.

```powershell
cmake -S . -B build/package-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DPORTALS_BUILD_TESTS=OFF -DPORTALS_BUILD_EXAMPLES=OFF -DPORTALS_BUILD_TOOLS=OFF
cmake --build build/package-release
cmake --install build/package-release --prefix "C:/sdk/Portals-release"
```

Поместите [полный пример приложения](../examples/guide_application.cpp) в
`my-application/main.cpp`, создайте `my-application/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.24)
project(MyApplication LANGUAGES CXX)
find_package(Portals 0.2 CONFIG REQUIRED)
add_executable(app main.cpp)
target_link_libraries(app PRIVATE portals::core)
```

```powershell
cmake -S my-application -B build/my-application-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="C:/sdk/Portals-release"
cmake --build build/my-application-release
```

Приложение и SDK здесь используют **Release**. `CMAKE_PREFIX_PATH` указывает
каталог установки, а не исходники и не каталог сборки библиотеки.

## Debug с Ninja

Ninja выбирает одну конфигурацию при настройке. Используйте отдельные каталоги
сборки SDK и приложения и одинаковый **Debug** с обеих сторон:

```powershell
cmake -S . -B build/package-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPORTALS_BUILD_TESTS=OFF -DPORTALS_BUILD_EXAMPLES=OFF -DPORTALS_BUILD_TOOLS=OFF
cmake --build build/package-debug
cmake --install build/package-debug --prefix "C:/sdk/Portals-debug"
cmake -S my-application -B build/my-application-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="C:/sdk/Portals-debug"
cmake --build build/my-application-debug
```

## Debug и Release в одной установке

С Ninja Multi-Config и Visual Studio конфигурация выбирается через `--config`
при сборке и установке. В этом варианте `CMAKE_BUILD_TYPE` не используется.

```powershell
cmake -S . -B build/package-multi -G "Ninja Multi-Config" -DPORTALS_BUILD_TESTS=OFF -DPORTALS_BUILD_EXAMPLES=OFF -DPORTALS_BUILD_TOOLS=OFF
cmake --build build/package-multi --config Debug
cmake --install build/package-multi --config Debug --prefix "C:/sdk/Portals"
cmake --build build/package-multi --config Release
cmake --install build/package-multi --config Release --prefix "C:/sdk/Portals"
cmake -S my-application -B build/my-application-multi -G "Ninja Multi-Config" -DCMAKE_PREFIX_PATH="C:/sdk/Portals"
cmake --build build/my-application-multi --config Debug
cmake --build build/my-application-multi --config Release
```

Архивы разделены по `lib/Debug` и `lib/Release` (внутри настроенного
`CMAKE_INSTALL_LIBDIR`). Установка одной конфигурации сохраняет другую. Наборы
инструментов должны быть совместимы; не смешивайте молча независимые SDK.

## Совместимость и перенос

Согласуйте архитектуру, компилятор, стандартную библиотеку, конфигурацию C++ и
runtime Portals, utf8proc и приложения. Стандартный runtime MSVC — `/MDd` в Debug
и `/MD` в Release. Portals сохраняет явно выбранный `CMAKE_MSVC_RUNTIME_LIBRARY`;
при его смене пересоберите архивы и приложение. Сам Release не означает `/MT`
и отсутствие требований к распространяемой среде выполнения.

Переносите **весь каталог установки**: заголовки, оба архива, CMake и лицензии.
Укажите новый `CMAKE_PREFIX_PATH`. Пути установленных целей относительны этому
каталогу. Собственные `CMAKE_INSTALL_LIBDIR` и `CMAKE_INSTALL_INCLUDEDIR` оставляйте
относительными. `SameMajorVersion` — правило поиска CMake, без обещания стабильного
ABI и универсальной бинарной переносимости. Переносимые BMI модулей не поставляются.
Источник версии метаданных — верхнеуровневый `project(Portals VERSION ...)`.

Необязательная цель `portals::lyra` добавляет `<portals/ui_lyra.hpp>` и закреплённые
заголовки Lyra для [командной строки](cli.md). Лицензия и манифест исходников входят
в SDK. Без адаптера подключайте `portals::core`.

## Запускаемые архивы Release

Архив содержит примеры, HTML-справку, статический SDK, исходники примеров,
темы/данные, скрипты запуска, лицензии и манифест. Windows-сборка архива по умолчанию
явно выбирает `/MT`; это не меняет стандартный runtime обычного SDK выше.
Приложению с SDK из архива нужен совместимый набор инструментов и runtime.
Linux-архив сохраняет требования совместимости libc и инструментов своего хоста.

<details>
<summary>Создание архива Release из закоммиченной ревизии</summary>

Pipeline строит одну ревизию Git из нового архива исходников; незакоммиченные
и игнорируемые файлы исключены. Выберите новые абсолютные пути staging и архива;
staging должен быть вне checkout. Существующие пути не перезаписываются.

На Windows нужны x64 Developer PowerShell MSVC, Git, CMake 3.24+, Ninja,
Doxygen 1.18+, Node.js и Mermaid CLI в PATH. Проверенная связка справки —
Doxygen 1.18, Node.js 24 LTS и Mermaid CLI 12. Обычному SDK генераторы справки не нужны.

```powershell
./tools/package/bundle.ps1 -WorkDir "C:/temp/portals bundle run-001" -OutputArchive "C:/artifacts/portals-mvp-windows-x64-release.zip"
```

`-Revision` выбирает закоммиченную ревизию, `-Parallel` — параллелизм сборки.
`-Runtime MultiThreadedDLL` явно выбирает `/MD` вместо `/MT` архива.
`-PythonExecutable` включает необязательную проверку локальных HTML-ссылок и якорей;
без Python в манифесте записывается `not_run`. Системные DLL Windows не поставляются.

На Linux:

```sh
cmake "-DSOURCE_DIR=/absolute/portals" "-DWORK_DIR=/tmp/portals-bundle-run-001" "-DOUTPUT_ARCHIVE=/absolute/artifacts/portals-mvp-linux-x64-release.tar.gz" "-DCXX_COMPILER=/usr/bin/g++" -P tools/package/bundle.cmake
```

Поддерживаются нативные x64 Windows/MSVC и Linux, по умолчанию single-config Ninja.
Примеры, HTML и SDK собираются из одной Release-ревизии; новый распакованный архив
проверяется до публикации. Кросс-компиляция не выполняется.

Манифест содержит ревизию/checksum исходников, компилятор/runtime, импорты executable
и SHA256 файлов. Сам манифест исключён из собственного списка; архив имеет отдельную
контрольную сумму. Они обнаруживают случайное повреждение, но не подтверждают
подлинность. Темы и replay-данные служат справочными входами; значения по умолчанию
встроены в примеры, файлы данных в текущем каталоге им не нужны.

Для проверки доверенного архива из нового места используйте новый тестовый каталог:

```powershell
cmake "-DARCHIVE=<absolute archive path>" "-DTEST_WORK_DIR=<new absolute temporary path>" "-DCXX_COMPILER=<compatible compiler>" -P tests/bundle_smoke.cmake
```

Проверяются состав, конечные сценарии примеров и потребитель перенесённого SDK;
каталоги доказательств сохраняются. Это не заменяет интерактивную проверку терминала.
`VERIFY_ONLY=ON` проверяет хеши без сборки.

</details>

Источники: [импорт/экспорт CMake](https://cmake.org/cmake/help/latest/guide/importing-exporting/index.html),
[конфигурации сборки](https://cmake.org/cmake/help/latest/manual/cmake-buildsystem.7.html#build-configurations),
[GNUInstallDirs](https://cmake.org/cmake/help/latest/module/GNUInstallDirs.html),
[runtime MSVC](https://cmake.org/cmake/help/latest/prop_tgt/MSVC_RUNTIME_LIBRARY.html).
