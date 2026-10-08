# Installed CMake package

Portals installs its C++23 public headers, static core archive, private utf8proc
archive, CMake configuration/version/targets files, and license notices. No
network download or system utf8proc installation is required. The imported
`portals::core` target carries the C++23 requirement and the complete static link
chain; on UNIX the package configuration finds the platform Threads dependency.

Build and install from an x64 MSVC developer PowerShell, or use the same CMake
commands with an appropriate C/C++ toolchain on Linux:

```powershell
cmake -S . -B build/package -G Ninja -DCMAKE_BUILD_TYPE=Release -DPORTALS_BUILD_TESTS=OFF -DPORTALS_BUILD_EXAMPLES=OFF -DPORTALS_BUILD_TOOLS=OFF
cmake --build build/package
cmake --install build/package --prefix "C:/sdk/Portals"
```

An external project uses only the installed prefix:

```cmake
cmake_minimum_required(VERSION 3.24)
project(MyApplication LANGUAGES CXX)
find_package(Portals 0.1 CONFIG REQUIRED)
add_executable(app main.cpp)
target_link_libraries(app PRIVATE portals::core)
```

```powershell
cmake -S my-application -B build/my-application -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="C:/sdk/Portals"
cmake --build build/my-application
```

Move the entire installation directory, including headers, both archives,
CMake metadata and notices, and pass its new location in `CMAKE_PREFIX_PATH`.
The installed targets resolve paths relative to that location. Keep install
directories such as `CMAKE_INSTALL_LIBDIR` and `CMAKE_INSTALL_INCLUDEDIR` relative
when selecting a custom layout with GNUInstallDirs.

## Toolchain and configuration compatibility

The package is a static-library/header distribution. Use a compatible target
architecture, compiler, standard library, C++ configuration and runtime for
Portals, utf8proc and the application. Build/install Debug and Release separately,
or install both configurations into one prefix with a compatible multi-config
build. Archives are stored separately under `lib/Debug` and `lib/Release`
(or the corresponding configured `CMAKE_INSTALL_LIBDIR`), so installing one
configuration preserves the other. CMake's `SameMajorVersion` package-version selection is a discovery rule;
it does not promise a stable ABI or binary portability. This package supplies no
portable module BMI files.

Portals preserves the caller's MSVC runtime selection. CMake's default
`MultiThreaded$<$<CONFIG:Debug>:Debug>DLL` means `/MDd` for Debug and `/MD` for
Release unless the caller explicitly selects another runtime. Match that choice
when building the consumer; never infer `/MT` or absence of Visual C++ runtime
redistributables from the word Release. Rebuild the archives when changing the
runtime or other incompatible parts of the build tuple.

## Relocation test

With `PORTALS_BUILD_TESTS=ON`, build `portals_core` and run:

```powershell
cmake --build build/windows-msvc-debug --target portals_core
ctest --test-dir build/windows-msvc-debug -R "^portals.package-install$" --output-on-failure --no-tests=error
```

The test installs the current build into a fresh directory, moves that directory
to a path containing spaces and Unicode, and copies a small independent consumer
into its own source/build directories. The consumer links imported targets only,
publishes a new immutable model revision, prepares a custom widget and verifies
its rendered total. It does not open a terminal. Windows test processes use the
existing noninteractive startup source copied beside the consumer fixture.
The test rejects source/build paths and private warnings/definitions in installed
CMake files, disables package registries, and retains its directories for inspection.

Multi-config generators that provide Debug and Release also register
`portals.package-install-multi-config`. This test builds both core/utf8proc
configurations, installs them into one prefix, moves it, then builds and runs
the external consumer in both configurations:

```powershell
cmake -S . -B build/package-multi -G "Ninja Multi-Config" -DPORTALS_BUILD_TESTS=ON -DPORTALS_BUILD_EXAMPLES=OFF -DPORTALS_BUILD_TOOLS=OFF
ctest --test-dir build/package-multi -C Debug -R "^portals.package-install-multi-config$" --output-on-failure --no-tests=error
```

The same test is available with a Visual Studio multi-config generator.

References: [CMake importing/exporting](https://cmake.org/cmake/help/latest/guide/importing-exporting/index.html),
[GNUInstallDirs](https://cmake.org/cmake/help/latest/module/GNUInstallDirs.html),
[package configuration helpers](https://cmake.org/cmake/help/latest/module/CMakePackageConfigHelpers.html),
[MSVC runtime selection](https://cmake.org/cmake/help/latest/prop_tgt/MSVC_RUNTIME_LIBRARY.html).
