# Install and use the CMake package {#package}

[Русская версия](ru/package.md)

Portals installs a C++23 static SDK: public headers, core and private utf8proc
archives, CMake package metadata and license notices. Link `portals::core` to
receive the language requirement and complete static link chain. No network
download or system utf8proc installation is needed. On UNIX the package finds
its platform Threads dependency.

## Release with Ninja

Run from an x64 MSVC developer PowerShell on Windows. On Linux, use the same
commands with an appropriate C/C++ toolchain and installation path.

```powershell
cmake -S . -B build/package-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DPORTALS_BUILD_TESTS=OFF -DPORTALS_BUILD_EXAMPLES=OFF -DPORTALS_BUILD_TOOLS=OFF
cmake --build build/package-release
cmake --install build/package-release --prefix "C:/sdk/Portals-release"
```

Place the [complete application example](examples/guide_application.cpp) in
`my-application/main.cpp`, then create `my-application/CMakeLists.txt`:

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

The application and SDK above both use **Release**. `CMAKE_PREFIX_PATH` points
to the installation prefix, not the source tree or its build directory.

## Debug with Ninja

Ninja selects one configuration when configured. Use separate SDK and application
build directories and match **Debug** on both sides:

```powershell
cmake -S . -B build/package-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPORTALS_BUILD_TESTS=OFF -DPORTALS_BUILD_EXAMPLES=OFF -DPORTALS_BUILD_TOOLS=OFF
cmake --build build/package-debug
cmake --install build/package-debug --prefix "C:/sdk/Portals-debug"
cmake -S my-application -B build/my-application-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="C:/sdk/Portals-debug"
cmake --build build/my-application-debug
```

## Debug and Release in one installation

With Ninja Multi-Config or Visual Studio, select the configuration at build and
install time using `--config`. Do not use `CMAKE_BUILD_TYPE` for this workflow.

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

Archives are stored separately in `lib/Debug` and `lib/Release` (under your
configured `CMAKE_INSTALL_LIBDIR`). Installing one preserves the other. Both
configurations must use compatible toolchains; do not silently mix independent SDKs.

## Compatibility and relocation

Match target architecture, compiler, standard library, C++ configuration and runtime
across Portals, utf8proc and the application. The default MSVC runtime is `/MDd`
in Debug and `/MD` in Release. Portals preserves an explicitly selected
`CMAKE_MSVC_RUNTIME_LIBRARY`; rebuild all archives and the consumer if changing it.
Release alone does not imply `/MT` or removal of runtime redistributable requirements.

Move the **entire installation prefix**, including headers, both archives,
CMake files and notices; point `CMAKE_PREFIX_PATH` to its new location. Installed
target paths are relative to the prefix. Keep custom `CMAKE_INSTALL_LIBDIR` and
`CMAKE_INSTALL_INCLUDEDIR` relative. `SameMajorVersion` is a CMake discovery rule,
with no stable ABI or universal binary portability guarantee. Portable module
BMI files are not supplied. The top-level `project(Portals VERSION ...)` declaration
is the version source for package metadata.

The optional `portals::lyra` target adds `<portals/ui_lyra.hpp>` and pinned Lyra
headers for [command-line startup](cli.md). Its license and source manifest ship
with the SDK. Core-only consumers link `portals::core`.

## Runnable Release archives

A Release bundle includes example executables, HTML reference, the static SDK,
example source, themes/data, launchers, notices and a manifest. Its default Windows
bundle runtime is `/MT`, selected by that packaging workflow; this does not change
the SDK build default above. Use a compatible toolchain/runtime when building
applications against the SDK inside a bundle. Linux bundles retain their host
libc/toolchain compatibility requirements.

<details>
<summary>Build a Release bundle from a committed revision</summary>

The pipeline builds one committed Git revision from a fresh source archive;
uncommitted and ignored files are excluded. Choose new absolute staging/output
paths; staging must be outside the checkout. Existing paths are never overwritten.

On Windows, use an x64 MSVC developer PowerShell with Git, CMake 3.24+, Ninja,
Doxygen 1.18+, Node.js and Mermaid CLI on PATH. Doxygen 1.18, Node.js 24 LTS and
Mermaid CLI 12 are the verified documentation toolchain; ordinary SDK builds do
not need documentation generators.

```powershell
./tools/package/bundle.ps1 -WorkDir "C:/temp/portals bundle run-001" -OutputArchive "C:/artifacts/portals-mvp-windows-x64-release.zip"
```

`-Revision` selects a committed revision; `-Parallel` controls build parallelism.
`-Runtime MultiThreadedDLL` explicitly selects `/MD` instead of the bundle's `/MT`.
`-PythonExecutable` enables the optional HTML local-link/anchor gate; without Python
its manifest status is `not_run`. System Windows DLLs are not redistributed.

On Linux:

```sh
cmake "-DSOURCE_DIR=/absolute/portals" "-DWORK_DIR=/tmp/portals-bundle-run-001" "-DOUTPUT_ARCHIVE=/absolute/artifacts/portals-mvp-linux-x64-release.tar.gz" "-DCXX_COMPILER=/usr/bin/g++" -P tools/package/bundle.cmake
```

The pipeline supports native x64 Windows/MSVC and Linux, with single-config Ninja
by default. It builds examples, HTML and SDK from the same Release revision and
checks a freshly unpacked archive before publication. No cross compilation is done.

The manifest records the source revision/checksum, compiler/runtime tuple,
executable imports and SHA256 payload inventory. It excludes itself from its own
inventory; the archive has a separate checksum. Checksums detect accidental
corruption, not authenticity. Themes/replay inputs are reference material; example
binaries embed defaults and do not require current-directory data files.

To inspect a trusted archive from a new location, use a fresh test directory:

```powershell
cmake "-DARCHIVE=<absolute archive path>" "-DTEST_WORK_DIR=<new absolute temporary path>" "-DCXX_COMPILER=<compatible compiler>" -P tests/bundle_smoke.cmake
```

This checks the inventory, finite example scenarios and relocated SDK consumer;
its evidence directories are retained. It does not substitute for interactive
terminal testing. `VERIFY_ONLY=ON` checks hashes without building.

</details>

References: [CMake importing/exporting](https://cmake.org/cmake/help/latest/guide/importing-exporting/index.html),
[build configurations](https://cmake.org/cmake/help/latest/manual/cmake-buildsystem.7.html#build-configurations),
[GNUInstallDirs](https://cmake.org/cmake/help/latest/module/GNUInstallDirs.html),
[MSVC runtime selection](https://cmake.org/cmake/help/latest/prop_tgt/MSVC_RUNTIME_LIBRARY.html).
